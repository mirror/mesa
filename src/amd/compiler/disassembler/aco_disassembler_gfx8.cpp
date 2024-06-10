/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_disassembler.h"

namespace aco {

void
disasm_smem_gfx8(instr_context& ctx)
{
   print_opcode(ctx, Format::SMEM, bfe(ctx, 18, 8));

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

   bool printed_soffset = false;
   if (bfe(ctx, 14, 1)) {
      printed_soffset =
         print_operand(ctx, bfe(ctx, 57, 7), 1, additional_operand_info{.skip_null = !!offset});
   }

   if (bfe(ctx, 17, 1)) {
      /* LLVM consistent printing :) */
      if (printed_soffset)
         fprintf(ctx.disasm->output, " offset:0x%x", u2i(offset, 21));
      else
         fprintf(ctx.disasm->output, ", 0x%x", u2i(offset, 21));
   } else {
      print_operand(ctx, bfe(offset, 0, 7), 1, additional_operand_info{.count = 1});
   }

   print_flag(ctx, " nv", 15);
   print_flag(ctx, " glc", 16);
}

} // namespace aco
