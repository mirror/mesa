/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2018 Broadcom
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
#include "nir_builtin_builder.h"
#include "nir_builder.h"

/** nir_lower_alu.c
 *
 * NIR's home for miscellaneous ALU operation lowering implementations.
 *
 * Most NIR ALU lowering occurs in nir_opt_algebraic.py, since it's generally
 * easy to write them there.  However, if terms appear multiple times in the
 * lowered code, it can get very verbose and cause a lot of work for CSE, so
 * it may end up being easier to write out in C code.
 *
 * The shader must be in SSA for this pass.
 */

static nir_def *
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

static nir_def *
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
lower_alu_instr(nir_builder *b, nir_alu_instr *instr, UNUSED void *cb_data)
{
   nir_def *lowered = NULL;

   b->cursor = nir_before_instr(&instr->instr);
   b->exact = instr->exact;
   b->fp_fast_math = instr->fp_fast_math;

   switch (instr->op) {
   case nir_op_bitfield_reverse:
      if (b->shader->options->lower_bitfield_reverse) {
         /* For more details, see:
          *
          * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel
          */
         nir_def *c1 = nir_imm_int(b, 1);
         nir_def *c2 = nir_imm_int(b, 2);
         nir_def *c4 = nir_imm_int(b, 4);
         nir_def *c8 = nir_imm_int(b, 8);
         nir_def *c16 = nir_imm_int(b, 16);
         nir_def *c33333333 = nir_imm_int(b, 0x33333333);
         nir_def *c55555555 = nir_imm_int(b, 0x55555555);
         nir_def *c0f0f0f0f = nir_imm_int(b, 0x0f0f0f0f);
         nir_def *c00ff00ff = nir_imm_int(b, 0x00ff00ff);

         lowered = nir_ssa_for_alu_src(b, instr, 0);

         /* Swap odd and even bits. */
         lowered = nir_ior(b,
                           nir_iand(b, nir_ushr(b, lowered, c1), c55555555),
                           nir_ishl(b, nir_iand(b, lowered, c55555555), c1));

         /* Swap consecutive pairs. */
         lowered = nir_ior(b,
                           nir_iand(b, nir_ushr(b, lowered, c2), c33333333),
                           nir_ishl(b, nir_iand(b, lowered, c33333333), c2));

         /* Swap nibbles. */
         lowered = nir_ior(b,
                           nir_iand(b, nir_ushr(b, lowered, c4), c0f0f0f0f),
                           nir_ishl(b, nir_iand(b, lowered, c0f0f0f0f), c4));

         /* Swap bytes. */
         lowered = nir_ior(b,
                           nir_iand(b, nir_ushr(b, lowered, c8), c00ff00ff),
                           nir_ishl(b, nir_iand(b, lowered, c00ff00ff), c8));

         lowered = nir_ior(b,
                           nir_ushr(b, lowered, c16),
                           nir_ishl(b, lowered, c16));
      }
      break;

   case nir_op_bit_count:
      if (b->shader->options->lower_bit_count) {
         /* For more details, see:
          *
          * http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
          */

         lowered = nir_ssa_for_alu_src(b, instr, 0);
         unsigned bit_size = lowered->bit_size;

         nir_def *c1 = nir_imm_int(b, 1);
         nir_def *c2 = nir_imm_int(b, 2);
         nir_def *c4 = nir_imm_int(b, 4);
         nir_def *cshift = nir_imm_int(b, bit_size - 8);
         nir_def *c33333333 = nir_imm_intN_t(b, 0x33333333, bit_size);
         nir_def *c55555555 = nir_imm_intN_t(b, 0x55555555, bit_size);
         nir_def *c0f0f0f0f = nir_imm_intN_t(b, 0x0f0f0f0f, bit_size);
         nir_def *c01010101 = nir_imm_intN_t(b, 0x01010101, bit_size);


         lowered = nir_isub(b, lowered,
                            nir_iand(b, nir_ushr(b, lowered, c1), c55555555));

         lowered = nir_iadd(b,
                            nir_iand(b, lowered, c33333333),
                            nir_iand(b, nir_ushr(b, lowered, c2), c33333333));

         lowered = nir_ushr(b,
                            nir_imul(b,
                                     nir_iand(b,
                                              nir_iadd(b,
                                                       lowered,
                                                       nir_ushr(b, lowered, c4)),
                                              c0f0f0f0f),
                                     c01010101),
                            cshift);

         lowered = nir_u2u32(b, lowered);
      }
      break;

   case nir_op_imul_high:
   case nir_op_umul_high:
      if (b->shader->options->lower_mul_high) {
         nir_def *src0 = nir_ssa_for_alu_src(b, instr, 0);
         nir_def *src1 = nir_ssa_for_alu_src(b, instr, 1);
         if (src0->bit_size < 32) {
            /* Just do the math in 32-bit space and shift the result */
            nir_alu_type base_type = nir_op_infos[instr->op].output_type;

            nir_def *src0_32 = nir_type_convert(b, src0, base_type, base_type | 32, nir_rounding_mode_undef);
            nir_def *src1_32 = nir_type_convert(b, src1, base_type, base_type | 32, nir_rounding_mode_undef);
            nir_def *dest_32 = nir_imul(b, src0_32, src1_32);
            nir_def *dest_shifted = nir_ishr_imm(b, dest_32, src0->bit_size);
            lowered = nir_type_convert(b, dest_shifted, base_type, base_type | src0->bit_size, nir_rounding_mode_undef);
         } else {
            nir_def *cshift = nir_imm_int(b, src0->bit_size / 2);
            nir_def *cmask = nir_imm_intN_t(b, (1ull << (src0->bit_size / 2)) - 1, src0->bit_size);
            nir_def *different_signs = NULL;
            if (instr->op == nir_op_imul_high) {
               nir_def *c0 = nir_imm_intN_t(b, 0, src0->bit_size);
               different_signs = nir_ixor(b,
                                          nir_ilt(b, src0, c0),
                                          nir_ilt(b, src1, c0));
               src0 = nir_iabs(b, src0);
               src1 = nir_iabs(b, src1);
            }

            /*   ABCD
             * * EFGH
             * ======
             * (GH * CD) + (GH * AB) << 16 + (EF * CD) << 16 + (EF * AB) << 32
             *
             * Start by splitting into the 4 multiplies.
             */
            nir_def *src0l = nir_iand(b, src0, cmask);
            nir_def *src1l = nir_iand(b, src1, cmask);
            nir_def *src0h = nir_ushr(b, src0, cshift);
            nir_def *src1h = nir_ushr(b, src1, cshift);

            nir_def *lo = nir_imul(b, src0l, src1l);
            nir_def *m1 = nir_imul(b, src0l, src1h);
            nir_def *m2 = nir_imul(b, src0h, src1l);
            nir_def *hi = nir_imul(b, src0h, src1h);

            nir_def *tmp;

            tmp = nir_ishl(b, m1, cshift);
            hi = nir_iadd(b, hi, nir_uadd_carry(b, lo, tmp));
            lo = nir_iadd(b, lo, tmp);
            hi = nir_iadd(b, hi, nir_ushr(b, m1, cshift));

            tmp = nir_ishl(b, m2, cshift);
            hi = nir_iadd(b, hi, nir_uadd_carry(b, lo, tmp));
            lo = nir_iadd(b, lo, tmp);
            hi = nir_iadd(b, hi, nir_ushr(b, m2, cshift));

            if (instr->op == nir_op_imul_high) {
               /* For channels where different_signs is set we have to perform a
                * 64-bit negation.  This is *not* the same as just negating the
                * high 32-bits.  Consider -3 * 2.  The high 32-bits is 0, but the
                * desired result is -1, not -0!  Recall -x == ~x + 1.
                */
               nir_def *c1 = nir_imm_intN_t(b, 1, src0->bit_size);
               hi = nir_bcsel(b, different_signs,
                              nir_iadd(b,
                                       nir_inot(b, hi),
                                       nir_uadd_carry(b, nir_inot(b, lo), c1)),
                              hi);
            }

            lowered = hi;
         }
      }
      break;

   case nir_op_fmin:
   case nir_op_fmax: {
      if (!b->shader->options->lower_fminmax_signed_zero ||
          !nir_alu_instr_is_signed_zero_preserve(instr))
         break;

      nir_def *s0 = nir_ssa_for_alu_src(b, instr, 0);
      nir_def *s1 = nir_ssa_for_alu_src(b, instr, 1);

      bool max = instr->op == nir_op_fmax;
      nir_def *iminmax = max ? nir_imax(b, s0, s1) : nir_imin(b, s0, s1);

      /* Lower the fmin/fmax to a no_signed_zero fmin/fmax. This ensures that
       * nir_lower_alu is idempotent, and allows the backend to implement
       * soundly the no_signed_zero subset of fmin/fmax.
       */
      b->fp_fast_math &= ~FLOAT_CONTROLS_SIGNED_ZERO_PRESERVE;
      nir_def *fminmax = max ? nir_fmax(b, s0, s1) : nir_fmin(b, s0, s1);
      b->fp_fast_math = instr->fp_fast_math;

      lowered = nir_bcsel(b, nir_feq(b, s0, s1), iminmax, fminmax);
      break;
   }

   case nir_op_atan: {
      if (b->shader->options->has_atan)
         break;

      nir_def *src0 = nir_ssa_for_alu_src(b, instr, 0);
      b->fp_fast_math = instr->fp_fast_math;
      lowered = nir_lowered_atan(b, src0);
      break;
   }
   case nir_op_atan2: {
      if (b->shader->options->has_atan)
         break;
      nir_def *src0 = nir_ssa_for_alu_src(b, instr, 0);
      nir_def *src1 = nir_ssa_for_alu_src(b, instr, 1);
      b->fp_fast_math = instr->fp_fast_math;
      lowered = nir_lowered_atan2(b, src0, src1);
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
nir_lower_alu(nir_shader *shader)
{
   if (!shader->options->lower_bitfield_reverse &&
       !shader->options->lower_bit_count &&
       !shader->options->lower_mul_high &&
       !shader->options->lower_fminmax_signed_zero)
      return false;

   return nir_shader_alu_pass(shader, lower_alu_instr,
                              nir_metadata_control_flow, NULL);
}
