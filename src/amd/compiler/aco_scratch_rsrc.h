/*
 * Copyright © 2024 Valve Corporation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_builder.h"
#include "aco_ir.h"

#include "ac_descriptors.h"
#include "amdgfxregs.h"

#ifndef ACO_SCRATCH_RSRC_H
#define ACO_SCRATCH_RSRC_H

namespace aco {

inline Temp
load_scratch_resource(Program* program, Builder& bld, bool apply_scratch_offset,
                      bool apply_stack_ptr)
{
   Temp private_segment_buffer = program->private_segment_buffer;
   if (!private_segment_buffer.bytes()) {
      Temp addr_lo =
         bld.sop1(aco_opcode::p_load_symbol, bld.def(s1), Operand::c32(aco_symbol_scratch_addr_lo));
      Temp addr_hi =
         bld.sop1(aco_opcode::p_load_symbol, bld.def(s1), Operand::c32(aco_symbol_scratch_addr_hi));
      private_segment_buffer =
         bld.pseudo(aco_opcode::p_create_vector, bld.def(s2), addr_lo, addr_hi);
   } else if (program->stage.hw != AC_HW_COMPUTE_SHADER) {
      private_segment_buffer =
         bld.smem(aco_opcode::s_load_dwordx2, bld.def(s2), private_segment_buffer, Operand::zero());
   }

   if ((apply_stack_ptr && program->stack_ptr != Temp()) || apply_scratch_offset) {
      Temp addr_lo = bld.tmp(s1);
      Temp addr_hi = bld.tmp(s1);
      bld.pseudo(aco_opcode::p_split_vector, Definition(addr_lo), Definition(addr_hi),
                 private_segment_buffer);

      if (apply_stack_ptr && program->stack_ptr != Temp()) {
         Temp carry = bld.tmp(s1);
         addr_lo = bld.sop2(aco_opcode::s_add_u32, bld.def(s1), bld.scc(Definition(carry)), addr_lo,
                            program->stack_ptr);
         addr_hi = bld.sop2(aco_opcode::s_addc_u32, bld.def(s1), bld.def(s1, scc), addr_hi,
                            Operand::c32(0), bld.scc(carry));
      }

      if (apply_scratch_offset) {
         Temp carry = bld.tmp(s1);
         addr_lo = bld.sop2(aco_opcode::s_add_u32, bld.def(s1), bld.scc(Definition(carry)), addr_lo,
                            program->scratch_offset);
         addr_hi = bld.sop2(aco_opcode::s_addc_u32, bld.def(s1), bld.def(s1, scc), addr_hi,
                            Operand::c32(0), bld.scc(carry));
      }

      private_segment_buffer =
         bld.pseudo(aco_opcode::p_create_vector, bld.def(s2), addr_lo, addr_hi);
   }

   struct ac_buffer_state ac_state = {0};
   uint32_t desc[4];

   ac_state.size = 0xffffffff;
   ac_state.format = PIPE_FORMAT_R32_FLOAT;
   for (int i = 0; i < 4; i++)
      ac_state.swizzle[i] = PIPE_SWIZZLE_0;
   /* older generations need element size = 4 bytes. element size removed in GFX9 */
   ac_state.element_size = program->gfx_level <= GFX8 ? 1u : 0u;
   ac_state.index_stride = program->wave_size == 64 ? 3u : 2u;
   ac_state.add_tid = true;
   ac_state.gfx10_oob_select = V_008F0C_OOB_SELECT_RAW;

   ac_build_buffer_descriptor(program->gfx_level, &ac_state, desc);

   return bld.pseudo(aco_opcode::p_create_vector, bld.def(s4), private_segment_buffer,
                     Operand::c32(desc[2]), Operand::c32(desc[3]));
}

} // namespace aco

#endif // ACO_SCRATCH_RSRC_H
