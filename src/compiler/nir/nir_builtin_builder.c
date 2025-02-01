/*
 * Copyright © 2018 Red Hat Inc.
 * Copyright © 2015 Intel Corporation
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

#include <math.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_builtin_builder.h"

nir_def *
nir_cross3(nir_builder *b, nir_def *x, nir_def *y)
{
   unsigned yzx[3] = { 1, 2, 0 };
   unsigned zxy[3] = { 2, 0, 1 };

   return nir_ffma(b, nir_swizzle(b, x, yzx, 3),
                   nir_swizzle(b, y, zxy, 3),
                   nir_fneg(b, nir_fmul(b, nir_swizzle(b, x, zxy, 3),
                                        nir_swizzle(b, y, yzx, 3))));
}

nir_def *
nir_cross4(nir_builder *b, nir_def *x, nir_def *y)
{
   nir_def *cross = nir_cross3(b, x, y);

   return nir_vec4(b,
                   nir_channel(b, cross, 0),
                   nir_channel(b, cross, 1),
                   nir_channel(b, cross, 2),
                   nir_imm_intN_t(b, 0, cross->bit_size));
}

nir_def *
nir_fast_length(nir_builder *b, nir_def *vec)
{
   return nir_fsqrt(b, nir_fdot(b, vec, vec));
}

nir_def *
nir_nextafter(nir_builder *b, nir_def *x, nir_def *y)
{
   nir_def *zero = nir_imm_intN_t(b, 0, x->bit_size);
   nir_def *one = nir_imm_intN_t(b, 1, x->bit_size);

   nir_def *condeq = nir_feq(b, x, y);
   nir_def *conddir = nir_flt(b, x, y);
   nir_def *condzero = nir_feq(b, x, zero);

   uint64_t sign_mask = 1ull << (x->bit_size - 1);
   uint64_t min_abs = 1;

   if (nir_is_denorm_flush_to_zero(b->shader->info.float_controls_execution_mode, x->bit_size)) {
      switch (x->bit_size) {
      case 16:
         min_abs = 1 << 10;
         break;
      case 32:
         min_abs = 1 << 23;
         break;
      case 64:
         min_abs = 1ULL << 52;
         break;
      }

      /* Flush denorm to zero to avoid returning a denorm when condeq is true. */
      x = nir_fmul_imm(b, x, 1.0);
   }

   /* beware of: +/-0.0 - 1 == NaN */
   nir_def *xn =
      nir_bcsel(b,
                condzero,
                nir_imm_intN_t(b, sign_mask | min_abs, x->bit_size),
                nir_isub(b, x, one));

   /* beware of -0.0 + 1 == -0x1p-149 */
   nir_def *xp = nir_bcsel(b, condzero,
                           nir_imm_intN_t(b, min_abs, x->bit_size),
                           nir_iadd(b, x, one));

   /* nextafter can be implemented by just +/- 1 on the int value */
   nir_def *res =
      nir_bcsel(b, nir_ixor(b, conddir, nir_flt(b, x, zero)), xp, xn);

   return nir_nan_check2(b, x, y, nir_bcsel(b, condeq, x, res));
}

nir_def *
nir_normalize(nir_builder *b, nir_def *vec)
{
   if (vec->num_components == 1)
      return nir_fsign(b, vec);

   nir_def *f0 = nir_imm_floatN_t(b, 0.0, vec->bit_size);
   nir_def *f1 = nir_imm_floatN_t(b, 1.0, vec->bit_size);
   nir_def *finf = nir_imm_floatN_t(b, INFINITY, vec->bit_size);

   /* scale the input to increase precision */
   nir_def *maxc = nir_fmax_abs_vec_comp(b, vec);
   nir_def *svec = nir_fdiv(b, vec, maxc);
   /* for inf */
   nir_def *finfvec = nir_copysign(b, nir_bcsel(b, nir_feq(b, vec, finf), f1, f0), f1);

   nir_def *temp = nir_bcsel(b, nir_feq(b, maxc, finf), finfvec, svec);
   nir_def *res = nir_fmul(b, temp, nir_frsq(b, nir_fdot(b, temp, temp)));

   return nir_bcsel(b, nir_feq(b, maxc, f0), vec, res);
}

nir_def *
nir_smoothstep(nir_builder *b, nir_def *edge0, nir_def *edge1, nir_def *x)
{
   nir_def *f2 = nir_imm_floatN_t(b, 2.0, x->bit_size);
   nir_def *f3 = nir_imm_floatN_t(b, 3.0, x->bit_size);

   /* t = clamp((x - edge0) / (edge1 - edge0), 0, 1) */
   nir_def *t =
      nir_fsat(b, nir_fdiv(b, nir_fsub(b, x, edge0),
                           nir_fsub(b, edge1, edge0)));

   /* result = t * t * (3 - 2 * t) */
   return nir_fmul(b, t, nir_fmul(b, t, nir_a_minus_bc(b, f3, f2, t)));
}

nir_def *
nir_upsample(nir_builder *b, nir_def *hi, nir_def *lo)
{
   assert(lo->num_components == hi->num_components);
   assert(lo->bit_size == hi->bit_size);

   nir_def *res[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < lo->num_components; ++i) {
      nir_def *vec = nir_vec2(b, nir_channel(b, lo, i), nir_channel(b, hi, i));
      res[i] = nir_pack_bits(b, vec, vec->bit_size * 2);
   }

   return nir_vec(b, res, lo->num_components);
}

nir_def *
nir_build_texture_query(nir_builder *b, nir_tex_instr *tex, nir_texop texop,
                        unsigned components, nir_alu_type dest_type,
                        bool include_coord, bool include_lod)
{
   nir_tex_instr *query;

   unsigned num_srcs = include_lod ? 1 : 0;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      if ((tex->src[i].src_type == nir_tex_src_coord && include_coord) ||
          tex->src[i].src_type == nir_tex_src_texture_deref ||
          tex->src[i].src_type == nir_tex_src_sampler_deref ||
          tex->src[i].src_type == nir_tex_src_texture_offset ||
          tex->src[i].src_type == nir_tex_src_sampler_offset ||
          tex->src[i].src_type == nir_tex_src_texture_handle ||
          tex->src[i].src_type == nir_tex_src_sampler_handle)
         num_srcs++;
   }

   query = nir_tex_instr_create(b->shader, num_srcs);
   query->op = texop;
   query->sampler_dim = tex->sampler_dim;
   query->is_array = tex->is_array;
   query->is_shadow = tex->is_shadow;
   query->is_new_style_shadow = tex->is_new_style_shadow;
   query->texture_index = tex->texture_index;
   query->sampler_index = tex->sampler_index;
   query->dest_type = dest_type;

   if (include_coord) {
      query->coord_components = tex->coord_components;
   }

   unsigned idx = 0;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      if ((tex->src[i].src_type == nir_tex_src_coord && include_coord) ||
          tex->src[i].src_type == nir_tex_src_texture_deref ||
          tex->src[i].src_type == nir_tex_src_sampler_deref ||
          tex->src[i].src_type == nir_tex_src_texture_offset ||
          tex->src[i].src_type == nir_tex_src_sampler_offset ||
          tex->src[i].src_type == nir_tex_src_texture_handle ||
          tex->src[i].src_type == nir_tex_src_sampler_handle) {
         query->src[idx].src = nir_src_for_ssa(tex->src[i].src.ssa);
         query->src[idx].src_type = tex->src[i].src_type;
         idx++;
      }
   }

   /* Add in an LOD because some back-ends require it */
   if (include_lod) {
      query->src[idx] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(b, 0));
   }

   nir_def_init(&query->instr, &query->def, nir_tex_instr_dest_size(query),
                nir_alu_type_get_type_size(dest_type));

   nir_builder_instr_insert(b, &query->instr);
   return &query->def;
}

nir_def *
nir_get_texture_size(nir_builder *b, nir_tex_instr *tex)
{
   b->cursor = nir_before_instr(&tex->instr);

   return nir_build_texture_query(b, tex, nir_texop_txs,
                                  nir_tex_instr_dest_size(tex),
                                  nir_type_int32, false, true);
}

nir_def *
nir_get_texture_lod(nir_builder *b, nir_tex_instr *tex)
{
   b->cursor = nir_before_instr(&tex->instr);

   nir_def *tql = nir_build_texture_query(b, tex, nir_texop_lod, 2,
                                          nir_type_float32, true, false);

   /* The LOD is the y component of the result */
   return nir_channel(b, tql, 1);
}
