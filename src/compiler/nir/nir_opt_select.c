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
pass(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_bcsel)
      return false;

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
   nir_def_init(instr, &alu->def, 1, alu->src[1].src.ssa->bit_size);

   /* Rewrite the first OP to use the bcsel for the different source. */
   nir_src_rewrite(&source_alus[0]->src[different_src_index].src, &alu->def);
   source_alus[0]->src[different_src_index].swizzle[0] = 0;

   nir_instr_move(nir_after_instr(instr), &source_alus[0]->instr);
   nir_instr_remove(&source_alus[1]->instr);

   return true;
}

bool
nir_opt_select(nir_shader *shader)
{
   return nir_shader_instructions_pass(shader, pass, nir_metadata_control_flow, NULL);
}
