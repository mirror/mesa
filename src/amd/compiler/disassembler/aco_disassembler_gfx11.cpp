/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_disassembler.h"

namespace aco {

void
disasm_vopd(instr_context& ctx)
{
   print_opcode(ctx, Format::VOPD, bfe(ctx, 22, 4));
   print_operand(ctx, bfe(ctx, 56, 8) | vgpr, operand_index_def,
                 additional_operand_info{.min_count = 1});
   print_operand(ctx, bfe(ctx, 0, 9), 0, additional_operand_info{.min_count = 1});

   if ((aco_opcode)ctx.op == aco_opcode::v_dual_fmamk_f32) {
      fprintf(ctx.disasm->output, ", 0x%x", ctx.dwords[2]);
      ctx.total_size = 3;
      ctx.has_literal = true;
   }

   if ((aco_opcode)ctx.op != aco_opcode::v_dual_mov_b32)
      print_operand(ctx, bfe(ctx, 9, 8) | vgpr, 1, additional_operand_info{.min_count = 1});

   if ((aco_opcode)ctx.op == aco_opcode::v_dual_fmaak_f32) {
      fprintf(ctx.disasm->output, ", 0x%x", ctx.dwords[2]);
      ctx.total_size = 3;
      ctx.has_literal = true;
   }

   fprintf(ctx.disasm->output, " :: ");

   ctx.printed_operand = false;

   print_opcode(ctx, Format::VOPD, bfe(ctx, 17, 5));
   print_operand(ctx, (bfe(ctx, 49, 7) << 1) | (bfe(ctx, 56, 1) ? 0 : 1) | vgpr, operand_index_def,
                 additional_operand_info{.min_count = 1});
   print_operand(ctx, bfe(ctx, 32, 9), 0, additional_operand_info{.min_count = 1});

   if ((aco_opcode)ctx.op == aco_opcode::v_dual_fmamk_f32) {
      fprintf(ctx.disasm->output, ", 0x%x", ctx.dwords[2]);
      ctx.total_size = 3;
      ctx.has_literal = true;
   }

   if ((aco_opcode)ctx.op != aco_opcode::v_dual_mov_b32)
      print_operand(ctx, bfe(ctx, 41, 8) | vgpr, 1, additional_operand_info{.min_count = 1});

   if ((aco_opcode)ctx.op == aco_opcode::v_dual_fmaak_f32) {
      fprintf(ctx.disasm->output, ", 0x%x", ctx.dwords[2]);
      ctx.total_size = 3;
      ctx.has_literal = true;
   }
}

void
disasm_mubuf_gfx11(instr_context& ctx)
{
   print_opcode(ctx, Format::MUBUF, bfe(ctx, 18, 8));

   print_operand(
      ctx, bfe(ctx, 40, 8) | vgpr, ctx.has_def ? operand_index_def : 3,
      additional_operand_info{.min_count = mem_get_data_size(ctx), .tfe = !!bfe(ctx, 53, 1)});

   if (bfe(ctx, 54, 2)) {
      print_operand(ctx, bfe(ctx, 32, 8) | vgpr, 1,
                    additional_operand_info{.min_count = util_bitcount(bfe(ctx, 54, 2))});
   } else {
      if (ctx.printed_operand)
         fprintf(ctx.disasm->output, ",");
      fprintf(ctx.disasm->output, " off");
      ctx.printed_operand = true;
   }

   print_operand(ctx, bfe(ctx, 48, 5) << 2, 0, additional_operand_info{.min_count = 4});

   print_operand(ctx, bfe(ctx, 56, 8), 2, additional_operand_info{.min_count = 1});

   print_flag(ctx, " idxen", 55);
   print_flag(ctx, " offen", 54);

   if (bfe(ctx, 0, 12))
      fprintf(ctx.disasm->output, " offset:%u", bfe(ctx, 0, 12));

   print_flag(ctx, " glc", 14);
   print_flag(ctx, " dlc", 13);
   print_flag(ctx, " slc", 12);
   print_flag(ctx, " lds", 16);
   print_flag(ctx, " tfe", 53);
}

const char* formats_gfx11[] = {
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

   "BUF_FMT_10_11_11_FLOAT",

   "BUF_FMT_11_11_10_FLOAT",

   "BUF_FMT_10_10_10_2_UNORM",
   "BUF_FMT_10_10_10_2_SNORM",
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
disasm_mtbuf_gfx11(instr_context& ctx)
{
   print_opcode(ctx, Format::MTBUF, bfe(ctx, 15, 4));

   print_operand(
      ctx, bfe(ctx, 40, 8) | vgpr, ctx.has_def ? operand_index_def : 3,
      additional_operand_info{.min_count = mem_get_data_size(ctx), .tfe = !!bfe(ctx, 53, 1)});

   if (bfe(ctx, 54, 2)) {
      print_operand(ctx, bfe(ctx, 32, 8) | vgpr, 1,
                    additional_operand_info{.min_count = util_bitcount(bfe(ctx, 54, 2))});
   } else {
      fprintf(ctx.disasm->output, ", off");
   }

   print_operand(ctx, bfe(ctx, 48, 5) << 2, 0, additional_operand_info{.min_count = 4});
   print_operand(ctx, bfe(ctx, 56, 8), 2, additional_operand_info{.min_count = 1});

   if (bfe(ctx, 19, 7) != 1)
      fprintf(ctx.disasm->output, " format:[%s]", formats_gfx11[bfe(ctx, 19, 7)]);

   print_flag(ctx, " idxen", 55);
   print_flag(ctx, " offen", 54);

   if (bfe(ctx, 0, 12))
      fprintf(ctx.disasm->output, " offset:%u", bfe(ctx, 0, 12));

   print_flag(ctx, " glc", 14);
   print_flag(ctx, " dlc", 13);
   print_flag(ctx, " slc", 12);
   print_flag(ctx, " tfe", 53);
}

void
disasm_mimg_gfx11(instr_context& ctx)
{
   print_opcode(ctx, Format::MIMG, bfe(ctx, 18, 8));

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
   if (bfe(ctx, 53, 1))
      data_components++;

   bool d16 = !!bfe(ctx, 17, 1);
   print_operand(ctx, bfe(ctx, 40, 8) | vgpr, 2,
                 additional_operand_info{.count = DIV_ROUND_UP(data_components, d16 ? 2 : 1)});

   bool nsa = !!bfe(ctx, 0, 1);
   bool a16 = !!bfe(ctx, 16, 1);
   uint32_t coord_components =
      get_mimg_coord_components(ctx, info, (ac_image_dim)bfe(ctx, 2, 3), a16);
   if (nsa) {
      fprintf(ctx.disasm->output, ", [");
      print_operand(ctx, bfe(ctx, 32, 8) | vgpr, 0,
                    additional_operand_info{
                       .skip_comma = true,
                       .count = (mimg_op == aco_mimg_op_info::bvh64 ? 2u : 1u),
                    });
      if (mimg_op == aco_mimg_op_info::bvh || mimg_op == aco_mimg_op_info::bvh64) {
         for (uint32_t i = 0; i < (a16 ? 3 : 4); i++) {
            uint32_t count = i > 0 ? 3 : 1;
            print_operand(ctx, bfe(ctx, 64 + i * 8, 8) | vgpr, 0,
                          additional_operand_info{.count = count});
         }
      } else {
         for (uint32_t i = 0; i < MIN2(4, coord_components - 1); i++) {
            print_operand(
               ctx, bfe(ctx, 64 + i * 8, 8) | vgpr, 0,
               additional_operand_info{.count = (i == 3) ? coord_components - 1 - i : 1});
         }
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
      print_operand(ctx, bfe(ctx, 58, 5) << 2, 1, additional_operand_info{.count = 4});

   bool is_bvh = mimg_op == aco_mimg_op_info::bvh || mimg_op == aco_mimg_op_info::bvh64;

   if (!is_bvh) {
      fprintf(ctx.disasm->output, " dmask:0x%x", dmask);
      print_mimg_dim(ctx, (ac_image_dim)bfe(ctx, 2, 3));

      print_flag(ctx, " lwe", 54);
      print_flag(ctx, " unorm", 7);
   }

   print_flag(ctx, " dlc", 13);
   print_flag(ctx, " glc", 14);
   print_flag(ctx, " slc", 12);
   print_flag(ctx, " a16", 16);

   if (!is_bvh) {
      print_flag(ctx, " d16", 17);
      print_flag(ctx, " tfe", 53);
   }

   if (is_bvh) {
      require_eq(ctx, dmask, dmask, 0xf);
      require_eq(ctx, d16, bfe(ctx, 17, 1), 0);
      require_eq(ctx, r128, bfe(ctx, 15, 1), 1);
      require_eq(ctx, unorm, bfe(ctx, 7, 1), 1);
      require_eq(ctx, dim, bfe(ctx, 2, 3), 0);
      require_eq(ctx, lwe, bfe(ctx, 54, 1), 0);
      require_eq(ctx, tfe, bfe(ctx, 53, 1), 0);
      require_eq(ctx, ssamp, bfe(ctx, 58, 5), 0);
   }
}

void
disasm_flatlike_gfx11(instr_context& ctx)
{
   uint32_t seg = bfe(ctx, 16, 2);
   Format format = Format::FLAT;
   if (seg == 1)
      format = Format::SCRATCH;
   else if (seg == 2)
      format = Format::GLOBAL;

   print_opcode(ctx, format, bfe(ctx, 18, 7));

   if (mem_has_dst(ctx) || (mem_has_conditional_dst(ctx) && bfe(ctx, 14, 1)))
      print_definition(ctx, bfe(ctx, 56, 8) | vgpr);

   uint32_t saddr = bfe(ctx, 48, 7);
   bool use_saddr =
      saddr != 0x7F && parse_reg_src(ctx, saddr) != sgpr_null && format != Format::FLAT;
   if (format == Format::SCRATCH && !bfe(ctx, 55, 1)) {
      if (ctx.printed_operand)
         fprintf(ctx.disasm->output, ",");
      fprintf(ctx.disasm->output, " off");
      ctx.printed_operand = true;
   } else {
      print_operand(
         ctx, bfe(ctx, 32, 8) | vgpr, 0,
         additional_operand_info{.count = (format == Format::SCRATCH || use_saddr) ? 1u : 2u});
   }

   if (mem_has_data(ctx)) {
      uint32_t data_size = std::max(1u, mem_get_data_size(ctx));
      if (mem_has_data2(ctx))
         data_size *= 2;
      print_operand(ctx, bfe(ctx, 40, 8) | vgpr, 1, additional_operand_info{.count = data_size});
   }

   if (use_saddr) {
      print_operand(ctx, saddr, 2,
                    additional_operand_info{.count = (format == Format::SCRATCH) ? 1u : 2u});
   } else if (format != Format::FLAT) {
      fprintf(ctx.disasm->output, ", off");
   }

   if (bfe(ctx, 0, 13)) {
      if (format == Format::FLAT)
         fprintf(ctx.disasm->output, " offset:%u", bfe(ctx, 0, 12));
      else
         fprintf(ctx.disasm->output, " offset:%i", u2i(bfe(ctx, 0, 13), 13));
   }

   print_flag(ctx, " glc", 14);
   print_flag(ctx, " dlc", 13);
   print_flag(ctx, " slc", 15);
}

void
disasm_vinterp(instr_context& ctx)
{
   print_opcode(ctx, Format::VINTERP_INREG, bfe(ctx, 16, 7));

   print_definition(ctx, bfe(ctx, 0, 8) | vgpr);

   print_operand(ctx, bfe(ctx, 32, 9), 0, additional_operand_info{.neg = !!bfe(ctx, 61, 1)});

   print_operand(ctx, bfe(ctx, 41, 9), 1, additional_operand_info{.neg = !!bfe(ctx, 62, 1)});

   print_operand(ctx, bfe(ctx, 50, 9), 2, additional_operand_info{.neg = !!bfe(ctx, 63, 1)});

   bool has_opsel = true;
   if (has_opsel) {
      uint32_t opsel[4] = {bfe(ctx, 11, 1), bfe(ctx, 12, 1), bfe(ctx, 13, 1), bfe(ctx, 14, 1)};
      print_integer_array(ctx, "op_sel", opsel, 4, 0);
   }

   print_flag(ctx, " clamp", 15);

   fprintf(ctx.disasm->output, " wait_exp:%u", bfe(ctx, 8, 3));
}

void
disasm_ldsdir(instr_context& ctx)
{
   print_opcode(ctx, Format::LDSDIR, bfe(ctx, 20, 2));

   print_operand(ctx, bfe(ctx, 0, 8) | vgpr, operand_index_def,
                 additional_operand_info{.min_count = 1});

   if ((aco_opcode)ctx.op == aco_opcode::lds_param_load) {
      char channels[] = {'x', 'y', 'z', 'w'};
      fprintf(ctx.disasm->output, ", attr%u.%c", bfe(ctx, 10, 6), channels[bfe(ctx, 8, 2)]);
   }

   if (ctx.disasm->program->gfx_level >= GFX12) {
      fprintf(ctx.disasm->output, " wait_va_vdst:%u", bfe(ctx, 16, 4));
      fprintf(ctx.disasm->output, " wait_vm_vsrc:%u", bfe(ctx, 23, 1));
   } else {
      fprintf(ctx.disasm->output, " wait_vdst:%u", bfe(ctx, 16, 4));
   }
}

} // namespace aco
