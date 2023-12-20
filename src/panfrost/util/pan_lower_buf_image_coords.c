/*
 * Copyright (C) 2023 Collabora Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "compiler/nir/nir_builder.h"

#include "pan_ir.h"

/* Image access take 16-bit coordinates, which is too limited for buffer
 * objects. Work around this limitation by turning buffer image access into 2D
 * ones, with the lower 16-bits of the texel index being treated as the X axis
 * and the upper 16-bits as the Y axis.
 *
 * 32-bit -> 16-bit coordinate lowering is left to the compiler backend.
 */
static bool
lower_buf_image_coords(nir_builder *b, nir_intrinsic_instr *intr,
                      UNUSED void *data)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_image_load:
   case nir_intrinsic_image_store:
   case nir_intrinsic_image_texel_address:
      break;
   default:
      return false;
   }

   enum glsl_sampler_dim dim = nir_intrinsic_image_dim(intr);

   if (dim != GLSL_SAMPLER_DIM_BUF)
      return false;

   b->cursor = nir_before_instr(&intr->instr);

   nir_def *undef = nir_undef(b, 1, 32);
   nir_def *coord = intr->src[1].ssa;
   nir_def *split_x_coord =
      nir_u2u32(b, nir_unpack_32_2x16(b, nir_channel(b, coord, 0)));

   assert(!nir_intrinsic_image_array(intr));
   coord = nir_vec4(b, nir_channel(b, split_x_coord, 0),
                    nir_channel(b, split_x_coord, 1), undef, undef);

   nir_intrinsic_set_image_dim(intr, GLSL_SAMPLER_DIM_2D);
   nir_src_rewrite(&intr->src[1], coord);
   return true;
}

bool
pan_lower_buf_image_coords(nir_shader *s)
{
   return nir_shader_intrinsics_pass(
      s, lower_buf_image_coords,
      nir_metadata_block_index | nir_metadata_dominance, NULL);
}
