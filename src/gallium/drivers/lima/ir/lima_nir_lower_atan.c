/*
 * Copyright © 2025 Lima Project
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "nir.h"
#include "nir_builder.h"
#include "lima_ir.h"

/** lima_nir_lower_atan.c
 *
 * Lima atan lowering
 */

static bool
lower_atan(nir_builder *b, nir_alu_instr *instr, UNUSED void *cb_data)
{
   nir_def *lowered = NULL;

   b->cursor = nir_before_instr(&instr->instr);
   b->exact = instr->exact;
   b->fp_fast_math = instr->fp_fast_math;

   switch (instr->op) {
   case nir_op_fatan: {
      nir_def *src0 = nir_ssa_for_alu_src(b, instr, 0);
      nir_def *reg = nir_decl_reg(b, 3,  instr->def.bit_size, 0);
      nir_def *atan_pt1 = nir_atan_utg_pt1(b, src0);
      nir_build_store_reg(b, atan_pt1, reg, .write_mask = (1 << 3) - 1);
      nir_def *atan_pt2 = nir_atan_utg_pt2(b, nir_load_reg(b, reg));

      lowered = atan_pt2;
      break;
   }
   case nir_op_fatan2: {
      nir_def *src0 = nir_ssa_for_alu_src(b, instr, 0);
      nir_def *src1 = nir_ssa_for_alu_src(b, instr, 1);

      nir_def *reg = nir_decl_reg(b, 3,  instr->def.bit_size, 0);
      nir_def *atan_pt1 = nir_atan2_utg_pt1(b, src0, src1);
      nir_build_store_reg(b, atan_pt1, reg, .write_mask = (1 << 3) - 1);
      nir_def *load_reg1 = nir_load_reg(b, reg);
      nir_def *mul = nir_fmul(b, load_reg1, load_reg1);
      nir_alu_instr *mul_alu = nir_instr_as_alu(mul->parent_instr);
      mul_alu->src[1].swizzle[0] = 1;
      nir_build_store_reg(b, mul, reg, .write_mask = 1);
      nir_def *atan_pt2 = nir_atan_utg_pt2(b, nir_load_reg(b, reg));
      lowered = atan_pt2;;
      break;
   }

   default:
      break;
   }

   if (lowered) {
      nir_def_replace(&instr->def, lowered);
      return true;
   } else {
      return false;
   }
}

bool
lima_nir_lower_atan(nir_shader *shader)
{
   return nir_shader_alu_pass(shader, lower_atan,
                              nir_metadata_control_flow, NULL);
}
