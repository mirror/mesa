/*
 * Copyright 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_builder.h"

/*
 * This pass rewrites patterns like 
 *    bcsel(a, op(..., b, ...), op(..., c, ...))
 * to
 *    op(..., bcsel(a, b, c), ...)
 * The bcsel has to be scalar, swizzles are supported.
 */

static bool
opt_bcsel(nir_builder *b, nir_alu_instr *alu)
{
   nir_instr *source_instrs[2] = {
      alu->src[1].src.ssa->parent_instr,
      alu->src[2].src.ssa->parent_instr,
   };

   if (source_instrs[0]->type != nir_instr_type_alu ||
       source_instrs[1]->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *source_alus[2] = {
      nir_instr_as_alu(source_instrs[0]),
      nir_instr_as_alu(source_instrs[1])
   };

   if (source_alus[0]->op != source_alus[1]->op)
      return false;

   if (!list_is_singular(&source_alus[0]->def.uses) ||
       !list_is_singular(&source_alus[1]->def.uses))
      return false;

   uint32_t different_src_count = 0;
   uint32_t different_src_index = 0;
   for (uint32_t i = 0; i < nir_op_infos[source_alus[0]->op].num_inputs; i++) {
      if (!nir_alu_srcs_equal(source_alus[0], source_alus[1], i, i)) {
         different_src_count++;
         different_src_index = i;
      }
   }

   /* Emitting more than 1 bcsel does not reduce the instruction count. */
   if (different_src_count != 1)
      return false;

   /* Assume that bcsel instructions will be scalarized (later). */
   if (nir_src_num_components(source_alus[0]->src[different_src_index].src) != 1 ||
       nir_src_num_components(source_alus[1]->src[different_src_index].src) != 1)
      return false;

   /* Rewrite the bcsel sources to be the different sources. */
   nir_src_rewrite(&alu->src[1].src, source_alus[0]->src[different_src_index].src.ssa);
   alu->src[1].swizzle[0] = source_alus[0]->src[different_src_index].swizzle[0];

   nir_src_rewrite(&alu->src[2].src, source_alus[1]->src[different_src_index].src.ssa);
   alu->src[2].swizzle[0] = source_alus[1]->src[different_src_index].swizzle[0];

   nir_def_rewrite_uses(&alu->def, &source_alus[0]->def);
   nir_def_init(&alu->instr, &alu->def, 1, alu->src[1].src.ssa->bit_size);

   /* Rewrite the first OP to use the bcsel for the different source. */
   nir_src_rewrite(&source_alus[0]->src[different_src_index].src, &alu->def);
   source_alus[0]->src[different_src_index].swizzle[0] = 0;

   nir_instr_move(nir_after_instr(&alu->instr), &source_alus[0]->instr);
   nir_instr_remove(&source_alus[1]->instr);

   return true;
}

static bool
opt_phi(nir_builder *b, nir_phi_instr *phi)
{
   if (exec_list_length(&phi->srcs) < 2)
      return false;

   nir_foreach_phi_src(src, phi) {
      if (!nir_src_is_alu(src->src))
         return false;
   }

   nir_alu_instr *first_alu = NULL;
   nir_op op = nir_num_opcodes;
   nir_foreach_phi_src(src, phi) {
      nir_alu_instr *alu = nir_src_as_alu_instr(src->src);

      if (!first_alu)
         first_alu = alu;

      if (op == nir_num_opcodes)
         op = alu->op;
      else if (op != alu->op)
         return false;
      
      if (!list_is_singular(&alu->def.uses))
         return false;
   }

   uint32_t different_src_index = 0;
   uint32_t different_src_count = 0;
   for (uint32_t i = 0; i < nir_op_infos[op].num_inputs; i++) {
      nir_foreach_phi_src(src, phi) {
         nir_alu_instr *alu = nir_src_as_alu_instr(src->src);
         if (!nir_alu_srcs_equal(first_alu, alu, i, i)) {
            different_src_count++;
            different_src_index = i;
         }
      }
   }

   /* TODO: Having multiple PHIs may be worth it if register allocation is good. */
   if (different_src_count != 1)
      return false;

   nir_foreach_phi_src(src, phi) {
      nir_alu_instr *alu = nir_src_as_alu_instr(src->src);
      if (nir_src_num_components(alu->src[different_src_index].src) != 1)
         return false;
      if (alu->src[different_src_index].swizzle[0] != 0)
         return false;
   }

   /* Rewrite the phi sources to be the different sources. */
   nir_foreach_phi_src(src, phi) {
      nir_alu_instr *alu = nir_src_as_alu_instr(src->src);
      nir_src_rewrite(&src->src, alu->src[different_src_index].src.ssa);
   }

   nir_def_rewrite_uses(&phi->def, &first_alu->def);
   nir_def_init(&phi->instr, &phi->def, 1, nir_src_bit_size(first_alu->src[different_src_index].src));

   /* Rewrite the first OP to use the phi for the different source. */
   nir_src_rewrite(&first_alu->src[different_src_index].src, &phi->def);

   nir_instr_move(nir_after_phis(phi->instr.block), &first_alu->instr);

   return true;
}

static bool
pass(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      if (alu->op == nir_op_bcsel)
         return opt_bcsel(b, alu);
   }

   if (instr->type == nir_instr_type_phi)
      return opt_phi(b, nir_instr_as_phi(instr));

   return false;
}

bool
nir_opt_select(nir_shader *shader)
{
   return nir_shader_instructions_pass(shader, pass, nir_metadata_control_flow, NULL);
}
