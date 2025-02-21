/*
 * Copyright © 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_builder.h"
#include "aco_ir.h"

#include <set>
#include <unordered_set>

namespace aco {

struct postdom_info {
   unsigned logical_imm_postdom;
   unsigned linear_imm_postdom;
};

struct spill_preserved_ctx {
   Program* program;
   SparseRegisterSet abi_preserved_range;
   aco::monotonic_buffer_resource memory;

   /* Info on how to spill preserved VGPRs. */
   aco::unordered_map<PhysReg, uint32_t> preserved_spill_offsets;
   aco::unordered_set<PhysReg> preserved_vgprs;
   aco::unordered_set<PhysReg> preserved_linear_vgprs;
   /* Info on how to spill preserved SGPRs. */
   aco::unordered_map<PhysReg, uint32_t> preserved_spill_lanes;
   aco::unordered_set<PhysReg> preserved_sgprs;

   aco::unordered_map<PhysReg, std::unordered_set<unsigned>> reg_block_uses;
   std::vector<postdom_info> dom_info;

   /* For each linear VGPR used for preserved SGPR spilling, the PhysReg it's assigned to. */
   std::vector<PhysReg> sgpr_spill_regs;

   /* Next scratch offset to spill VGPRs to. */
   unsigned next_preserved_offset;
   /* Next linear VGPR lane to spill SGPRs to. */
   unsigned next_preserved_lane;

   explicit spill_preserved_ctx(Program* program_)
       : program(program_), memory(), preserved_spill_offsets(memory), preserved_vgprs(memory),
         preserved_linear_vgprs(memory), preserved_spill_lanes(memory), preserved_sgprs(memory),
         reg_block_uses(memory), next_preserved_offset(DIV_ROUND_UP(
                                    program_->config->scratch_bytes_per_wave, program_->wave_size)),
         next_preserved_lane(program->first_abi_sgpr_spill_lane)
   {
      abi_preserved_range = program->callee_abi.preservedRegisters(program->max_reg_demand.sgpr,
                                                                   program->max_reg_demand.vgpr);
      dom_info.resize(program->blocks.size(), {-1u, -1u});
      sgpr_spill_regs.resize(program->abi_sgpr_spill_temps.size());
   }
};

void
add_instr(spill_preserved_ctx& ctx, unsigned block_index, bool seen_reload,
          const aco_ptr<Instruction>& instr, Instruction* startpgm)
{
   for (auto& def : instr->definitions) {
      assert(def.isFixed());
      /* Round down subdword registers to their base */
      PhysReg start_reg = PhysReg{def.physReg().reg()};
      for (auto reg : PhysRegInterval{start_reg, def.regClass().size()}) {
         if (!ctx.abi_preserved_range.contains(reg) && !def.regClass().is_linear_vgpr())
            continue;

         if (instr->opcode == aco_opcode::p_start_linear_vgpr) {
            auto abi_spill_it =
               std::find(ctx.program->abi_sgpr_spill_temps.begin(),
                         ctx.program->abi_sgpr_spill_temps.end(), instr->definitions[0].getTemp());
            if (abi_spill_it != ctx.program->abi_sgpr_spill_temps.end()) {
               unsigned vgpr_idx =
                  std::distance(ctx.program->abi_sgpr_spill_temps.begin(), abi_spill_it);
               ctx.sgpr_spill_regs[vgpr_idx] = instr->definitions[0].physReg();
            }
            /* Don't count start_linear_vgpr without a copy as a use since the value doesn't matter.
             * This allows us to move reloads a bit further up the CF.
             */
            if (instr->operands.empty())
               continue;
         }

         if (def.regClass().is_linear_vgpr())
            ctx.preserved_linear_vgprs.insert(reg);
         else if (def.regClass().type() == RegType::sgpr)
            ctx.preserved_sgprs.insert(reg);
         else
            ctx.preserved_vgprs.insert(reg);

         if (seen_reload) {
            if (def.regClass().is_linear())
               for (auto succ : ctx.program->blocks[block_index].linear_succs)
                  ctx.reg_block_uses[reg].emplace(succ);
            else
               for (auto succ : ctx.program->blocks[block_index].logical_succs)
                  ctx.reg_block_uses[reg].emplace(succ);
         } else {
            ctx.reg_block_uses[reg].emplace(block_index);
         }
      }
   }

   for (auto& op : instr->operands) {
      assert(op.isFixed());

      if (!op.isTemp())
         continue;
      /* Temporaries defined by startpgm are the preserved value - these uses don't need
       * any preservation.
       */
      if (std::any_of(startpgm->definitions.begin(), startpgm->definitions.end(),
                      [op](const auto& def)
                      { return def.isTemp() && def.tempId() == op.tempId(); }))
         continue;

      /* Round down subdword registers to their base */
      PhysReg start_reg = PhysReg{op.physReg().reg()};
      for (auto reg : PhysRegInterval{start_reg, op.regClass().size()}) {
         if (!ctx.abi_preserved_range.contains(reg) && !op.regClass().is_linear_vgpr())
            continue;

         /* Don't count end_linear_vgpr as a use since the value doesn't matter.
          * This allows us to move reloads a bit further up the CF.
          */
         if (instr->opcode == aco_opcode::p_end_linear_vgpr)
            continue;

         if (op.regClass().is_linear_vgpr())
            ctx.preserved_linear_vgprs.insert(reg);
         else if (op.regClass().type() == RegType::sgpr)
            ctx.preserved_sgprs.insert(reg);
         else
            ctx.preserved_vgprs.insert(reg);

         if (seen_reload) {
            if (op.regClass().is_linear())
               for (auto succ : ctx.program->blocks[block_index].linear_succs)
                  ctx.reg_block_uses[reg].emplace(succ);
            else
               for (auto succ : ctx.program->blocks[block_index].logical_succs)
                  ctx.reg_block_uses[reg].emplace(succ);
         } else {
            ctx.reg_block_uses[reg].emplace(block_index);
         }
      }
   }
}

void
add_preserved_vgpr_spill(spill_preserved_ctx& ctx, PhysReg reg,
                         std::vector<std::pair<PhysReg, unsigned>>& spills,
                         std::vector<std::pair<PhysReg, unsigned>>& lvgpr_spills)
{
   unsigned offset;

   auto offset_iter = ctx.preserved_spill_offsets.find(reg);
   if (offset_iter == ctx.preserved_spill_offsets.end()) {
      offset = ctx.next_preserved_offset;
      ctx.next_preserved_offset += 4;
      ctx.preserved_spill_offsets.emplace(reg, offset);
   } else {
      offset = offset_iter->second;
   }

   if (ctx.preserved_linear_vgprs.find(reg) != ctx.preserved_linear_vgprs.end())
      lvgpr_spills.emplace_back(reg, offset);
   else
      spills.emplace_back(reg, offset);
}

void
add_preserved_sgpr_spill(spill_preserved_ctx& ctx, PhysReg reg,
                         std::vector<std::pair<PhysReg, unsigned>>& spills)
{
   unsigned lane;

   auto lane_iter = ctx.preserved_spill_lanes.find(reg);
   if (lane_iter == ctx.preserved_spill_lanes.end()) {
      lane = ctx.next_preserved_lane++;
      ctx.preserved_spill_lanes.emplace(reg, lane);
   } else {
      lane = lane_iter->second;
   }

   spills.emplace_back(reg, lane);

   unsigned vgpr_idx = lane / ctx.program->wave_size;
   for (auto use : ctx.reg_block_uses[reg])
      ctx.reg_block_uses[ctx.sgpr_spill_regs[vgpr_idx]].insert(use);
}

void
emit_vgpr_spills_reloads(spill_preserved_ctx& ctx, Builder& bld,
                         std::vector<std::pair<PhysReg, unsigned>>& spills, PhysReg stack_reg,
                         PhysReg soffset, bool reload, bool linear, bool soffset_valid)
{
   if (spills.empty())
      return;

   int end_offset = (int)spills.back().second;
   int start_offset = (int)spills.front().second;
   if (ctx.program->gfx_level >= GFX9)
      assert(end_offset - start_offset < ctx.program->dev.scratch_global_offset_max);

   bool overflow =
      end_offset > ctx.program->dev.scratch_global_offset_max || ctx.program->gfx_level < GFX9;
   if (overflow) {
      if (ctx.program->gfx_level >= GFX9)
         bld.sop2(aco_opcode::s_add_u32, Definition(soffset, s1), Definition(scc, s1),
                  Operand(stack_reg, s1), Operand::c32(start_offset));
      else if (soffset_valid)
         bld.sop2(aco_opcode::s_add_u32, Definition(soffset, s1), Definition(scc, s1),
                  Operand(soffset, s1), Operand::c32(start_offset * ctx.program->wave_size));
      else
         bld.sop1(aco_opcode::s_mov_b32, Definition(soffset, s1),
                  Operand::c32(start_offset * ctx.program->wave_size));
   }

   Operand soffset_op;
   if (ctx.program->gfx_level >= GFX9)
      soffset_op = Operand(overflow ? soffset : stack_reg, s1);
   else
      soffset_op = soffset_valid || overflow ? Operand(soffset, s1) : Operand(sgpr_null, s1);

   for (const auto& spill : spills) {
      if (ctx.program->gfx_level >= GFX9) {
         if (reload)
            bld.scratch(aco_opcode::scratch_load_dword,
                        Definition(spill.first, linear ? v1.as_linear() : v1), Operand(v1),
                        soffset_op, overflow ? spill.second - start_offset : spill.second,
                        memory_sync_info(storage_vgpr_spill, semantic_private));
         else
            bld.scratch(aco_opcode::scratch_store_dword, Operand(v1), soffset_op,
                        Operand(spill.first, linear ? v1.as_linear() : v1),
                        overflow ? spill.second - start_offset : spill.second,
                        memory_sync_info(storage_vgpr_spill, semantic_private));
      } else {
         if (reload) {
            Instruction* instr = bld.mubuf(
               aco_opcode::buffer_load_dword, Definition(spill.first, linear ? v1.as_linear() : v1),
               Operand(stack_reg, s4), Operand(v1), soffset_op,
               overflow ? spill.second - start_offset : spill.second, false);
            instr->mubuf().sync = memory_sync_info(storage_vgpr_spill, semantic_private);
            instr->mubuf().cache.value = ac_swizzled;
         } else {
            Instruction* instr =
               bld.mubuf(aco_opcode::buffer_store_dword, Operand(stack_reg, s4), Operand(v1),
                         soffset_op, Operand(spill.first, linear ? v1.as_linear() : v1),
                         overflow ? spill.second - start_offset : spill.second, false);
            instr->mubuf().sync = memory_sync_info(storage_vgpr_spill, semantic_private);
            instr->mubuf().cache.value = ac_swizzled;
         }
      }
   }

   if (overflow && ctx.program->gfx_level < GFX9)
      bld.sop2(aco_opcode::s_sub_i32, Definition(soffset, s1), Definition(scc, s1),
               Operand(soffset, s1), Operand::c32(start_offset * ctx.program->wave_size));
}

void
emit_sgpr_spills_reloads(spill_preserved_ctx& ctx, std::vector<aco_ptr<Instruction>>& instructions,
                         std::vector<aco_ptr<Instruction>>::iterator& insert_point,
                         std::vector<std::pair<PhysReg, unsigned>>& spills, bool reload)
{
   std::vector<aco_ptr<Instruction>> spill_instructions;
   Builder bld(ctx.program, &spill_instructions);

   for (auto& spill : spills) {
      unsigned vgpr_idx = spill.second / ctx.program->wave_size;
      unsigned lane = spill.second % ctx.program->wave_size;
      Operand vgpr_op =
         Operand(ctx.program->abi_sgpr_spill_temps[vgpr_idx], ctx.sgpr_spill_regs[vgpr_idx]);
      if (reload)
         bld.pseudo(aco_opcode::p_reload, bld.def(s1, spill.first), vgpr_op, Operand::c32(lane));
      else
         bld.pseudo(aco_opcode::p_spill, vgpr_op, Operand::c32(lane), Operand(spill.first, s1));
   }

   insert_point = instructions.insert(insert_point, std::move_iterator(spill_instructions.begin()),
                                      std::move_iterator(spill_instructions.end()));
}

void
emit_spills_reloads(spill_preserved_ctx& ctx, std::vector<aco_ptr<Instruction>>& instructions,
                    std::vector<aco_ptr<Instruction>>::iterator& insert_point,
                    std::vector<std::pair<PhysReg, unsigned>>& spills,
                    std::vector<std::pair<PhysReg, unsigned>>& lvgpr_spills, bool reload)
{
   auto spill_reload_compare = [](const auto& first, const auto& second)
   { return first.second < second.second; };

   std::sort(spills.begin(), spills.end(), spill_reload_compare);
   std::sort(lvgpr_spills.begin(), lvgpr_spills.end(), spill_reload_compare);

   PhysReg stack_reg = (*insert_point)->operands[0].physReg();
   PhysReg soffset = (*insert_point)->definitions[0].physReg();
   PhysReg exec_backup = (*insert_point)->definitions[1].physReg();

   std::vector<aco_ptr<Instruction>> spill_instructions;
   Builder bld(ctx.program, &spill_instructions);

   emit_vgpr_spills_reloads(ctx, bld, spills, stack_reg, soffset, reload, false, false);
   if (!lvgpr_spills.empty()) {
      bld.sop1(Builder::s_or_saveexec, Definition(exec_backup, bld.lm), Definition(scc, s1),
               Definition(exec, bld.lm), Operand::c64(UINT64_MAX), Operand(exec, bld.lm));
      emit_vgpr_spills_reloads(ctx, bld, lvgpr_spills, stack_reg, soffset, reload, true, false);
      bld.sop1(Builder::WaveSpecificOpcode::s_mov, Definition(exec, bld.lm),
               Operand(exec_backup, bld.lm));
   }

   insert_point = instructions.erase(insert_point);

   instructions.insert(insert_point, std::move_iterator(spill_instructions.begin()),
                       std::move_iterator(spill_instructions.end()));
}

void
init_block_info(spill_preserved_ctx& ctx)
{
   Instruction* startpgm = ctx.program->blocks.front().instructions.front().get();

   unsigned cur_loop_header = -1u;
   for (unsigned index = ctx.program->blocks.size() - 1; index < ctx.program->blocks.size();) {
      const Block& block = ctx.program->blocks[index];

      if (block.linear_succs.empty()) {
         ctx.dom_info[index].logical_imm_postdom = block.index;
         ctx.dom_info[index].linear_imm_postdom = block.index;
      } else {
         int new_logical_postdom = -1;
         int new_linear_postdom = -1;
         for (unsigned succ_idx : block.logical_succs) {
            if ((int)ctx.dom_info[succ_idx].logical_imm_postdom == -1) {
               assert(cur_loop_header == -1u || succ_idx >= cur_loop_header);
               if (cur_loop_header == -1u)
                  cur_loop_header = succ_idx;
               continue;
            }

            if (new_logical_postdom == -1) {
               new_logical_postdom = (int)succ_idx;
               continue;
            }

            while ((int)succ_idx != new_logical_postdom) {
               if ((int)succ_idx < new_logical_postdom)
                  succ_idx = ctx.dom_info[succ_idx].logical_imm_postdom;
               if ((int)succ_idx > new_logical_postdom)
                  new_logical_postdom = (int)ctx.dom_info[new_logical_postdom].logical_imm_postdom;
            }
         }

         for (unsigned succ_idx : block.linear_succs) {
            if ((int)ctx.dom_info[succ_idx].linear_imm_postdom == -1) {
               assert(cur_loop_header == -1u || succ_idx >= cur_loop_header);
               if (cur_loop_header == -1u)
                  cur_loop_header = succ_idx;
               continue;
            }

            if (new_linear_postdom == -1) {
               new_linear_postdom = (int)succ_idx;
               continue;
            }

            while ((int)succ_idx != new_linear_postdom) {
               if ((int)succ_idx < new_linear_postdom)
                  succ_idx = ctx.dom_info[succ_idx].linear_imm_postdom;
               if ((int)succ_idx > new_linear_postdom)
                  new_linear_postdom = (int)ctx.dom_info[new_linear_postdom].linear_imm_postdom;
            }
         }

         ctx.dom_info[index].logical_imm_postdom = new_logical_postdom;
         ctx.dom_info[index].linear_imm_postdom = new_linear_postdom;
      }

      bool seen_reload_vgpr = false;
      for (auto& instr : block.instructions) {
         if (instr->opcode == aco_opcode::p_reload_preserved) {
            seen_reload_vgpr = true;
            continue;
         } else if (instr->opcode == aco_opcode::p_startpgm) {
            continue;
         }

         add_instr(ctx, index, seen_reload_vgpr, instr, startpgm);
      }

      /* Process predecessors of loop headers again, since post-dominance information of the header
       * was not available the first time
       */
      unsigned next_idx = index - 1;
      if (index == cur_loop_header) {
         assert(block.kind & block_kind_loop_header);
         for (auto pred : block.logical_preds)
            if (ctx.dom_info[pred].logical_imm_postdom == -1u)
               next_idx = std::max(next_idx, pred);
         for (auto pred : block.linear_preds)
            if (ctx.dom_info[pred].linear_imm_postdom == -1u)
               next_idx = std::max(next_idx, pred);
         cur_loop_header = -1u;
      }
      index = next_idx;
   }
}

struct call_spill {
   unsigned instr_idx;
   std::vector<std::pair<PhysReg, unsigned>> spills;
};

void
emit_call_spills(spill_preserved_ctx& ctx)
{
   std::set<PhysReg> linear_vgprs;
   std::unordered_map<unsigned, std::vector<call_spill>> block_call_spills;

   unsigned max_scratch_offset = ctx.next_preserved_offset;

   for (auto& block : ctx.program->blocks) {
      for (auto it = block.instructions.begin(); it != block.instructions.end(); ++it) {
         auto& instr = *it;

         if (instr->opcode == aco_opcode::p_call) {
            unsigned scratch_offset = ctx.next_preserved_offset;
            struct call_spill spill = {
               .instr_idx = (unsigned)(it - block.instructions.begin()),
            };
            SparseRegisterSet preserved_regs = instr->call().abi.preservedRegisters(
               ctx.program->max_reg_demand.sgpr, ctx.program->max_reg_demand.vgpr);
            for (auto& reg : linear_vgprs) {
               if (preserved_regs.contains(reg))
                  continue;
               spill.spills.emplace_back(reg, scratch_offset);
               scratch_offset += 4;
            }
            max_scratch_offset = std::max(max_scratch_offset, scratch_offset);

            block_call_spills[block.index].emplace_back(std::move(spill));
         } else if (instr->opcode == aco_opcode::p_start_linear_vgpr) {
            linear_vgprs.insert(instr->definitions[0].physReg());
         } else if (instr->opcode == aco_opcode::p_end_linear_vgpr) {
            for (auto& op : instr->operands)
               linear_vgprs.erase(op.physReg());
         }
      }
   }

   /* XXX: This should also be possible on GFX9, although small negative scratch offsets
    * seem to hang the GPU, so disable it there now.
    */
   if (ctx.program->gfx_level >= GFX10)
      for (auto& block_calls : block_call_spills)
         for (auto& call_spills : block_calls.second)
            for (auto& spill : call_spills.spills)
               spill.second -= max_scratch_offset;

   for (auto& block_calls : block_call_spills) {
      for (unsigned i = 0; i < block_calls.second.size(); ++i) {
         auto& block = ctx.program->blocks[block_calls.first];
         auto& call = block_calls.second[i];
         auto& instr = block.instructions[call.instr_idx];
         auto it = block.instructions.begin() + call.instr_idx;
         unsigned num_inserted_instrs = 0;

         std::vector<aco_ptr<Instruction>> spill_instructions;
         Builder bld(ctx.program, &spill_instructions);

         PhysReg stack_reg = instr->operands[0].physReg();
         PhysReg soffset = PhysReg{UINT32_MAX};
         PhysReg scratch_rsrc = PhysReg{UINT32_MAX};
         if (ctx.program->gfx_level < GFX9)
            scratch_rsrc = instr->operands.back().physReg();

         if (ctx.program->gfx_level >= GFX10) {
            bld.sop2(aco_opcode::s_add_u32, Definition(stack_reg, s1), Definition(scc, s1),
                     Operand(stack_reg, s1), Operand::c32(max_scratch_offset));
            emit_vgpr_spills_reloads(ctx, bld, call.spills, stack_reg, soffset, false, true, false);
         } else if (ctx.program->gfx_level == GFX9) {
            emit_vgpr_spills_reloads(ctx, bld, call.spills, stack_reg, soffset, false, true, false);
            bld.sop2(aco_opcode::s_add_u32, Definition(stack_reg, s1), Definition(scc, s1),
                     Operand(stack_reg, s1), Operand::c32(max_scratch_offset));
         } else {
            emit_vgpr_spills_reloads(ctx, bld, call.spills, scratch_rsrc, stack_reg, false, true,
                                     true);
            bld.sop2(aco_opcode::s_add_u32, Definition(stack_reg, s1), Definition(scc, s1),
                     Operand(stack_reg, s1),
                     Operand::c32(max_scratch_offset * ctx.program->wave_size));
         }

         it = block.instructions.insert(it, std::move_iterator(spill_instructions.begin()),
                                        std::move_iterator(spill_instructions.end()));
         it += spill_instructions.size() + 1;
         num_inserted_instrs += spill_instructions.size();

         spill_instructions.clear();

         if (ctx.program->gfx_level >= GFX10) {
            emit_vgpr_spills_reloads(ctx, bld, call.spills, stack_reg, soffset, true, true, false);
            bld.sop2(aco_opcode::s_sub_u32, Definition(stack_reg, s1), Definition(scc, s1),
                     Operand(stack_reg, s1), Operand::c32(max_scratch_offset));
         } else if (ctx.program->gfx_level == GFX9) {
            bld.sop2(aco_opcode::s_sub_u32, Definition(stack_reg, s1), Definition(scc, s1),
                     Operand(stack_reg, s1), Operand::c32(max_scratch_offset));
            emit_vgpr_spills_reloads(ctx, bld, call.spills, stack_reg, soffset, true, true, false);
         } else {
            bld.sop2(aco_opcode::s_sub_u32, Definition(stack_reg, s1), Definition(scc, s1),
                     Operand(stack_reg, s1),
                     Operand::c32(max_scratch_offset * ctx.program->wave_size));
            emit_vgpr_spills_reloads(ctx, bld, call.spills, scratch_rsrc, stack_reg, true, true,
                                     true);
         }

         block.instructions.insert(it, std::move_iterator(spill_instructions.begin()),
                                   std::move_iterator(spill_instructions.end()));
         num_inserted_instrs += spill_instructions.size();

         for (unsigned j = i + 1; j < block_calls.second.size(); ++j)
            block_calls.second[j].instr_idx += num_inserted_instrs;
      }
   }

   ctx.next_preserved_offset = max_scratch_offset;
}

void
emit_preserved_spills(spill_preserved_ctx& ctx)
{
   std::vector<std::pair<PhysReg, unsigned>> spills;
   std::vector<std::pair<PhysReg, unsigned>> lvgpr_spills;
   std::vector<std::pair<PhysReg, unsigned>> sgpr_spills;

   for (auto reg : ctx.preserved_vgprs)
      add_preserved_vgpr_spill(ctx, reg, spills, lvgpr_spills);
   for (auto reg : ctx.preserved_linear_vgprs)
      add_preserved_vgpr_spill(ctx, reg, spills, lvgpr_spills);
   for (auto reg : ctx.preserved_sgprs)
      add_preserved_sgpr_spill(ctx, reg, sgpr_spills);

   auto start_instr = std::find_if(ctx.program->blocks.front().instructions.begin(),
                                   ctx.program->blocks.front().instructions.end(),
                                   [](const auto& instr)
                                   { return instr->opcode == aco_opcode::p_spill_preserved; });
   /* The spiller inserts linear VGPRs for SGPR spilling after p_spill_preserved. Move past
    * them and start spilling preserved SGPRs there.
    */
   for (start_instr = std::next(start_instr);
        start_instr != ctx.program->blocks.front().instructions.end() &&
        (*start_instr)->opcode == aco_opcode::p_start_linear_vgpr;
        ++start_instr) {}
   emit_sgpr_spills_reloads(ctx, ctx.program->blocks.front().instructions, start_instr, sgpr_spills,
                            false);
   /* Move the iterator back to p_spill_preserved where we started from. */
   for (start_instr = std::prev(start_instr);
        start_instr != ctx.program->blocks.front().instructions.begin() &&
        (*start_instr)->opcode == aco_opcode::p_start_linear_vgpr;
        --start_instr) {}

   emit_spills_reloads(ctx, ctx.program->blocks.front().instructions, start_instr, spills,
                       lvgpr_spills, false);

   auto block_reloads =
      std::vector<std::vector<std::pair<PhysReg, unsigned>>>(ctx.program->blocks.size());
   auto lvgpr_block_reloads =
      std::vector<std::vector<std::pair<PhysReg, unsigned>>>(ctx.program->blocks.size());
   auto sgpr_block_reloads =
      std::vector<std::vector<std::pair<PhysReg, unsigned>>>(ctx.program->blocks.size());

   for (auto it = ctx.reg_block_uses.begin(); it != ctx.reg_block_uses.end();) {
      bool is_linear_vgpr =
         ctx.preserved_linear_vgprs.find(it->first) != ctx.preserved_linear_vgprs.end();
      bool is_sgpr = ctx.preserved_sgprs.find(it->first) != ctx.preserved_sgprs.end();
      bool is_linear = is_linear_vgpr || is_sgpr;

      if (!is_linear && ctx.preserved_vgprs.find(it->first) == ctx.preserved_vgprs.end()) {
         it = ctx.reg_block_uses.erase(it);
         continue;
      }

      unsigned min_common_postdom = 0;

      for (auto succ_idx : it->second) {
         while (succ_idx != min_common_postdom) {
            if (min_common_postdom < succ_idx) {
               min_common_postdom = is_linear
                                       ? ctx.dom_info[min_common_postdom].linear_imm_postdom
                                       : ctx.dom_info[min_common_postdom].logical_imm_postdom;
            } else {
               succ_idx = is_linear ? ctx.dom_info[succ_idx].linear_imm_postdom
                                    : ctx.dom_info[succ_idx].logical_imm_postdom;
            }
         }
      }

      while (std::find_if(ctx.program->blocks[min_common_postdom].instructions.rbegin(),
                          ctx.program->blocks[min_common_postdom].instructions.rend(),
                          [](const auto& instr) {
                             return instr->opcode == aco_opcode::p_reload_preserved;
                          }) == ctx.program->blocks[min_common_postdom].instructions.rend())
         min_common_postdom = is_linear ? ctx.dom_info[min_common_postdom].linear_imm_postdom
                                        : ctx.dom_info[min_common_postdom].logical_imm_postdom;

      if (is_linear_vgpr) {
         lvgpr_block_reloads[min_common_postdom].emplace_back(
            it->first, ctx.preserved_spill_offsets[it->first]);
         ctx.preserved_linear_vgprs.erase(it->first);
      } else if (is_sgpr) {
         sgpr_block_reloads[min_common_postdom].emplace_back(it->first,
                                                             ctx.preserved_spill_lanes[it->first]);
         ctx.preserved_sgprs.erase(it->first);
      } else {
         block_reloads[min_common_postdom].emplace_back(it->first,
                                                        ctx.preserved_spill_offsets[it->first]);
         ctx.preserved_vgprs.erase(it->first);
      }

      it = ctx.reg_block_uses.erase(it);
   }

   for (unsigned i = 0; i < ctx.program->blocks.size(); ++i) {
      auto instr_it = std::find_if(
         ctx.program->blocks[i].instructions.rbegin(), ctx.program->blocks[i].instructions.rend(),
         [](const auto& instr) { return instr->opcode == aco_opcode::p_reload_preserved; });
      if (instr_it == ctx.program->blocks[i].instructions.rend()) {
         assert(block_reloads[i].empty() && lvgpr_block_reloads[i].empty());
         continue;
      }
      /* Insert SGPR spills after reload_preserved_vgpr, then move the iterator back to the
       * reload_preserved_vgpr instruction.
       */
      auto end_instr = instr_it.base();
      emit_sgpr_spills_reloads(ctx, ctx.program->blocks[i].instructions, end_instr,
                               sgpr_block_reloads[i], true);
      end_instr = std::prev(end_instr);

      emit_spills_reloads(ctx, ctx.program->blocks[i].instructions, end_instr, block_reloads[i],
                          lvgpr_block_reloads[i], true);
   }
}

void
spill_preserved(Program* program)
{
   if (!program->is_callee)
      return;

   spill_preserved_ctx ctx(program);

   init_block_info(ctx);

   if (!program->bypass_reg_preservation)
      emit_preserved_spills(ctx);

   emit_call_spills(ctx);

   program->config->scratch_bytes_per_wave = ctx.next_preserved_offset * program->wave_size;
}
} // namespace aco
