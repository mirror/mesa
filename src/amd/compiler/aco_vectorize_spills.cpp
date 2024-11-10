/*
 * Copyright © 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_builder.h"
#include "aco_ir.h"

#include <unordered_set>

namespace aco {

struct vectorize_ctx {
   std::vector<aco_ptr<Instruction>> instrs_to_vectorize;

   std::vector<aco_ptr<Instruction>> vectors;
   std::vector<aco_ptr<Instruction>> vectorized_instrs;

   std::vector<unsigned> component_idxs;

   std::unordered_set<unsigned> killed_soffset_ids;
   std::unordered_set<unsigned> seen_soffset_ids;

   std::vector<aco_ptr<Instruction>>::iterator insert_point;
   Block* block;
   Program* program;
};

void
vectorize_and_insert(vectorize_ctx& ctx, bool store)
{
   std::sort(ctx.instrs_to_vectorize.begin(), ctx.instrs_to_vectorize.end(),
             [](const auto& one, const auto& other)
             { return one->scratch().offset < other->scratch().offset; });

   Builder instr_bld(ctx.program, &ctx.vectorized_instrs);

   for (unsigned i = 0; i < ctx.instrs_to_vectorize.size(); ++i) {
      ctx.component_idxs.push_back(i);
      for (auto j = i + 1; j < ctx.instrs_to_vectorize.size(); ++j) {
         const auto& component = ctx.instrs_to_vectorize[ctx.component_idxs.back()];
         const auto& instr = ctx.instrs_to_vectorize[j];
         /* skip stores with unrelated soffset */
         if (instr->operands[1].tempId() != component->operands[1].tempId())
            continue;
         int16_t next_offset;
         if (store)
            next_offset = component->scratch().offset + (int16_t)component->operands[2].bytes();
         else
            next_offset = component->scratch().offset + (int16_t)component->definitions[0].bytes();

         /* there's a gap, can't vectorize across it */
         if (instr->scratch().offset > next_offset)
            break;
         /* XXX: Hitting this means there are intersecting stores. This shouldn't happen! */
         if (instr->scratch().offset != next_offset)
            break;

         if (instr->operands[1].isKill())
            ctx.killed_soffset_ids.insert(instr->operands[1].tempId());

         ctx.component_idxs.push_back(j);
      }

      if (ctx.component_idxs.empty())
         continue;

      size_t comp_idx = 0;
      while (comp_idx < ctx.component_idxs.size()) {
         size_t vector_size = 4;
         while (vector_size > ctx.component_idxs.size() - comp_idx)
            vector_size >>= 1;

         auto& first_component = ctx.instrs_to_vectorize[ctx.component_idxs[comp_idx]];

         if (vector_size == 1) {
            ctx.vectorized_instrs.emplace_back(std::move(first_component));
            ++comp_idx;
            continue;
         }

         if (store) {
            Temp vec_tmp = ctx.program->allocateTmp(RegClass(RegType::vgpr, vector_size));
            Instruction* vec =
               create_instruction(aco_opcode::p_create_vector, Format::PSEUDO, vector_size, 1);
            for (unsigned c = 0; c < vector_size; ++c) {
               auto& component = ctx.instrs_to_vectorize[ctx.component_idxs[comp_idx + c]];
               vec->operands[c] = component->operands[2];
            }
            vec->definitions[0] = Definition(vec_tmp);
            ctx.vectors.emplace_back(vec);

            aco_opcode opcode;
            switch (vector_size) {
            case 4: opcode = aco_opcode::scratch_store_dwordx4; break;
            case 2: opcode = aco_opcode::scratch_store_dwordx2; break;
            default: unreachable("invalid vector size");
            }

            Operand vec_op = Operand(vec_tmp);
            vec_op.setFirstKill(true);
            instr_bld.scratch(opcode, Operand(v1), first_component->operands[1], vec_op,
                              first_component->scratch().offset, first_component->scratch().sync);
         } else {
            Temp vec_tmp = ctx.program->allocateTmp(RegClass(RegType::vgpr, vector_size));

            aco_opcode opcode;
            switch (vector_size) {
            case 4: opcode = aco_opcode::scratch_load_dwordx4; break;
            case 2: opcode = aco_opcode::scratch_load_dwordx2; break;
            default: unreachable("invalid vector size");
            }

            instr_bld.scratch(opcode, Definition(vec_tmp), Operand(v1),
                              first_component->operands[1], first_component->scratch().offset,
                              first_component->scratch().sync);

            Instruction* vec =
               create_instruction(aco_opcode::p_split_vector, Format::PSEUDO, 1, vector_size);
            for (unsigned c = 0; c < vector_size; ++c) {
               auto& component = ctx.instrs_to_vectorize[ctx.component_idxs[comp_idx + c]];
               vec->definitions[c] = component->definitions[0];
            }
            vec->operands[0] = Operand(vec_tmp);
            vec->operands[0].setFirstKill(true);
            ctx.vectors.emplace_back(vec);
         }
         comp_idx += vector_size;
      }

      for (unsigned j = 0; j < ctx.component_idxs.size(); ++j) {
         auto idx = ctx.component_idxs[j];
         ctx.instrs_to_vectorize.erase(ctx.instrs_to_vectorize.begin() + (idx - j));
      }
      /* Adjust for deleted instruction */
      --i;

      ctx.component_idxs.clear();
   }

   for (auto it = ctx.vectorized_instrs.rbegin(); it != ctx.vectorized_instrs.rend(); ++it) {
      auto soffset_id = (*it)->operands[1].tempId();
      if (ctx.seen_soffset_ids.find(soffset_id) == ctx.seen_soffset_ids.end()) {
         if (ctx.killed_soffset_ids.find(soffset_id) != ctx.killed_soffset_ids.end())
            (*it)->operands[1].setFirstKill(true);
         ctx.seen_soffset_ids.insert(soffset_id);
      }
   }

   if (store) {
      ctx.insert_point =
         ctx.block->instructions.insert(ctx.insert_point, std::move_iterator(ctx.vectors.begin()),
                                        std::move_iterator(ctx.vectors.end()));
      ctx.insert_point += ctx.vectors.size();
      ctx.insert_point = ctx.block->instructions.insert(
         ctx.insert_point, std::move_iterator(ctx.vectorized_instrs.rbegin()),
         std::move_iterator(ctx.vectorized_instrs.rend()));
      ctx.insert_point += ctx.vectorized_instrs.size();
   } else {
      ctx.insert_point = ctx.block->instructions.insert(
         ctx.insert_point, std::move_iterator(ctx.vectorized_instrs.rbegin()),
         std::move_iterator(ctx.vectorized_instrs.rend()));
      ctx.insert_point += ctx.vectorized_instrs.size();
      ctx.insert_point =
         ctx.block->instructions.insert(ctx.insert_point, std::move_iterator(ctx.vectors.begin()),
                                        std::move_iterator(ctx.vectors.end()));
      ctx.insert_point += ctx.vectors.size();
   }

   ctx.vectors.clear();
   ctx.vectorized_instrs.clear();
   ctx.instrs_to_vectorize.clear();
   ctx.seen_soffset_ids.clear();
   ctx.killed_soffset_ids.clear();
}

void
vectorize_spills(Program* program)
{
   vectorize_ctx ctx;
   ctx.program = program;
   aco::monotonic_buffer_resource memory;

   for (auto& block : program->blocks) {
      ctx.block = &block;
      IDSet conflicting_temps(memory);

      /* Try vectorizing stores */
      for (auto it = block.instructions.begin(); it != block.instructions.end();) {
         bool vectorize_now = !(*it)->isVMEM() && it != block.instructions.begin();

         /* Only look for stores that kill their operand. We can move/combine these with other
          * instructions without affecting register demand.
          */
         if ((*it)->opcode == aco_opcode::scratch_store_dword && (*it)->operands[2].isKill() &&
             !(*it)->operands[2].regClass().is_subdword()) {
            if (conflicting_temps.count((*it)->operands[2].tempId())) {
               vectorize_now = true;
               --it;
            } else {
               bool first = ctx.instrs_to_vectorize.empty();
               ctx.instrs_to_vectorize.emplace_back(std::move(*it));
               it = block.instructions.erase(it);
               if (first)
                  ctx.insert_point = it;
               continue;
            }
         }

         if (vectorize_now) {
            auto clause_size = it - ctx.insert_point;
            vectorize_and_insert(ctx, true);
            it = ctx.insert_point + clause_size;
            conflicting_temps = IDSet(memory);
         } else {
            for (auto& def : (*it)->definitions)
               if (def.isTemp())
                  conflicting_temps.insert(def.tempId());
         }
         ++it;
      }
      /* Try vectorizing loads */
      for (auto it = block.instructions.begin(); it != block.instructions.end();) {
         bool vectorize_now = !(*it)->isVMEM() && it != block.instructions.begin();
         for (auto& op : (*it)->operands) {
            if (op.isTemp() && conflicting_temps.count(op.tempId())) {
               vectorize_now = true;
               --it;
            }
         }

         /* Loads that kill their definition are dead and shouldn't appear with spilling */
         if (!vectorize_now && (*it)->opcode == aco_opcode::scratch_load_dword &&
             !(*it)->definitions[0].isKill() && !(*it)->definitions[0].regClass().is_subdword()) {
            ctx.instrs_to_vectorize.emplace_back(std::move(*it));
            conflicting_temps.insert((*it)->definitions[0].tempId());
            it = block.instructions.erase(it);
            continue;
         }

         if (vectorize_now) {
            ctx.insert_point = it;
            vectorize_and_insert(ctx, false);
            it = ctx.insert_point;
            conflicting_temps = IDSet(memory);
         }
         ++it;
      }
   }
}

} // namespace aco
