/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_disassembler.h"

namespace aco {

void
disasm_smem_gfx10(instr_context& ctx)
{
   print_opcode(ctx, Format::SMEM,
                ctx.disasm->program->gfx_level <= GFX11_5 ? bfe(ctx, 18, 8) : bfe(ctx, 13, 8));

   print_definition(ctx, bfe(ctx, 6, 7));

   if ((aco_opcode)ctx.op == aco_opcode::s_memtime || (aco_opcode)ctx.op == aco_opcode::s_memtime ||
       (aco_opcode)ctx.op == aco_opcode::s_dcache_inv ||
       (aco_opcode)ctx.op == aco_opcode::s_dcache_inv_vol)
      return;

   if (smem_buffer_ops.count((aco_opcode)ctx.op))
      print_operand(ctx, bfe(ctx, 0, 6) << 1, 0, additional_operand_info{.count = 4});
   else
      print_operand(ctx, bfe(ctx, 0, 6) << 1, 0, additional_operand_info{.count = 2});

   uint32_t offset = bfe(ctx, 32, 21);

   bool printed_soffset =
      print_operand(ctx, bfe(ctx, 57, 7), 1, additional_operand_info{.skip_null = !!offset});

   /* LLVM consistent printing :) */
   if (offset) {
      if (printed_soffset)
         fprintf(ctx.disasm->output, " offset:0x%x", u2i(offset, 21));
      else
         fprintf(ctx.disasm->output, ", 0x%x", u2i(offset, 21));
   }

   if (ctx.disasm->program->gfx_level < GFX12) {
      print_flag(ctx, " dlc", ctx.disasm->program->gfx_level >= GFX11 ? 13 : 14);
      print_flag(ctx, " glc", ctx.disasm->program->gfx_level >= GFX11 ? 14 : 16);
   } else {
      print_cache_flags_gfx12(ctx, 21);
   }
}

static const char* formats_gfx10[] = {
   "BUF_FMT_INVALID",

   "BUF_FMT_8_UNORM",
   "BUF_FMT_8_SNORM",
   "BUF_FMT_8_USCALED",
   "BUF_FMT_8_SSCALED",
   "BUF_FMT_8_UINT",
   "BUF_FMT_8_SINT",

   "BUF_FMT_16_UNORM",
   "BUF_FMT_16_SNORM",
   "BUF_FMT_16_USCALED",
   "BUF_FMT_16_SSCALED",
   "BUF_FMT_16_UINT",
   "BUF_FMT_16_SINT",
   "BUF_FMT_16_FLOAT",

   "BUF_FMT_8_8_UNORM",
   "BUF_FMT_8_8_SNORM",
   "BUF_FMT_8_8_USCALED",
   "BUF_FMT_8_8_SSCALED",
   "BUF_FMT_8_8_UINT",
   "BUF_FMT_8_8_SINT",

   "BUF_FMT_32_UINT",
   "BUF_FMT_32_SINT",
   "BUF_FMT_32_FLOAT",

   "BUF_FMT_16_16_UNORM",
   "BUF_FMT_16_16_SNORM",
   "BUF_FMT_16_16_USCALED",
   "BUF_FMT_16_16_SSCALED",
   "BUF_FMT_16_16_UINT",
   "BUF_FMT_16_16_SINT",
   "BUF_FMT_16_16_FLOAT",

   "BUF_FMT_10_11_11_UNORM",
   "BUF_FMT_10_11_11_SNORM",
   "BUF_FMT_10_11_11_USCALED",
   "BUF_FMT_10_11_11_SSCALED",
   "BUF_FMT_10_11_11_UINT",
   "BUF_FMT_10_11_11_SINT",
   "BUF_FMT_10_11_11_FLOAT",

   "BUF_FMT_11_11_10_UNORM",
   "BUF_FMT_11_11_10_SNORM",
   "BUF_FMT_11_11_10_USCALED",
   "BUF_FMT_11_11_10_SSCALED",
   "BUF_FMT_11_11_10_UINT",
   "BUF_FMT_11_11_10_SINT",
   "BUF_FMT_11_11_10_FLOAT",

   "BUF_FMT_10_10_10_2_UNORM",
   "BUF_FMT_10_10_10_2_SNORM",
   "BUF_FMT_10_10_10_2_USCALED",
   "BUF_FMT_10_10_10_2_SSCALED",
   "BUF_FMT_10_10_10_2_UINT",
   "BUF_FMT_10_10_10_2_SINT",

   "BUF_FMT_2_10_10_10_UNORM",
   "BUF_FMT_2_10_10_10_SNORM",
   "BUF_FMT_2_10_10_10_USCALED",
   "BUF_FMT_2_10_10_10_SSCALED",
   "BUF_FMT_2_10_10_10_UINT",
   "BUF_FMT_2_10_10_10_SINT",

   "BUF_FMT_8_8_8_8_UNORM",
   "BUF_FMT_8_8_8_8_SNORM",
   "BUF_FMT_8_8_8_8_USCALED",
   "BUF_FMT_8_8_8_8_SSCALED",
   "BUF_FMT_8_8_8_8_UINT",
   "BUF_FMT_8_8_8_8_SINT",

   "BUF_FMT_32_32_UINT",
   "BUF_FMT_32_32_SINT",
   "BUF_FMT_32_32_FLOAT",

   "BUF_FMT_16_16_16_16_UNORM",
   "BUF_FMT_16_16_16_16_SNORM",
   "BUF_FMT_16_16_16_16_USCALED",
   "BUF_FMT_16_16_16_16_SSCALED",
   "BUF_FMT_16_16_16_16_UINT",
   "BUF_FMT_16_16_16_16_SINT",
   "BUF_FMT_16_16_16_16_FLOAT",

   "BUF_FMT_32_32_32_UINT",
   "BUF_FMT_32_32_32_SINT",
   "BUF_FMT_32_32_32_FLOAT",
   "BUF_FMT_32_32_32_32_UINT",
   "BUF_FMT_32_32_32_32_SINT",
   "BUF_FMT_32_32_32_32_FLOAT",
};

void
disasm_mtbuf_gfx10(instr_context& ctx)
{
   print_opcode(ctx, Format::MTBUF, bfe(ctx, 16, 3) | (bfe(ctx, 53, 1) << 3));

   print_operand(
      ctx, bfe(ctx, 40, 8) | vgpr, ctx.has_def ? operand_index_def : 3,
      additional_operand_info{.min_count = mem_get_data_size(ctx), .tfe = !!bfe(ctx, 55, 1)});

   if (bfe(ctx, 12, 2)) {
      print_operand(ctx, bfe(ctx, 32, 8) | vgpr, 1,
                    additional_operand_info{.min_count = util_bitcount(bfe(ctx, 12, 2))});
   } else {
      fprintf(ctx.disasm->output, ", off");
   }

   print_operand(ctx, bfe(ctx, 48, 5) << 2, 0, additional_operand_info{.min_count = 4});
   print_operand(ctx, bfe(ctx, 56, 8), 2, additional_operand_info{.min_count = 1});

   if (bfe(ctx, 19, 7) != 1)
      fprintf(ctx.disasm->output, " format:[%s]", formats_gfx10[bfe(ctx, 19, 7)]);

   print_flag(ctx, " idxen", 13);
   print_flag(ctx, " offen", 12);

   if (bfe(ctx, 0, 12))
      fprintf(ctx.disasm->output, " offset:%u", bfe(ctx, 0, 12));

   print_flag(ctx, " glc", 14);
   print_flag(ctx, " dlc", 15);
   print_flag(ctx, " slc", 54);
   print_flag(ctx, " tfe", 55);
}

void
disasm_mimg_gfx10(instr_context& ctx)
{
   print_opcode(ctx, Format::MIMG, bfe(ctx, 18, 7) | (bfe(ctx, 0, 1) << 7));

   aco_mimg_op_info info = (aco_mimg_op_info)instr_info.mimg_infos.at((aco_opcode)ctx.op);
   aco_mimg_op_info mimg_op = aco_mimg_op_info_get_op(info);

   uint32_t dmask = bfe(ctx, 8, 4);
   uint32_t data_components;
   switch (mimg_op) {
   case aco_mimg_op_info::msaa_load:
   case aco_mimg_op_info::gather4: data_components = 4; break;
   case aco_mimg_op_info::atomic: data_components = 1; break;
   default: data_components = util_bitcount(dmask); break;
   }
   if (bfe(ctx, 16, 1))
      data_components++;

   bool d16 = !!bfe(ctx, 63, 1);
   print_operand(ctx, bfe(ctx, 40, 8) | vgpr, 2,
                 additional_operand_info{.count = DIV_ROUND_UP(data_components, d16 ? 2 : 1)});

   uint32_t nsa = bfe(ctx, 1, 2);
   uint32_t coord_components =
      get_mimg_coord_components(ctx, info, (ac_image_dim)bfe(ctx, 3, 3), !!bfe(ctx, 62, 1));
   if (nsa > 0) {
      fprintf(ctx.disasm->output, ", [");
      print_operand(ctx, bfe(ctx, 32, 8) | vgpr, 0, additional_operand_info{.skip_comma = true});
      for (uint32_t i = 0; i < MIN2(nsa * 4, coord_components - 1); i++) {
         print_operand(ctx, bfe(ctx, 64 + i * 8, 8) | vgpr, 0);
      }
      fprintf(ctx.disasm->output, "]");
   } else {
      print_operand(ctx, bfe(ctx, 32, 8) | vgpr, 0,
                    additional_operand_info{.count = coord_components});
   }
   ctx.total_size += nsa;

   print_operand(ctx, bfe(ctx, 48, 5) << 2, 0,
                 additional_operand_info{.count = bfe(ctx, 15, 1) ? 4u : 8u});

   if (mimg_op == aco_mimg_op_info::get_lod || mimg_op == aco_mimg_op_info::sample ||
       mimg_op == aco_mimg_op_info::gather4)
      print_operand(ctx, bfe(ctx, 53, 5) << 2, 1, additional_operand_info{.count = 4});

   bool is_bvh = mimg_op == aco_mimg_op_info::bvh || mimg_op == aco_mimg_op_info::bvh64;

   if (!is_bvh) {
      fprintf(ctx.disasm->output, " dmask:0x%x", dmask);
      print_mimg_dim(ctx, (ac_image_dim)bfe(ctx, 3, 3));

      print_flag(ctx, " lwe", 17);
      print_flag(ctx, " unorm", 12);
   }

   print_flag(ctx, " dlc", 7);
   print_flag(ctx, " glc", 13);
   print_flag(ctx, " slc", 25);
   print_flag(ctx, " a16", 62);

   if (!is_bvh) {
      print_flag(ctx, " d16", 63);
      print_flag(ctx, " tfe", 16);
   }

   if (is_bvh) {
      require_eq(ctx, dmask, dmask, 0xf);
      require_eq(ctx, d16, bfe(ctx, 63, 1), 0);
      require_eq(ctx, r128, bfe(ctx, 15, 1), 1);
      require_eq(ctx, unorm, bfe(ctx, 12, 1), 1);
      require_eq(ctx, dim, bfe(ctx, 3, 3), 0);
      require_eq(ctx, lwe, bfe(ctx, 17, 1), 0);
      require_eq(ctx, tfe, bfe(ctx, 16, 1), 0);
      require_eq(ctx, ssamp, bfe(ctx, 53, 5), 0);
   }
}

} // namespace aco
