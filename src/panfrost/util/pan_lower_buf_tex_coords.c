/*
 * Copyright (C) 2024 BrightSign LLC
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

/* Texture access take 16-bit coordinates, which is too limited for buffer
 * objects. Work around this limitation by turning buffer texture accesses
 * into 2D ones, with the lower 16-bits of the texel index being treated as
 * the X axis and the upper 16-bits as the Y axis.
 */
static void
lower_buf_tex_coords(nir_builder *b, nir_tex_instr *tex)
{
   assert(tex->sampler_dim == GLSL_SAMPLER_DIM_BUF);

   /* Buffer textures are 1D, no mipmaps, no arrays */
   assert(!nir_steal_tex_src(tex, nir_tex_src_ddy));
   assert(!nir_steal_tex_src(tex, nir_tex_src_offset));
   assert(!tex->is_array);

   b->cursor = nir_before_instr(&tex->instr);

   nir_def *coords = nir_steal_tex_src(tex, nir_tex_src_coord);
   nir_def *ddx = nir_steal_tex_src(tex, nir_tex_src_ddx);

   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;

   if (coords) {
      assert(tex->coord_components == 1);
      tex->coord_components = 2;
      nir_def *split_x_coord =
         nir_u2u32(b, nir_unpack_32_2x16(b, nir_channel(b, coords, 0)));

      coords = nir_vec2(b, nir_channel(b, split_x_coord, 0),
                        nir_channel(b, split_x_coord, 1));

      nir_tex_instr_add_src(tex, nir_tex_src_coord, coords);
   }

   if (ddx) {
      nir_tex_instr_add_src(tex, nir_tex_src_ddx,
                            nir_pad_vector_imm_int(b, ddx, 0, 2));
   }
}

bool
pan_lower_buf_tex_coords(nir_shader *s)
{
   bool any_progress = false;

   nir_foreach_function_impl(impl, s) {
      nir_builder builder = nir_builder_create(impl);
      bool changed = false;

      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;

            nir_tex_instr *tex = nir_instr_as_tex(instr);
            if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF &&
                tex->op != nir_texop_txs) {
               lower_buf_tex_coords(&builder, tex);
               any_progress |= true;
               changed = true;
            }
         }
      }

      if (changed)
         nir_metadata_preserve(impl, nir_metadata_control_flow);
      else
         nir_metadata_preserve(impl, 0);
   }

   return any_progress;
}
