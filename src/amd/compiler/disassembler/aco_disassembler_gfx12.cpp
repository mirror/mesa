/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_disassembler.h"

namespace aco {

void
print_cache_flags_gfx12(instr_context& ctx, uint32_t bit)
{
   uint32_t packed = bfe(ctx, bit, 5);

   uint32_t scope = packed & 0x3;
   if (scope) {
      switch (scope) {
      case gfx12_scope_cu: fprintf(ctx.disasm->output, " scope:SCOPE_CU"); break;
      case gfx12_scope_se: fprintf(ctx.disasm->output, " scope:SCOPE_SE"); break;
      case gfx12_scope_device: fprintf(ctx.disasm->output, " scope:SCOPE_DEV"); break;
      case gfx12_scope_memory: fprintf(ctx.disasm->output, " scope:SCOPE_SYS"); break;
      }
   }

   uint32_t temporal_hint = packed >> 2;
   if (temporal_hint) {
      if (instr_info.is_atomic[ctx.op]) {
         fprintf(ctx.disasm->output, " th:TH_ATOMIC");
         if (temporal_hint & gfx12_atomic_accum_deferred_scope)
            fprintf(ctx.disasm->output, "_CASCADE");
         if (temporal_hint & gfx12_atomic_non_temporal)
            fprintf(ctx.disasm->output, "_NT");
         if (temporal_hint & gfx12_atomic_return)
            fprintf(ctx.disasm->output, "_RETURN");
      } else if (mem_has_data(ctx)) {
         switch (temporal_hint) {
         case gfx12_store_non_temporal: fprintf(ctx.disasm->output, " th:TH_STORE_NT"); break;
         case gfx12_store_high_temporal: fprintf(ctx.disasm->output, " th:TH_STORE_HT"); break;
         case gfx12_store_high_temporal_stay_dirty:
            fprintf(ctx.disasm->output, " th:TH_STORE_RT_WB");
            break;
         case gfx12_store_near_non_temporal_far_regular_temporal:
            fprintf(ctx.disasm->output, " th:TH_STORE_NT_RT");
            break;
         case gfx12_store_near_regular_temporal_far_non_temporal:
            fprintf(ctx.disasm->output, " th:TH_STORE_RT_NT");
            break;
         case gfx12_store_near_non_temporal_far_high_temporal:
            fprintf(ctx.disasm->output, " th:TH_STORE_NT_HT");
            break;
         case gfx12_store_near_non_temporal_far_writeback:
            fprintf(ctx.disasm->output, " th:TH_STORE_NT_WB");
            break;
         }
      } else {
         switch (temporal_hint) {
         case gfx12_load_non_temporal: fprintf(ctx.disasm->output, " th:TH_LOAD_NT"); break;
         case gfx12_load_high_temporal: fprintf(ctx.disasm->output, " th:TH_LOAD_HT"); break;
         case gfx12_load_last_use_discard: fprintf(ctx.disasm->output, " th:TH_LOAD_LU"); break;
         case gfx12_load_near_non_temporal_far_regular_temporal:
            fprintf(ctx.disasm->output, " th:TH_LOAD_NT_RT");
            break;
         case gfx12_load_near_regular_temporal_far_non_temporal:
            fprintf(ctx.disasm->output, " th:TH_LOAD_RT_NT");
            break;
         case gfx12_load_near_non_temporal_far_high_temporal:
            fprintf(ctx.disasm->output, " th:TH_LOAD_NT_HT");
            break;
         }
      }
   }
}

void
disasm_mubuf_gfx12(instr_context& ctx)
{
   print_opcode(ctx, Format::MUBUF, bfe(ctx, 14, 8));

   print_operand(
      ctx, bfe(ctx, 32, 8) | vgpr, ctx.has_def ? operand_index_def : 3,
      additional_operand_info{.min_count = mem_get_data_size(ctx), .tfe = !!bfe(ctx, 22, 1)});

   if (bfe(ctx, 62, 2)) {
      print_operand(ctx, bfe(ctx, 64, 8) | vgpr, 1,
                    additional_operand_info{.min_count = util_bitcount(bfe(ctx, 62, 2))});
   } else {
      if (ctx.printed_operand)
         fprintf(ctx.disasm->output, ",");
      fprintf(ctx.disasm->output, " off");
      ctx.printed_operand = true;
   }

   print_operand(ctx, bfe(ctx, 41, 7), 0, additional_operand_info{.min_count = 4});

   print_operand(ctx, bfe(ctx, 0, 8), 2, additional_operand_info{.min_count = 1});

   print_flag(ctx, " idxen", 63);
   print_flag(ctx, " offen", 62);

   if (bfe(ctx, 72, 24))
      fprintf(ctx.disasm->output, " offset:%u", bfe(ctx, 72, 24));

   print_flag(ctx, " tfe", 22);

   print_cache_flags_gfx12(ctx, 50);
}

void
disasm_mtbuf_gfx12(instr_context& ctx)
{
   print_opcode(ctx, Format::MTBUF, bfe(ctx, 14, 4));

   print_operand(
      ctx, bfe(ctx, 32, 8) | vgpr, ctx.has_def ? operand_index_def : 3,
      additional_operand_info{.min_count = mem_get_data_size(ctx), .tfe = !!bfe(ctx, 22, 1)});

   if (bfe(ctx, 62, 2)) {
      print_operand(ctx, bfe(ctx, 64, 8) | vgpr, 1,
                    additional_operand_info{.min_count = util_bitcount(bfe(ctx, 62, 2))});
   } else {
      fprintf(ctx.disasm->output, ", off");
   }

   print_operand(ctx, bfe(ctx, 41, 7), 0, additional_operand_info{.min_count = 4});
   print_operand(ctx, bfe(ctx, 0, 8), 2, additional_operand_info{.min_count = 1});

   if (bfe(ctx, 19, 7) != 1)
      fprintf(ctx.disasm->output, " format:[%s]", formats_gfx11[bfe(ctx, 55, 7)]);

   print_flag(ctx, " idxen", 63);
   print_flag(ctx, " offen", 62);

   if (bfe(ctx, 72, 24))
      fprintf(ctx.disasm->output, " offset:%u", bfe(ctx, 72, 24));

   print_flag(ctx, " tfe", 22);

   print_cache_flags_gfx12(ctx, 50);
}

void
disasm_mimg_gfx12(instr_context& ctx)
{
   print_opcode(ctx, Format::MIMG, bfe(ctx, 14, 8));

   uint32_t encoding = bfe(ctx, 26, 6);
   bool vsample = encoding == 0b111001;
   uint32_t tfe_bit = vsample ? 3 : 55;

   aco_mimg_op_info info = (aco_mimg_op_info)instr_info.mimg_infos.at((aco_opcode)ctx.op);
   aco_mimg_op_info mimg_op = aco_mimg_op_info_get_op(info);

   uint32_t dmask = bfe(ctx, 22, 4);
   uint32_t data_components;
   switch (mimg_op) {
   case aco_mimg_op_info::msaa_load:
   case aco_mimg_op_info::gather4: data_components = 4; break;
   case aco_mimg_op_info::atomic: data_components = 1; break;
   default: data_components = util_bitcount(dmask); break;
   }
   if (bfe(ctx, tfe_bit, 1))
      data_components++;

   bool d16 = !!bfe(ctx, 5, 1);
   print_operand(ctx, bfe(ctx, 32, 8) | vgpr, 2,
                 additional_operand_info{.count = DIV_ROUND_UP(data_components, d16 ? 2 : 1)});

   bool a16 = !!bfe(ctx, 6, 1);
   uint32_t coord_components =
      get_mimg_coord_components(ctx, info, (ac_image_dim)bfe(ctx, 0, 3), a16);

   if (coord_components > 1)
      fprintf(ctx.disasm->output, ", [");

   print_operand(ctx, bfe(ctx, 64, 8) | vgpr, 0,
                 additional_operand_info{
                    .skip_comma = coord_components > 1,
                    .count = (mimg_op == aco_mimg_op_info::bvh64 ? 2u : 1u),
                 });

   uint32_t vaddr[4] = {bfe(ctx, 72, 8), bfe(ctx, 80, 8), bfe(ctx, 88, 8), bfe(ctx, 56, 8)};
   if (mimg_op == aco_mimg_op_info::bvh || mimg_op == aco_mimg_op_info::bvh64) {
      for (uint32_t i = 0; i < (a16 ? 3 : 4); i++) {
         uint32_t count = i > 0 ? 3 : 1;
         print_operand(ctx, vaddr[i] | vgpr, 0, additional_operand_info{.count = count});
      }
   } else {
      for (uint32_t i = 0; i < MIN2(4, coord_components - 1); i++) {
         print_operand(ctx, vaddr[i] | vgpr, 0,
                       additional_operand_info{.count = (i == 3) ? coord_components - 1 - i : 1});
      }
   }

   if (coord_components > 1)
      fprintf(ctx.disasm->output, "]");

   print_operand(ctx, bfe(ctx, 41, 7), 0,
                 additional_operand_info{.count = bfe(ctx, 4, 1) ? 4u : 8u});

   if (vsample && (aco_opcode)ctx.op != aco_opcode::image_msaa_load)
      print_operand(ctx, bfe(ctx, 55, 7), 1, additional_operand_info{.count = 4});

   bool is_bvh = mimg_op == aco_mimg_op_info::bvh || mimg_op == aco_mimg_op_info::bvh64;

   if (!is_bvh) {
      fprintf(ctx.disasm->output, " dmask:0x%x", dmask);
      print_mimg_dim(ctx, (ac_image_dim)bfe(ctx, 0, 3));
      print_flag(ctx, " unorm", 13);
   }

   if (vsample)
      print_flag(ctx, " lwe", 40);

   print_flag(ctx, " a16", 6);

   if (!is_bvh) {
      print_flag(ctx, " d16", 5);
      print_flag(ctx, " tfe", tfe_bit);
   }

   if (is_bvh) {
      require_eq(ctx, dmask, dmask, 0xf);
      require_eq(ctx, d16, bfe(ctx, 5, 1), 0);
      require_eq(ctx, r128, bfe(ctx, 4, 1), 1);
      require_eq(ctx, dim, bfe(ctx, 0, 3), 0);
      require_eq(ctx, tfe, bfe(ctx, tfe_bit, 1), 0);
   }

   print_cache_flags_gfx12(ctx, 50);
}

void
disasm_flatlike_gfx12(instr_context& ctx)
{
   uint32_t seg = bfe(ctx, 24, 2);
   Format format = Format::FLAT;
   if (seg == 1)
      format = Format::SCRATCH;
   else if (seg == 2)
      format = Format::GLOBAL;

   print_opcode(ctx, format, bfe(ctx, 14, 7));

   if (mem_has_dst(ctx) || (mem_has_conditional_dst(ctx) && bfe(ctx, 14, 1)))
      print_definition(ctx, bfe(ctx, 32, 8) | vgpr);

   uint32_t saddr = bfe(ctx, 0, 7);
   bool use_saddr =
      saddr != 0x7F && parse_reg_src(ctx, saddr) != sgpr_null && format != Format::FLAT;
   if (format == Format::SCRATCH && !bfe(ctx, 49, 1)) {
      if (ctx.printed_operand)
         fprintf(ctx.disasm->output, ",");
      fprintf(ctx.disasm->output, " off");
      ctx.printed_operand = true;
   } else {
      print_operand(
         ctx, bfe(ctx, 64, 8) | vgpr, 0,
         additional_operand_info{.count = (format == Format::SCRATCH || use_saddr) ? 1u : 2u});
   }

   if (mem_has_data(ctx)) {
      uint32_t data_size = std::max(1u, mem_get_data_size(ctx));
      if (mem_has_data2(ctx))
         data_size *= 2;
      print_operand(ctx, bfe(ctx, 55, 8) | vgpr, 1, additional_operand_info{.count = data_size});
   }

   if (use_saddr) {
      print_operand(ctx, saddr, 2,
                    additional_operand_info{.count = (format == Format::SCRATCH) ? 1u : 2u});
   } else if (format != Format::FLAT) {
      fprintf(ctx.disasm->output, ", off");
   }

   if (bfe(ctx, 72, 24)) {
      if (format == Format::FLAT)
         fprintf(ctx.disasm->output, " offset:%u", bfe(ctx, 72, 24));
      else
         fprintf(ctx.disasm->output, " offset:%i", u2i(bfe(ctx, 72, 24), 24));
   }

   print_cache_flags_gfx12(ctx, 50);
}

} // namespace aco
