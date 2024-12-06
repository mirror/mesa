/*
 * Copyright © 2018 Intel Corporation
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

#include "isl/isl.h"

#include "brw_nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_format_convert.h"

static nir_def *
load_image_param(nir_builder *b, nir_deref_instr *deref, unsigned indice)
{
   nir_intrinsic_instr *load =
      nir_intrinsic_instr_create(b->shader,
                                 nir_intrinsic_image_deref_load_param_intel);
   load->src[0] = nir_src_for_ssa(&deref->def);
   nir_intrinsic_set_base(load, indice);

   switch (indice) {
   case ISL_IMG_PARAM_SURF_SIZE:
      load->num_components = 2;
      break;
   case ISL_IMG_PARAM_TILE_MODE:
   case ISL_IMG_PARAM_SURF_PITCH:
      load->num_components = 1;
      break;
   default:
      unreachable("Invalid param offset");
   }
   nir_def_init(&load->instr, &load->def, load->num_components, 32);

   nir_builder_instr_insert(b, &load->instr);
   return &load->def;
}


static nir_def *
load_image_base_address(nir_builder *b, nir_deref_instr *deref)
{
   nir_intrinsic_instr *load =
      nir_intrinsic_instr_create(b->shader,
         nir_intrinsic_image_deref_load_base_address_intel);

   load->src[0] = nir_src_for_ssa(&deref->def);
   nir_def_init(&load->instr, &load->def, 1, 64);

   nir_builder_instr_insert(b, &load->instr);
   return &load->def;
}


static nir_def *
image_coord_is_in_bounds(nir_builder *b, nir_deref_instr *deref,
                         nir_def *coord)
{
   nir_def *size = load_image_param(b, deref, ISL_IMG_PARAM_SURF_SIZE);
   nir_def *cmp = nir_ilt(b, coord, size);

   unsigned coord_comps = glsl_get_sampler_coordinate_components(deref->type);
   nir_def *in_bounds = nir_imm_true(b);
   for (unsigned i = 0; i < coord_comps; i++)
      in_bounds = nir_iand(b, in_bounds, nir_channel(b, cmp, i));

   return in_bounds;
}

/** Calculate the offset in memory of the texel given by \p coord.
 *
 * This is meant to be used with untyped surface messages to access a tiled
 * surface, what involves taking into account the tiling.
 *
*/
static nir_def *
image_address(nir_builder *b, const struct intel_device_info *devinfo,
              nir_deref_instr *deref, nir_def *coord)
{
   unsigned dims = glsl_get_sampler_coordinate_components(deref->type);
   coord = nir_trim_vector(b, coord, dims);

   nir_def *tile_mode = load_image_param(b, deref, ISL_IMG_PARAM_TILE_MODE);
   nir_def *pitch_in_bytes = load_image_param(b, deref, ISL_IMG_PARAM_SURF_PITCH);
   // Only consider 64bit
   nir_def *pitch = nir_ushr(b, pitch_in_bytes, nir_imm_int(b, 3));

   nir_def *xypos = (coord->num_components == 1) ?
         nir_vec2(b, coord, nir_imm_int(b, 0)) : nir_trim_vector(b, coord, 2);

   nir_def *linear_addr;
   nir_def *tile4_addr;

   nir_def *not_do_detiling = nir_ieq_imm(b, tile_mode, 0);
   nir_if *if_not_do_detiling = nir_push_if(b, not_do_detiling);

   {
      /*Simple Linear
      Add it to the start of the tile row. */
      nir_def *idx;
      idx = nir_imul(b, nir_channel(b, xypos, 1), pitch);
      idx = nir_iadd(b, idx, nir_channel(b, xypos, 0));

      /* Multiply by the Bpp value. */
      linear_addr = nir_imul(b, idx, nir_imm_int(b, 8));
   }
   nir_push_else(b, if_not_do_detiling);
   {
      // Tile4 De-tiling Algorithm
      // #define TILE_WIDTH_BITS   4  // Log2(Tile Width in 64bpe Pixels)
      // #define TILE_HEIGHT_BITS  5  // Log2(Tile Height)
      // #define TILE_SIZE_BITS    12 // Log2(Tile Size in Bytes)
      //
      //        tilesPerRow = surface.Pitch >> TILE_WIDTH_BITS; // Surface Width in Tiles
      //
      //        // Tile grid position on surface, of tile containing specified pixel.
      //        int row = y >> TILE_HEIGHT_BITS;
      //        int col = x >> TILE_WIDTH_BITS;
      //        // Start with surface offset of given tile...
      //        int swizzledOffset = (row * tilesPerRow + col) << TILE_SIZE_BITS; // <-- Tiles laid across surface in row-major order.
      //
      //        // ...then OR swizzled offset of byte within tile...
      //        swizzledOffset +=      // YYxYxxYYx---
      //            (y << 7 & 0xc00) | // YY<<<<<<<---
      //            (y << 6 & 0x200) | //    Y<<<<<<--
      //            (y << 4 & 0x030) | //       YY<<<<
      //            (x << 6 & 0x200) | //   x<<<<<<---
      //            (x << 5 & 0x0c0) | //     xx<<<<<-
      //            (x << 3 & 0x008);  //         x<<<
      //
      //        linearAddress = surface.SurfaceBaseAddress + swizzledOffset;
      nir_def *x = nir_channel(b, xypos, 0);
      nir_def *y = nir_channel(b, xypos, 1);
      nir_def *row = nir_ushr(b, y, nir_imm_int(b, 5));
      nir_def *col = nir_ushr(b, x, nir_imm_int(b, 4));

      nir_def *tilesPerRow = nir_ushr(b, pitch, nir_imm_int(b, 4));
      tile4_addr = nir_imul(b, tilesPerRow, row);
      tile4_addr = nir_iadd(b, tile4_addr, col);
      tile4_addr = nir_ishl(b, tile4_addr, nir_imm_int(b, 12));

      nir_def* tile4_shl1 = nir_ishl(b, nir_channel(b, xypos, 1), nir_imm_int(b, 7));
      nir_def* tile4_and1 = nir_iand(b, tile4_shl1, nir_imm_int(b, 0xc00));
      nir_def* tile4_shl2 = nir_ishl(b, nir_channel(b, xypos, 1), nir_imm_int(b, 6));
      nir_def* tile4_and2 = nir_iand(b, tile4_shl2, nir_imm_int(b, 0x100));
      nir_def* tile4_shl3 =nir_ishl(b, nir_channel(b, xypos, 1), nir_imm_int(b, 4));
      nir_def* tile4_and3 = nir_iand(b, tile4_shl3, nir_imm_int(b, 0x030));
      nir_def* tile4_shl4 = nir_ishl(b, nir_channel(b, xypos, 0), nir_imm_int(b, 6));
      nir_def* tile4_and4 = nir_iand(b, tile4_shl4, nir_imm_int(b, 0x200));
      nir_def* tile4_shl5 = nir_ishl(b, nir_channel(b, xypos, 0), nir_imm_int(b, 5));
      nir_def* tile4_and5 = nir_iand(b, tile4_shl5, nir_imm_int(b, 0x0c0));
      nir_def* tile4_shl6 = nir_ishl(b, nir_channel(b, xypos, 0), nir_imm_int(b, 3));
      nir_def* tile4_and6 = nir_iand(b, tile4_shl6, nir_imm_int(b, 0x008));
      nir_def* tile4_or1 = nir_ior(b, tile4_and1, tile4_and2);
      nir_def* tile4_or2 = nir_ior(b, tile4_and3, tile4_and4);
      nir_def* tile4_or3 = nir_ior(b, tile4_or1, tile4_or2);
      nir_def* tile4_or4 = nir_ior(b, tile4_and5, tile4_and6);
      nir_def* tile4_or5 = nir_ior(b, tile4_or3, tile4_or4);

      tile4_addr = nir_iadd(b, tile4_addr, tile4_or5);

   }
   nir_pop_if(b, if_not_do_detiling);

   nir_def *addr = nir_if_phi(b, linear_addr, tile4_addr);

   return addr;
}

struct format_info {
   const struct isl_format_layout *fmtl;
   unsigned chans;
   unsigned bits[4];
};

static struct format_info
get_format_info(enum isl_format fmt)
{
   const struct isl_format_layout *fmtl = isl_format_get_layout(fmt);

   return (struct format_info) {
      .fmtl = fmtl,
      .chans = isl_format_get_num_channels(fmt),
      .bits = {
         fmtl->channels.r.bits,
         fmtl->channels.g.bits,
         fmtl->channels.b.bits,
         fmtl->channels.a.bits
      },
   };
}

static nir_def *
convert_color_for_load(nir_builder *b, const struct intel_device_info *devinfo,
                       nir_def *color,
                       enum isl_format image_fmt, enum isl_format lower_fmt,
                       unsigned dest_components)
{
   if (image_fmt == lower_fmt)
      goto expand_vec;

   if (image_fmt == ISL_FORMAT_R11G11B10_FLOAT) {
      assert(lower_fmt == ISL_FORMAT_R32_UINT);
      color = nir_format_unpack_11f11f10f(b, color);
      goto expand_vec;
   } else if (image_fmt == ISL_FORMAT_R64_PASSTHRU) {
      assert(lower_fmt == ISL_FORMAT_R32G32_UINT);
      color = nir_pack_64_2x32(b, nir_channels(b, color, 0x3));
      goto expand_vec;
   }

   struct format_info image = get_format_info(image_fmt);
   struct format_info lower = get_format_info(lower_fmt);

   const bool needs_sign_extension =
      isl_format_has_snorm_channel(image_fmt) ||
      isl_format_has_sint_channel(image_fmt);

   /* We only check the red channel to detect if we need to pack/unpack */
   assert(image.bits[0] != lower.bits[0] ||
          memcmp(image.bits, lower.bits, sizeof(image.bits)) == 0);

   if (image.bits[0] != lower.bits[0] && lower_fmt == ISL_FORMAT_R32_UINT) {
      if (needs_sign_extension)
         color = nir_format_unpack_sint(b, color, image.bits, image.chans);
      else
         color = nir_format_unpack_uint(b, color, image.bits, image.chans);
   } else {
      /* All these formats are homogeneous */
      for (unsigned i = 1; i < image.chans; i++)
         assert(image.bits[i] == image.bits[0]);

      if (image.bits[0] != lower.bits[0]) {
         color = nir_format_bitcast_uvec_unmasked(b, color, lower.bits[0],
                                                  image.bits[0]);
      }

      if (needs_sign_extension)
         color = nir_format_sign_extend_ivec(b, color, image.bits);
   }

   switch (image.fmtl->channels.r.type) {
   case ISL_UNORM:
      assert(isl_format_has_uint_channel(lower_fmt));
      color = nir_format_unorm_to_float(b, color, image.bits);
      break;

   case ISL_SNORM:
      assert(isl_format_has_uint_channel(lower_fmt));
      color = nir_format_snorm_to_float(b, color, image.bits);
      break;

   case ISL_SFLOAT:
      if (image.bits[0] == 16)
         color = nir_unpack_half_2x16_split_x(b, color);
      break;

   case ISL_UINT:
   case ISL_SINT:
      break;

   default:
      unreachable("Invalid image channel type");
   }

expand_vec:
   assert(dest_components == 1 || dest_components == 4);
   assert(color->num_components <= dest_components);
   if (color->num_components == dest_components)
      return color;

   nir_def *comps[4];
   for (unsigned i = 0; i < color->num_components; i++)
      comps[i] = nir_channel(b, color, i);

   for (unsigned i = color->num_components; i < 3; i++)
      comps[i] = nir_imm_zero(b, 1, color->bit_size);

   if (color->num_components < 4) {
      if (isl_format_has_int_channel(image_fmt) ||
      image_fmt == ISL_FORMAT_R64_PASSTHRU)
         comps[3] = nir_imm_intN_t(b, 1, color->bit_size);
      else
         comps[3] = nir_imm_float(b, 1);
   }

   return nir_vec(b, comps, dest_components);
}

static bool
lower_image_load_instr(nir_builder *b,
                       const struct intel_device_info *devinfo,
                       nir_intrinsic_instr *intrin,
                       bool sparse)
{
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   if (var->data.image.format == PIPE_FORMAT_NONE)
      return false;

   const enum isl_format image_fmt =
      isl_format_for_pipe_format(var->data.image.format);

   assert(isl_has_matching_typed_storage_image_format(devinfo, image_fmt));
   const enum isl_format lower_fmt =
      isl_lower_storage_image_format(devinfo, image_fmt);
   const unsigned dest_components =
      sparse ? (intrin->num_components - 1) : intrin->num_components;

   /* Use an undef to hold the uses of the load while we do the color
    * conversion.
    */
   nir_def *placeholder = nir_undef(b, 4, 32);
   nir_def_rewrite_uses(&intrin->def, placeholder);

   intrin->num_components = isl_format_get_num_channels(lower_fmt);
   intrin->def.num_components = intrin->num_components;

   if (intrin->def.bit_size == 64 &&
      lower_fmt == ISL_FORMAT_R32G32_UINT) {
      intrin->def.bit_size = 32;
   }

   b->cursor = nir_after_instr(&intrin->instr);

   nir_def *color = convert_color_for_load(b, devinfo, &intrin->def, image_fmt, lower_fmt,
                                           dest_components);

   if (sparse) {
      /* Put the sparse component back on the original instruction */
      intrin->num_components++;
      intrin->def.num_components = intrin->num_components;

      /* Carry over the sparse component without modifying it with the
       * converted color.
       */
      nir_def *sparse_color[NIR_MAX_VEC_COMPONENTS];
      for (unsigned i = 0; i < dest_components; i++)
         sparse_color[i] = nir_channel(b, color, i);
      sparse_color[dest_components] =
         nir_channel(b, &intrin->def, intrin->num_components - 1);
      color = nir_vec(b, sparse_color, dest_components + 1);
   }

   nir_def_rewrite_uses(placeholder, color);
   nir_instr_remove(placeholder->parent_instr);

   return true;
}

static nir_def *
convert_color_for_store(nir_builder *b, const struct intel_device_info *devinfo,
                        nir_def *color,
                        enum isl_format image_fmt, enum isl_format lower_fmt)
{
   struct format_info image = get_format_info(image_fmt);

   color = nir_trim_vector(b, color, image.chans);

   assert(image_fmt == ISL_FORMAT_R64_PASSTHRU);
   assert(lower_fmt == ISL_FORMAT_R32G32_UINT);

   return nir_unpack_64_2x32(b, nir_channel(b, color, 0));
}

static bool
lower_image_store_instr(nir_builder *b,
                        const struct intel_device_info *devinfo,
                        nir_intrinsic_instr *intrin)
{
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   /* Only for image atomic64 emulation*/
   if (nir_src_bit_size(intrin->src[3]) != 64)
      return false;

   const enum isl_format image_fmt =
      isl_format_for_pipe_format(var->data.image.format);
   assert(isl_has_matching_typed_storage_image_format(devinfo, image_fmt));
   enum isl_format lower_fmt = isl_lower_storage_image_format(devinfo, image_fmt);

   /* Color conversion goes before the store */
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *color = convert_color_for_store(b, devinfo,
                                                intrin->src[3].ssa,
                                                image_fmt, lower_fmt);
   intrin->num_components = isl_format_get_num_channels(lower_fmt);
   nir_src_rewrite(&intrin->src[3], color);

   return true;
}

static bool
lower_image_atomic_instr(nir_builder *b,
                         const struct intel_device_info *devinfo,
                         nir_intrinsic_instr *intrin)
{
   if (intrin->def.bit_size != 64)
      return false;

   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);

   b->cursor = nir_instr_remove(&intrin->instr);

   /* Use an undef to hold the uses of the load conversion. */
   nir_def *placeholder = nir_undef(b, 4, 32);
   nir_def_rewrite_uses(&intrin->def, placeholder);
   nir_def *zero = nir_imm_zero(b, intrin->def.num_components,
                                    intrin->def.bit_size);

   nir_def *coord = intrin->src[1].ssa;
   nir_push_if(b, image_coord_is_in_bounds(b, deref, coord));
   nir_def *addr = image_address(b, devinfo, deref, coord);

    /* We have to fall all the way back to A64 messages */
   addr = nir_iadd(b, load_image_base_address(b, deref),
                         nir_u2u64(b, addr));

   /* Build the global atomic */
   nir_atomic_op atomic_op = nir_intrinsic_atomic_op(intrin);

   nir_def *global;
   if (intrin->intrinsic == nir_intrinsic_image_deref_atomic) {
      global = nir_global_atomic(b, intrin->def.bit_size, addr, intrin->src[3].ssa,
                                 .atomic_op = atomic_op);
   }
   else if (intrin->intrinsic == nir_intrinsic_image_deref_atomic_swap) {
      global = nir_global_atomic_swap(b, intrin->def.bit_size, addr, intrin->src[3].ssa,
                                      intrin->src[4].ssa, .atomic_op = atomic_op);
   }
   else {
      unreachable("Unsupported image intrinsic");
   }

   nir_pop_if(b, NULL);

   nir_def *result = nir_if_phi(b, global, zero);
   nir_def_rewrite_uses(placeholder, result);

   return true;
}

static bool
brw_nir_lower_storage_image_instr(nir_builder *b,
                                  nir_instr *instr,
                                  void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   const struct brw_nir_lower_storage_image_opts *opts = cb_data;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   switch (intrin->intrinsic) {
   case nir_intrinsic_image_deref_load:
      if (opts->lower_loads)
         return lower_image_load_instr(b, opts->devinfo, intrin, false);
      return false;

   case nir_intrinsic_image_deref_sparse_load:
      if (opts->lower_loads)
         return lower_image_load_instr(b, opts->devinfo, intrin, true);
      return false;

   case nir_intrinsic_image_deref_store:
      if (opts->lower_stores)
         return lower_image_store_instr(b, opts->devinfo, intrin);
      return false;

   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
      if (opts->lower_atomics)
         return lower_image_atomic_instr(b, opts->devinfo, intrin);
      return false;

   default:
      /* Nothing to do */
      return false;
   }
}

bool
brw_nir_lower_storage_image(nir_shader *shader,
                            const struct brw_nir_lower_storage_image_opts *opts)
{
   bool progress = false;

   const nir_lower_image_options image_options = {
      .lower_cube_size = true,
      .lower_image_samples_to_one = true,
   };

   progress |= nir_lower_image(shader, &image_options);

   progress |= nir_shader_instructions_pass(shader,
                                            brw_nir_lower_storage_image_instr,
                                            nir_metadata_none,
                                            (void *)opts);

   return progress;
}
