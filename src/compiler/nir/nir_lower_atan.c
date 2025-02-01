/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2018 Broadcom
 * Copyright © 2025 Vasily Khoruzhick <anarsoul@gmail.com>
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
#include "nir_builtin_builder.h"

/** nir_lower_atan.c
 *
 * NIR's atan and atan2 lowering
 */

nir_def *
nir_lowered_atan(nir_builder *b, nir_def *y_over_x)
{
   const uint32_t bit_size = y_over_x->bit_size;

   nir_def *abs_y_over_x = nir_fabs(b, y_over_x);

   /*
    * range-reduction, first step:
    *
    *      / y_over_x         if |y_over_x| <= 1.0;
    * u = <
    *      \ 1.0 / y_over_x   otherwise
    *
    * x = |u| for the corrected sign.
    */
   nir_def *le_1 = nir_fle_imm(b, abs_y_over_x, 1.0);
   nir_def *u = nir_bcsel(b, le_1, y_over_x, nir_frcp(b, y_over_x));

   /*
    * approximate atan by evaluating polynomial using Horner's method:
    *
    * x   * 0.9999793128310355 - x^3  * 0.3326756418091246 +
    * x^5 * 0.1938924977115610 - x^7  * 0.1173503194786851 +
    * x^9 * 0.0536813784310406 - x^11 * 0.0121323213173444
    */
   float coeffs[] = {
      -0.0121323213173444f, 0.0536813784310406f,
      -0.1173503194786851f, 0.1938924977115610f,
      -0.3326756418091246f, 0.9999793128310355f
   };

   nir_def *x_2 = nir_fmul(b, u, u);
   nir_def *res = nir_imm_floatN_t(b, coeffs[0], bit_size);

   for (unsigned i = 1; i < ARRAY_SIZE(coeffs); ++i) {
      res = nir_ffma_imm2(b, res, x_2, coeffs[i]);
   }

   /* range-reduction fixup value */
   nir_def *bias = nir_bcsel(b, le_1, nir_imm_floatN_t(b, 0, bit_size),
                             nir_imm_floatN_t(b, -M_PI_2, bit_size));

   /* multiply through by x while fixing up the range reduction */
   nir_def *tmp = nir_ffma(b, nir_fabs(b, u), res, bias);

   /* sign fixup */
   return nir_copysign(b, tmp, y_over_x);
}

nir_def *
nir_lowered_atan2(nir_builder *b, nir_def *y, nir_def *x)
{
   assert(y->bit_size == x->bit_size);
   const uint32_t bit_size = x->bit_size;

   nir_def *zero = nir_imm_floatN_t(b, 0, bit_size);
   nir_def *one = nir_imm_floatN_t(b, 1, bit_size);

   /* If we're on the left half-plane rotate the coordinates π/2 clock-wise
    * for the y=0 discontinuity to end up aligned with the vertical
    * discontinuity of atan(s/t) along t=0.  This also makes sure that we
    * don't attempt to divide by zero along the vertical line, which may give
    * unspecified results on non-GLSL 4.1-capable hardware.
    */
   nir_def *flip = nir_fge(b, zero, x);
   nir_def *s = nir_bcsel(b, flip, nir_fabs(b, x), y);
   nir_def *t = nir_bcsel(b, flip, y, nir_fabs(b, x));

   /* If the magnitude of the denominator exceeds some huge value, scale down
    * the arguments in order to prevent the reciprocal operation from flushing
    * its result to zero, which would cause precision problems, and for s
    * infinite would cause us to return a NaN instead of the correct finite
    * value.
    *
    * If fmin and fmax are respectively the smallest and largest positive
    * normalized floating point values representable by the implementation,
    * the constants below should be in agreement with:
    *
    *    huge <= 1 / fmin
    *    scale <= 1 / fmin / fmax (for |t| >= huge)
    *
    * In addition scale should be a negative power of two in order to avoid
    * loss of precision.  The values chosen below should work for most usual
    * floating point representations with at least the dynamic range of ATI's
    * 24-bit representation.
    */
   const double huge_val = bit_size >= 32 ? 1e18 : 16384;
   nir_def *scale = nir_bcsel(b, nir_fge_imm(b, nir_fabs(b, t), huge_val),
                              nir_imm_floatN_t(b, 0.25, bit_size), one);
   nir_def *rcp_scaled_t = nir_frcp(b, nir_fmul(b, t, scale));
   nir_def *abs_s_over_t = nir_fmul(b, nir_fabs(b, nir_fmul(b, s, scale)),
                                    nir_fabs(b, rcp_scaled_t));

   /* For |x| = |y| assume tan = 1 even if infinite (i.e. pretend momentarily
    * that ∞/∞ = 1) in order to comply with the rather artificial rules
    * inherited from IEEE 754-2008, namely:
    *
    *  "atan2(±∞, −∞) is ±3π/4
    *   atan2(±∞, +∞) is ±π/4"
    *
    * Note that this is inconsistent with the rules for the neighborhood of
    * zero that are based on iterated limits:
    *
    *  "atan2(±0, −0) is ±π
    *   atan2(±0, +0) is ±0"
    *
    * but GLSL specifically allows implementations to deviate from IEEE rules
    * at (0,0), so we take that license (i.e. pretend that 0/0 = 1 here as
    * well).
    */
   nir_def *tan = nir_bcsel(b, nir_feq(b, nir_fabs(b, x), nir_fabs(b, y)),
                            one, abs_s_over_t);

   /* Calculate the arctangent and fix up the result if we had flipped the
    * coordinate system.
    */
   nir_def *arc =
      nir_ffma_imm1(b, nir_b2fN(b, flip, bit_size), M_PI_2, nir_lowered_atan(b, tan));

   /* Rather convoluted calculation of the sign of the result.  When x < 0 we
    * cannot use fsign because we need to be able to distinguish between
    * negative and positive zero.  We don't use bitwise arithmetic tricks for
    * consistency with the GLSL front-end.  When x >= 0 rcp_scaled_t will
    * always be non-negative so this won't be able to distinguish between
    * negative and positive zero, but we don't care because atan2 is
    * continuous along the whole positive y = 0 half-line, so it won't affect
    * the result significantly.
    */
   return nir_bcsel(b, nir_flt(b, nir_fmin(b, y, rcp_scaled_t), zero),
                    nir_fneg(b, arc), arc);
}

static bool
lower_atan(nir_builder *b, nir_alu_instr *instr, UNUSED void *cb_data)
{
   if (instr->op != nir_op_fatan && instr->op != nir_op_fatan2)
      return false;

   b->cursor = nir_before_instr(&instr->instr);
   b->exact = instr->exact;
   b->fp_fast_math = instr->fp_fast_math;
   nir_def *src0 = nir_ssa_for_alu_src(b, instr, 0);

   if (instr->op == nir_op_fatan) {
      nir_def_replace(&instr->def, nir_lowered_atan(b, src0));
   } else {
      nir_def *src1 = nir_ssa_for_alu_src(b, instr, 1);
      nir_def_replace(&instr->def, nir_lowered_atan2(b, src0, src1));
   }

   return true;
}

bool
nir_lower_atan(nir_shader *shader)
{
   if (shader->options->has_atan)
      return false;

   return nir_shader_alu_pass(shader, lower_atan,
                              nir_metadata_control_flow, NULL);
}
