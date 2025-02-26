/*
 * Copyright © 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

/* For each output slot, gather which input components are used to compute it.
 * Component-wise ALU instructions must be scalar.
 */

#include "nir_builder.h"
#include "util/hash_table.h"
#include "util/u_memory.h"

struct gather_state {
   nir_outputs_deps *all;
   void *mem_ctx;
   struct hash_table *ht;
   nir_output_deps *prev;
   unsigned prev_components_read;
   unsigned depth;
};

static void
print_output_info(nir_output_deps *deps, FILE *f)
{
   unsigned i;

   BITSET_FOREACH_SET(i, deps->inputs, NUM_TOTAL_VARYING_SLOTS * 8) {
      fprintf(f, " %u.%c%s", i / 8, "xyzw"[(i % 8) / 2], i % 2 ? ".hi" : "");
   }
   fprintf(f, "%s%s%s", deps->uses_output_load ? " (output_load)" : "",
           deps->uses_ssbo_reads ? " (ssbo read)" : "",
           deps->uses_image_reads ? " (image read)" : "");
}

/* For debugging. */
static void
print_progress(struct gather_state *state, nir_instr *instr, bool enter)
{
#if 0
   if (!enter)
      state->depth--;

   printf("%*s%s", state->depth, "", enter ? "-->" : "<--");
   nir_print_instr(instr, stdout);
   printf(" =");
   print_output_info(state->prev, stdout);
   puts("");

   if (enter)
      state->depth++;
#endif
}

void
nir_print_output_deps(nir_outputs_deps *deps, nir_shader *nir, FILE *f)
{
   for (unsigned i = 0; i < deps->num_locations; i++) {
      fprintf(f, "%s(->%s): %s =",
              _mesa_shader_stage_to_abbrev(nir->info.stage),
              _mesa_shader_stage_to_abbrev(nir->info.next_stage),
              gl_varying_slot_name_for_stage(deps->locations[i],
                                             nir->info.stage));

      print_output_info(&deps->output[i], f);
      fprintf(f, "\n");
   }
}

static bool
gather_dependencies(nir_src *src, void *opaque) //nir_def *ssa, unsigned location, unsigned num_slots)
{
   nir_instr *instr = src->ssa->parent_instr;

   if (instr->type == nir_instr_type_load_const ||
       instr->type == nir_instr_type_undef)
      return true;

   /* Don't re-enter visited phis to prevent infinite recursion. */
   if (instr->type == nir_instr_type_phi) {
      if (instr->pass_flags)
         return true;

      instr->pass_flags = 1;
   }

   struct gather_state *state = (struct gather_state *)opaque;
   struct hash_entry *entry = _mesa_hash_table_search(state->ht, instr);

   /* Save the caller. */
   nir_output_deps *cur;

   if (entry) {
      cur = (nir_output_deps *)entry->data;
   } else {
      /* If we haven't visited this instruction yet, gather dependencies from
       * its sources.
       */
      cur = rzalloc_size(state->mem_ctx, sizeof(*cur));

      /* Gather the current instruction. */
      if (instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

         switch (intr->intrinsic) {
         case nir_intrinsic_load_input:
         case nir_intrinsic_load_input_vertex:
         case nir_intrinsic_load_per_vertex_input:
         case nir_intrinsic_load_per_primitive_input:
         case nir_intrinsic_load_interpolated_input: {
            nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
            assert(sem.num_slots >= 1);

            for (unsigned i = 0; i < sem.num_slots; i++) {
               u_foreach_bit(c, state->prev_components_read &
                                BITFIELD_MASK(intr->def.num_components)) {
                  unsigned bit = (sem.location + i) * 8 +
                                 (nir_intrinsic_component(intr) + c) * 2 +
                                 sem.high_16bits;
                  BITSET_SET(cur->inputs, bit);
               }
            }
            break;
         }
         case nir_intrinsic_load_output:
         case nir_intrinsic_load_per_vertex_output:
            cur->uses_output_load = true;
            break;

         default: {
            const char *name = nir_intrinsic_infos[intr->intrinsic].name;

            if (strstr(name, "load_ssbo") || strstr(name, "ssbo_atomic"))
               cur->uses_ssbo_reads = true;

            if (strstr(name, "image") &&
                (strstr(name, "load") || strstr(name, "atomic")))
               cur->uses_image_reads = true;
            break;
         }
         }
      } else if (instr->type == nir_instr_type_tex) {
         if (!nir_tex_instr_is_query(nir_instr_as_tex(instr)))
            cur->uses_image_reads = true;
      }

      /* Save parameters because we are going to overwrite them. */
      nir_output_deps *saved_prev = state->prev;
      unsigned saved_prev_components_read = state->prev_components_read;

      /* Gather srcs of the current instruction recursively. */
      state->prev = cur;

      print_progress(state, instr, true);

      switch (instr->type) {
      case nir_instr_type_alu: {
         nir_alu_instr *alu = nir_instr_as_alu(instr);

         if (nir_op_is_vec(alu->op)) {
            u_foreach_bit(i, saved_prev_components_read &
                             BITFIELD_MASK(alu->def.num_components)) {
               state->prev_components_read =
                  BITFIELD_BIT(alu->src[i].swizzle[0]);
               gather_dependencies(&alu->src[i].src, state);
            }
         } else if (nir_op_infos[alu->op].output_size) {
            /* Not a component-wise ALU instruction (like fdot). */
            for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
               /* Compute the mask of read components of the src.
                * Assume all components used by swizzle contribute to the result.
                */
               state->prev_components_read = 0;
               for (unsigned c = 0; c < alu->src[i].src.ssa->num_components; c++) {
                  state->prev_components_read |=
                     BITFIELD_BIT(alu->src[i].swizzle[c]);
               }

               gather_dependencies(&alu->src[i].src, state);
            }
         } else {
            assert(alu->def.num_components == 1);

            /* Component-wise ALU instruction. */
            for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
               state->prev_components_read =
                  BITFIELD_BIT(alu->src[i].swizzle[0]);
               gather_dependencies(&alu->src[i].src, state);
            }
         }
         break;
      }
      default:
         /* TODO: Component-wise intrinsics: Skip components not used by def. */
         /* Assume all src components contribute to the result. */
         state->prev_components_read = ~0;
         nir_foreach_src(instr, gather_dependencies, state);
         break;
      }

      print_progress(state, instr, false);

      /* Restore the state because we may re-enter this function
       * for the next src.
       */
      state->prev = saved_prev;
      state->prev_components_read = saved_prev_components_read;

      /* Save the dependencies for this instruction, so that future visits can
       * reuse the already-computed result for faster gathering, but only if it
       * has a scalar result. Vector results (e.g. vec4(x,y,z,w)) can have
       * different dependencies per component, but we only save dependencies
       * per instruction.
       */
      if (src->ssa->num_components == 1)
         _mesa_hash_table_insert(state->ht, instr, cur);
   }

   /* Accumulate dependencies for the caller. */
   BITSET_OR(state->prev->inputs, state->prev->inputs, cur->inputs);
   state->prev->uses_output_load |= cur->uses_output_load;
   state->prev->uses_ssbo_reads |= cur->uses_ssbo_reads;
   state->prev->uses_image_reads |= cur->uses_image_reads;
   return true;
}

static bool
visit_output_store(struct nir_builder *b, nir_intrinsic_instr *intr,
                   void *opaque)
{
   struct gather_state *state = (struct gather_state *)opaque;

   if (intr->intrinsic != nir_intrinsic_store_output &&
       intr->intrinsic != nir_intrinsic_store_per_vertex_output &&
       intr->intrinsic != nir_intrinsic_store_per_primitive_output)
      return false;

   /* Check whether we were asked to gather this output. */
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   assert(sem.num_slots >= 1);
   int index = -1;

   /* The write mask must be contigous starting from x. */
   ASSERTED unsigned writemask = nir_intrinsic_write_mask(intr);
   assert(writemask == BITFIELD_MASK(util_bitcount(writemask)));

   for (unsigned i = 0; i < state->all->num_locations; i++) {
      if (state->all->locations[i] >= (int)sem.location &&
          state->all->locations[i] < (int)(sem.location + sem.num_slots)) {
         index = i;
         break;
      }
   }
   if (index < 0)
      return false;

   /* Gather the output dependencies. */
   state->prev = &state->all->output[index];
   state->prev_components_read = BITFIELD_MASK(intr->src[0].ssa->num_components);

   print_progress(state, &intr->instr, true);
   gather_dependencies(&intr->src[0], state);
   print_progress(state, &intr->instr, false);
   return false;
}

/* For each output slot, gather which input components are used to compute it.
 * Component-wise ALU instructions must be scalar.
 */
void
nir_gather_output_dependencies(nir_shader *nir, nir_outputs_deps *deps)
{
   struct gather_state state = {
      .all = deps,
      .mem_ctx = ralloc_context(NULL),
      .ht = _mesa_pointer_hash_table_create(NULL),
   };

   memset(deps->output, 0, sizeof(deps->output));

   nir_shader_clear_pass_flags(nir);
   nir_shader_intrinsics_pass(nir, visit_output_store, nir_metadata_all,
                              &state);

   _mesa_hash_table_destroy(state.ht, NULL);
   ralloc_free(state.mem_ctx);
}

/* Gather 3 disjoint sets:
 * - the set of input components only used to compute outputs for the clipper
 *   (those that are only used to compute the position and clip outputs)
 * - the set of input components only used to compute all other outputs
 * - the set of input components that are used to compute BOTH outputs for
 *   the clipper and all other outputs
 *
 * Patch outputs are not gathered because shaders feeding the clipper don't
 * have patch outputs.
 */
void
nir_gather_output_clipper_var_groups(nir_shader *nir,
                                     nir_output_clipper_var_groups *groups)
{
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   /* Use calloc because these are large structures. */
   nir_outputs_deps *pos_deps = calloc(1, sizeof(nir_outputs_deps));
   nir_outputs_deps *var_deps = calloc(1, sizeof(nir_outputs_deps));

   uint64_t clipper_outputs = VARYING_BIT_POS |
                              VARYING_BIT_CLIP_VERTEX |
                              VARYING_BIT_CLIP_DIST0 |
                              VARYING_BIT_CLIP_DIST1;

   /* Gather input components used to compute outputs for the clipper. */
   u_foreach_bit64(i, nir->info.outputs_written & clipper_outputs) {
      pos_deps->locations[pos_deps->num_locations++] = i;
   }

   if (pos_deps->num_locations)
      nir_gather_output_dependencies(nir, pos_deps);

   /* Gather input components used to compute all other outputs. */
   u_foreach_bit64(i, nir->info.outputs_written & ~clipper_outputs) {
      var_deps->locations[var_deps->num_locations++] = i;
   }
   u_foreach_bit(i, nir->info.outputs_written_16bit) {
      var_deps->locations[var_deps->num_locations++] =
         VARYING_SLOT_VAR0_16BIT + i;
   }

   if (var_deps->num_locations)
      nir_gather_output_dependencies(nir, var_deps);

   /* OR-reduce the per-output sets. */
   memset(groups, 0, sizeof(*groups));

   for (unsigned i = 0; i < pos_deps->num_locations; i++) {
      assert(!pos_deps->output[i].uses_output_load);
      BITSET_OR(groups->pos_only, groups->pos_only,
                pos_deps->output[i].inputs);
   }

   for (unsigned i = 0; i < var_deps->num_locations; i++) {
      assert(!var_deps->output[i].uses_output_load);
      BITSET_OR(groups->var_only, groups->var_only,
                var_deps->output[i].inputs);
   }

   /* Compute the intersection of the above and make them disjoint. */
   BITSET_AND(groups->both, groups->pos_only, groups->var_only);
   BITSET_ANDNOT(groups->pos_only, groups->pos_only, groups->both);
   BITSET_ANDNOT(groups->var_only, groups->var_only, groups->both);

   free(pos_deps);
   free(var_deps);
}
