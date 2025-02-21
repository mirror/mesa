/*
 * Copyright © 2023 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "aco_nir_call_attribs.h"
#include "nir_builder.h"
#include "radv_nir.h"

void
radv_nir_lower_callee_signature(nir_function *function, struct set *visited_funcs)
{
   if (visited_funcs) {
      if (_mesa_set_search(visited_funcs, function))
         return;
      _mesa_set_add(visited_funcs, function);
   }

   nir_parameter *old_params = function->params;
   unsigned old_num_params = function->num_params;

   function->num_params += 2;
   function->params = rzalloc_array_size(function->shader, function->num_params, sizeof(nir_parameter));

   memcpy(function->params + 2, old_params, old_num_params * sizeof(nir_parameter));

   function->params[0].num_components = 1;
   function->params[0].bit_size = 64;
   function->params[1].num_components = 1;
   function->params[1].bit_size = 64;
   function->params[1].is_uniform = true;

   nir_function_impl *impl = function->impl;

   if (!impl)
      return;

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

         if (intr->intrinsic == nir_intrinsic_load_param)
            nir_intrinsic_set_param_idx(intr, nir_intrinsic_param_idx(intr) + 2);
      }
   }
}

/* Checks if caller can call callee using tail calls.
 *
 * If the ABIs mismatch, we might need to insert move instructions to move return values from callee return registers to
 * caller return registers after the call. In that case, tail-calls are impossible to do correctly.
 */
static bool
is_tail_call_compatible(nir_function *caller, nir_function *callee)
{
   /* If the caller doesn't return at all, we don't need to care if return params are compatible */
   if (caller->driver_attributes & ACO_NIR_FUNCTION_ATTRIB_NORETURN)
      return true;
   /* The same ABI can't mismatch */
   if ((caller->driver_attributes & ACO_NIR_FUNCTION_ATTRIB_ABI_MASK) ==
       (callee->driver_attributes & ACO_NIR_FUNCTION_ATTRIB_ABI_MASK))
      return true;
   /* The recursive shader ABI and the traversal shader ABI are built so that return parameters occupy exactly
    * the same registers, to allow tail calls from the traversal shader. */
   if ((caller->driver_attributes & ACO_NIR_FUNCTION_ATTRIB_ABI_MASK) == ACO_NIR_CALL_ABI_TRAVERSAL &&
       (callee->driver_attributes & ACO_NIR_FUNCTION_ATTRIB_ABI_MASK) == ACO_NIR_CALL_ABI_RT_RECURSIVE)
      return true;
   return false;
}

static void
gather_tail_call_instrs_block(nir_function *caller, const struct nir_block *block, struct set *tail_calls)
{
   nir_foreach_instr_reverse (instr, block) {
      switch (instr->type) {
      case nir_instr_type_phi:
      case nir_instr_type_undef:
      case nir_instr_type_load_const:
         continue;
      case nir_instr_type_alu:
         if (!nir_op_is_vec_or_mov(nir_instr_as_alu(instr)->op))
            return;
         continue;
      case nir_instr_type_call: {
         nir_call_instr *call = nir_instr_as_call(instr);

         if (!is_tail_call_compatible(caller, call->callee))
            return;

         for (unsigned i = 0; i < call->num_params; ++i) {
            if (call->callee->params[i].is_return != caller->params[i].is_return)
               return;
            /* We can only do tail calls if the caller returns exactly the callee return values */
            if (caller->params[i].is_return) {
               assert(call->params[i].ssa->parent_instr->type == nir_instr_type_deref);
               nir_deref_instr *deref_root = nir_instr_as_deref(call->params[i].ssa->parent_instr);
               while (nir_deref_instr_parent(deref_root))
                  deref_root = nir_deref_instr_parent(deref_root);

               if (!deref_root->parent.ssa)
                  return;
               if (deref_root->parent.ssa->parent_instr->type != nir_instr_type_intrinsic)
                  return;
               nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(deref_root->parent.ssa->parent_instr);
               if (intrin->intrinsic != nir_intrinsic_load_param)
                  return;
               if (nir_intrinsic_param_idx(intrin) != i)
                  return;
            }
            if (call->callee->params[i].is_uniform != caller->params[i].is_uniform)
               return;
            if (call->callee->params[i].bit_size != caller->params[i].bit_size)
               return;
            if (call->callee->params[i].num_components != caller->params[i].num_components)
               return;
         }

         _mesa_set_add(tail_calls, instr);
         continue;
      }
      default:
         return;
      }
   }

   set_foreach (block->predecessors, pred) {
      gather_tail_call_instrs_block(caller, pred->key, tail_calls);
   }
}

struct lower_param_info {
   /*  */
   nir_def *load_param_def;

   nir_def *return_deref;
   bool has_store;
};

static void
check_param_uses_for_stores(nir_deref_instr *instr, struct lower_param_info *info)
{
   nir_foreach_use (deref_use, &instr->def) {
      nir_instr *use_instr = nir_src_parent_instr(deref_use);
      if (use_instr->type == nir_instr_type_deref)
         check_param_uses_for_stores(nir_instr_as_deref(use_instr), info);
      else if ((use_instr->type == nir_instr_type_intrinsic &&
                nir_instr_as_intrinsic(use_instr)->intrinsic == nir_intrinsic_store_deref) ||
               use_instr->type == nir_instr_type_call)
         info->has_store = true;
   }
}

static void
rewrite_return_param_uses(nir_intrinsic_instr *intr, unsigned param_idx, struct lower_param_info *param_defs)
{
   nir_foreach_use_safe (use, &intr->def) {
      nir_instr *use_instr = nir_src_parent_instr(use);
      assert(use_instr && use_instr->type == nir_instr_type_deref &&
             nir_instr_as_deref(use_instr)->deref_type == nir_deref_type_cast);
      check_param_uses_for_stores(nir_instr_as_deref(use_instr), &param_defs[param_idx]);
      nir_def_rewrite_uses(&nir_instr_as_deref(use_instr)->def, param_defs[param_idx].return_deref);

      nir_instr_remove(use_instr);
   }
}

static void
lower_call_abi_for_callee(nir_function *function, unsigned wave_size, struct set *visited_funcs)
{
   nir_function_impl *impl = function->impl;

   nir_builder b = nir_builder_create(impl);
   b.cursor = nir_before_impl(impl);

   nir_variable *tail_call_pc =
      nir_variable_create(b.shader, nir_var_shader_temp, glsl_uint64_t_type(), "_tail_call_pc");

   struct set *tail_call_instrs = _mesa_set_create(b.shader, _mesa_hash_pointer, _mesa_key_pointer_equal);
   gather_tail_call_instrs_block(function, nir_impl_last_block(impl), tail_call_instrs);

   radv_nir_lower_callee_signature(function, visited_funcs);

   /* guard the shader, so that only the correct invocations execute it */

   nir_def *guard_condition = NULL;
   nir_def *shader_addr;
   nir_def *uniform_shader_addr;
   if (function->driver_attributes & ACO_NIR_FUNCTION_ATTRIB_DIVERGENT_CALL) {
      nir_cf_list list;
      nir_cf_extract(&list, nir_before_impl(impl), nir_after_impl(impl));

      b.cursor = nir_before_impl(impl);

      shader_addr = nir_load_param(&b, 0);
      uniform_shader_addr = nir_load_param(&b, 1);

      nir_store_var(&b, tail_call_pc, shader_addr, 0x1);

      guard_condition = nir_ieq(&b, uniform_shader_addr, shader_addr);
      nir_if *shader_guard = nir_push_if(&b, guard_condition);
      shader_guard->control = nir_selection_control_divergent_always_taken;
      nir_store_var(&b, tail_call_pc, nir_imm_int64(&b, 0), 0x1);
      nir_cf_reinsert(&list, b.cursor);
      nir_pop_if(&b, shader_guard);
   } else {
      shader_addr = nir_load_param(&b, 0);
      nir_store_var(&b, tail_call_pc, nir_imm_int64(&b, 0), 0x1);
   }

   b.cursor = nir_before_impl(impl);
   struct lower_param_info *param_infos = ralloc_size(b.shader, function->num_params * sizeof(struct lower_param_info));
   nir_variable **param_vars = ralloc_size(b.shader, function->num_params * sizeof(nir_variable *));

   for (unsigned i = 2; i < function->num_params; ++i) {
      param_vars[i] = nir_local_variable_create(impl, function->params[i].type, "_param");
      unsigned num_components = glsl_get_vector_elements(function->params[i].type);

      if (function->params[i].is_return) {
         assert(!glsl_type_is_array(function->params[i].type) && !glsl_type_is_struct(function->params[i].type));

         function->params[i].bit_size = glsl_get_bit_size(function->params[i].type);
         function->params[i].num_components = num_components;

         param_infos[i].return_deref = &nir_build_deref_var(&b, param_vars[i])->def;
      } else {
         param_infos[i].return_deref = NULL;
      }

      param_infos[i].has_store = false;
      param_infos[i].load_param_def = nir_load_param(&b, i);
      nir_store_var(&b, param_vars[i], param_infos[i].load_param_def, (0x1 << num_components) - 1);
   }

   unsigned max_tail_call_param = 0;

   nir_foreach_block (block, impl) {
      bool progress;
      do {
         progress = false;
         nir_foreach_instr_safe (instr, block) {
            if (instr->type == nir_instr_type_call && _mesa_set_search(tail_call_instrs, instr)) {
               nir_call_instr *call = nir_instr_as_call(instr);
               b.cursor = nir_before_instr(instr);

               for (unsigned i = 0; i < call->num_params; ++i) {
                  if (call->callee->params[i].is_return)
                     nir_store_var(&b, param_vars[i + 2],
                                   nir_load_deref(&b, nir_instr_as_deref(call->params[i].ssa->parent_instr)),
                                   (0x1 << glsl_get_vector_elements(call->callee->params[i].type)) - 1);
                  else
                     nir_store_var(&b, param_vars[i + 2], call->params[i].ssa,
                                   (0x1 << call->params[i].ssa->num_components) - 1);
                  param_infos[i + 2].has_store = true;
               }

               nir_store_var(&b, tail_call_pc, call->indirect_callee.ssa, 0x1);
               max_tail_call_param = MAX2(max_tail_call_param, call->num_params + 2);

               nir_instr_remove(instr);

               progress = true;
               break;
            }

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_param) {
               unsigned param_idx = nir_intrinsic_param_idx(intr);

               if (param_idx >= 2 && &intr->def != param_infos[param_idx].load_param_def) {
                  if (function->params[param_idx].is_return)
                     rewrite_return_param_uses(intr, param_idx, param_infos);
                  else
                     nir_def_rewrite_uses(&intr->def, param_infos[param_idx].load_param_def);
                  nir_instr_remove(instr);
                  progress = true;
                  break;
               }
            }
         }
      } while (progress);
   }

   b.cursor = nir_after_impl(impl);

   for (unsigned i = 2; i < function->num_params; ++i) {
      if (param_infos[i].has_store)
         nir_store_param_amd(&b, nir_load_var(&b, param_vars[i]), .param_idx = i);
   }

   if (guard_condition)
      shader_addr = nir_bcsel(&b, guard_condition, nir_load_var(&b, tail_call_pc), shader_addr);
   else
      shader_addr = nir_load_var(&b, tail_call_pc);
   nir_def *ballot = nir_ballot(&b, 1, wave_size, nir_ine_imm(&b, shader_addr, 0));
   nir_def *ballot_addr = nir_read_invocation(&b, shader_addr, nir_find_lsb(&b, ballot));
   uniform_shader_addr = nir_bcsel(&b, nir_ieq_imm(&b, ballot, 0), nir_load_call_return_address_amd(&b), ballot_addr);

   if (!(function->driver_attributes & ACO_NIR_FUNCTION_ATTRIB_NORETURN)) {
      nir_push_if(&b, nir_ieq_imm(&b, uniform_shader_addr, 0));
      nir_terminate(&b);
      nir_pop_if(&b, NULL);

      nir_set_next_call_pc_amd(&b, shader_addr, uniform_shader_addr);
   }
}

static void
lower_call_abi_for_call(nir_builder *b, nir_call_instr *call, unsigned *cur_call_idx, struct set *visited_funcs,
                        struct set *visited_calls)
{
   unsigned call_idx = (*cur_call_idx)++;

   for (unsigned i = 0; i < call->num_params; ++i) {
      unsigned callee_param_idx = i;
      if (_mesa_set_search(visited_funcs, call->callee))
         callee_param_idx += 2;

      if (!call->callee->params[callee_param_idx].is_return)
         continue;

      b->cursor = nir_before_instr(&call->instr);

      nir_src *old_src = &call->params[i];

      assert(old_src->ssa->parent_instr->type == nir_instr_type_deref);
      nir_deref_instr *param_deref = nir_instr_as_deref(old_src->ssa->parent_instr);
      assert(param_deref->deref_type == nir_deref_type_var);

      nir_src_rewrite(old_src, nir_load_deref(b, param_deref));

      b->cursor = nir_after_instr(&call->instr);

      unsigned num_components = glsl_get_vector_elements(param_deref->type);

      nir_store_deref(
         b, param_deref,
         nir_load_return_param_amd(b, num_components, glsl_base_type_get_bit_size(param_deref->type->base_type),
                                   .call_idx = call_idx, .param_idx = i + 2),
         (1u << num_components) - 1);

      assert(call->callee->params[callee_param_idx].bit_size == glsl_get_bit_size(param_deref->type));
      assert(call->callee->params[callee_param_idx].num_components == num_components);
   }

   radv_nir_lower_callee_signature(call->callee, visited_funcs);

   b->cursor = nir_after_instr(&call->instr);

   nir_call_instr *new_call = nir_call_instr_create(b->shader, call->callee);
   new_call->indirect_callee = nir_src_for_ssa(call->indirect_callee.ssa);
   new_call->params[0] = nir_src_for_ssa(call->indirect_callee.ssa);
   new_call->params[1] = nir_src_for_ssa(nir_read_first_invocation(b, call->indirect_callee.ssa));
   for (unsigned i = 2; i < new_call->num_params; ++i)
      new_call->params[i] = nir_src_for_ssa(call->params[i - 2].ssa);

   nir_builder_instr_insert(b, &new_call->instr);
   b->cursor = nir_after_instr(&new_call->instr);
   _mesa_set_add(visited_calls, new_call);

   nir_instr_remove(&call->instr);
}

static bool
lower_call_abi_for_caller(nir_function_impl *impl, struct set *visited_funcs)
{
   bool progress = false;
   unsigned cur_call_idx = 0;
   struct set *visited_calls = _mesa_set_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_call)
            continue;
         nir_call_instr *call = nir_instr_as_call(instr);
         if (call->callee->impl)
            continue;
         if (_mesa_set_search(visited_calls, call))
            continue;

         nir_builder b = nir_builder_create(impl);
         lower_call_abi_for_call(&b, call, &cur_call_idx, visited_funcs, visited_calls);
         progress = true;
      }
   }

   _mesa_set_destroy(visited_calls, NULL);

   return progress;
}

bool
radv_nir_lower_call_abi(nir_shader *shader, unsigned wave_size)
{
   struct set *visited_funcs = _mesa_set_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);

   bool progress = false;
   nir_foreach_function_with_impl (function, impl, shader) {
      bool func_progress = false;
      if (function->is_exported) {
         lower_call_abi_for_callee(function, wave_size, visited_funcs);
         func_progress = true;
      }
      func_progress |= lower_call_abi_for_caller(impl, visited_funcs);

      if (func_progress)
         nir_metadata_preserve(impl, nir_metadata_none);
      progress |= func_progress;
   }

   _mesa_set_destroy(visited_funcs, NULL);

   return progress;
}
