/*
 * Copyright © 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_builder.h"
#include "aco_ir.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace aco {

namespace {

struct var_state;

struct split_vector_state;

struct var_state_reference {
   explicit var_state_reference(bool is_vector_component_, unsigned remap_id_)
       : is_vector_component(is_vector_component_), id(remap_id_)
   {}

   bool operator==(const var_state_reference& other) const {
      return is_vector_component == other.is_vector_component && id == other.id;
   }

   bool is_vector_component;
   unsigned id;
};

struct phi_info {
   RegClass rc;
   std::vector<Temp> split_defs;
   std::vector<std::variant<var_state_reference, Operand>> operands;
};

std::vector<IDSet>* get_dom_frontier(split_vector_state* state, RegType type);
void add_header_phi(split_vector_state* state, unsigned block, phi_info&& info);
void insert_instruction(split_vector_state* state, unsigned block, aco_ptr<Instruction>&& instr);
void insert_instruction(split_vector_state* state, unsigned block,
                        const std::function<bool(const aco_ptr<Instruction>&)>& insert_pred,
                        aco_ptr<Instruction>&& instr);
Operand get_phi_operand(split_vector_state& state, unsigned block, unsigned pred, RegType def_type,
                const var_state_reference& ref);

struct block_def {

   block_def(Operand value_) : value(value_) {}

   block_def(Operand value_, bool used_) : value(value_), used(used_) {}

   block_def() {}

   std::optional<Operand> value;
   Instruction* phi = nullptr;
   bool used = true;
};

struct var_state {
   var_state_reference ref;
   split_vector_state* state;
   Program* program;
   RegClass rc;
   std::unordered_map<unsigned, block_def> block_defs;

   var_state() : ref(false, 0) {}

   var_state(split_vector_state* state_, Program* program_, var_state_reference ref_, RegClass rc_,
             unsigned block, Operand def, bool initially_used)
       : ref(ref_), state(state_), program(program_), rc(rc_)
   {
      assert(rc.size() == 1);
      block_defs.emplace(block, block_def(def, initially_used));
   }

   bool output_is_used(unsigned block, unsigned id)
   {
      auto it = block_defs.find(block);

      assert(it != block_defs.end() && it->second.value);

      /* If the temp ID of the block output is different, then this id has been used in a subsequent
       * create_vector, and the block output is the result of the last split_vector.
       */
      if (!it->second.value->isTemp() || it->second.value->tempId() != id)
         return true;
      return it->second.used;
   }

   unsigned get_output_block(unsigned block) const
   {
      for (int dom = (int)block; dom != -1; dom = rc.is_linear()
                                                     ? program->blocks[dom].linear_idom
                                                     : program->blocks[dom].logical_idom) {
         auto it = block_defs.find(dom);
         if (it == block_defs.end()) {
            if (dom == 0)
               break;
            continue;
         }

         return dom;
      }
      return -1u;
   }

   Operand& get_output(unsigned block)
   {
      for (int dom = (int)block; dom != -1; dom = rc.is_linear()
                                                     ? program->blocks[dom].linear_idom
                                                     : program->blocks[dom].logical_idom) {
         auto* preds = rc.is_linear() ? &program->blocks[dom].linear_preds
                                      : &program->blocks[dom].logical_preds;

         auto it = block_defs.find(dom);

         if ((program->blocks[dom].kind & block_kind_loop_header) &&
             (it == block_defs.end() || !it->second.value)) {
            bool needs_header_phi = it != block_defs.end() || block < (*preds).back();
            for (unsigned loop_block = dom; !needs_header_phi && loop_block < (*preds).back();
                 ++loop_block) {
               if (block_defs.find(loop_block) != block_defs.end())
                  needs_header_phi = true;
            }

            if (needs_header_phi) {
               auto new_tmp = program->allocateTmp(rc);
               it = add_output(dom, Operand(new_tmp));

               add_header_phi(
                  state, dom,
                  phi_info{
                     rc,
                     {new_tmp},
                     std::vector<std::variant<var_state_reference, Operand>>(preds->size(), ref),
                  });
            }
         }

         if (it == block_defs.end()) {
            if (dom == 0)
               break;
            continue;
         }

         if (it->second.value) {
            it->second.used = true;
            return *it->second.value;
         }

         Temp tmp = program->allocateTmp(rc);

         std::vector<Temp> pred_temps(preds->size());

         auto phi =
            create_instruction(rc.is_linear() ? aco_opcode::p_linear_phi : aco_opcode::p_phi,
                               Format::PSEUDO, preds->size(), 1);
         for (unsigned i = 0; i < preds->size(); ++i) {
            auto pred = (*preds)[i];
            phi->operands[i] = get_phi_operand(*state, dom, pred, rc.type(), ref);
         }
         phi->definitions[0] = Definition(tmp);

         block_defs[dom].phi = phi;

         insert_instruction(state, dom, aco_ptr<Instruction>(phi));

         return *add_output(dom, Operand(tmp))->second.value;
      }
      fprintf(stderr, "Value is undefined in block %u!\n", block);
      unreachable("Value is undefined in block!");
   }

   typename std::unordered_map<unsigned, block_def>::iterator add_output(unsigned block,
                                                                         block_def&& def)
   {
      auto it = block_defs.find(block);
      if (it != block_defs.end()) {
         for (auto it2 = block_defs.begin(); it2 != block_defs.end(); ++it2) {
            if (!it->second.value || !it->second.value->isTemp())
               break;
            if (it == it2)
               continue;
            if (!it2->second.phi)
               continue;
            assert(it2->second.phi->definitions[0].size() == 1);
            for (auto& op : it2->second.phi->operands) {
               if (op.isTemp() && op.tempId() == it->second.value->tempId() && def.value)
                  op = *def.value;
            }
         }
         block_defs.erase(it);
      }
      auto ret = block_defs.emplace(block, std::forward<block_def>(def));

      auto* dom_frontier = get_dom_frontier(state, rc.type());
      for (auto phi_block : (*dom_frontier)[block]) {
         if (block_defs.find(phi_block) == block_defs.end())
            add_output(phi_block, block_def());
      }

      return ret.first;
   }
};

struct split_vector_state {
   explicit split_vector_state(Program* program_)
       : program(program_), temps_to_split(memory), temps_to_preserve(memory)
   {
      linear_dom_frontiers.resize(program->blocks.size(), IDSet(memory));
      logical_dom_frontiers.resize(program->blocks.size(), IDSet(memory));
      for (auto& block : program->blocks) {
         if (block.linear_preds.size() > 1) {
            for (unsigned pred : block.linear_preds) {
               for (int it = (int)pred; it != block.linear_idom;
                    it = program->blocks[it].linear_idom) {
                  if (it == (int)block.index)
                     break;
                  linear_dom_frontiers[it].insert(block.index);
               }
            }
         }
         if (block.logical_preds.size() > 1) {
            for (unsigned pred : block.logical_preds) {
               for (int it = (int)pred; it != block.logical_idom;
                    it = program->blocks[it].logical_idom) {
                  if (it == (int)block.index)
                     break;
                  logical_dom_frontiers[it].insert(block.index);
               }
            }
         }
      }
   }

   void remap_temp(unsigned id, unsigned block, Operand new_def)
   {
      auto it = scalar_states.find(id);
      if (it == scalar_states.end()) {
         std::unordered_map<unsigned, block_def> block_defs;
         block_defs.emplace(block, new_def);
         scalar_states.emplace(id, var_state(this, program, var_state_reference(false, id),
                                             program->temp_rc[id], block, new_def, true));
         return;
      }

      it->second.add_output(block, new_def);
   }

   void split_vector(unsigned id, unsigned block, const std::vector<Operand>& ops)
   {
      auto it = vector_splits.find(id);
      if (it == vector_splits.end()) {
         std::vector<var_state_reference> states;

         RegClass elem_rc = RegClass(program->temp_rc[id].type(), 1);
         if (program->temp_rc[id].is_linear_vgpr())
            elem_rc = elem_rc.as_linear();
         for (const auto& op : ops) {
            if (op.isTemp()) {
               auto temp_it = scalar_states.find(op.tempId());
               if (temp_it != scalar_states.end()) {
                  states.emplace_back(false, temp_it->first);
                  continue;
               }
            }
            var_state_reference ref = var_state_reference(true, vector_states.size());
            vector_states.emplace_back(this, program, ref, elem_rc, block, op, false);
            states.push_back(ref);
         }
         it = vector_splits.emplace(id, std::move(states)).first;
      }

      for (unsigned i = 0; i < it->second.size(); ++i)
         resolve_var_state_ref(it->second[i]).add_output(block, block_def(ops[i], false));
   }

   var_state& resolve_var_state_ref(const var_state_reference& ref)
   {
      return ref.is_vector_component ? vector_states[ref.id] : scalar_states[ref.id];
   }

   aco::monotonic_buffer_resource memory;
   Program* program;
   unsigned loop_header_worklist = -1u;
   std::pair<unsigned, std::vector<aco_ptr<Instruction>>*> working_block;
   std::unordered_map<unsigned, var_state> scalar_states;
   std::vector<var_state> vector_states;
   std::unordered_map<unsigned, std::vector<phi_info>> header_phis;
   std::unordered_map<unsigned, std::vector<var_state_reference>> vector_splits;
   std::vector<IDSet> logical_dom_frontiers;
   std::vector<IDSet> linear_dom_frontiers;
   IDSet temps_to_split;
   IDSet temps_to_preserve;
};

void
add_header_phi(split_vector_state* state, unsigned block, phi_info&& info)
{
   state->header_phis[block].emplace_back(std::forward<phi_info>(info));
   state->loop_header_worklist = std::min(state->loop_header_worklist, block);
}

std::vector<IDSet>*
get_dom_frontier(split_vector_state* state, RegType type)
{
   return type == RegType::sgpr ? &state->linear_dom_frontiers : &state->logical_dom_frontiers;
}

void
insert_instruction(split_vector_state* state, unsigned block, aco_ptr<Instruction>&& instr)
{
   if (state->working_block.first == block)
      state->working_block.second->insert(state->working_block.second->begin(),
                                          std::forward<aco_ptr<Instruction>>(instr));
   else
      state->program->blocks[block].instructions.insert(
         state->program->blocks[block].instructions.begin(),
         std::forward<aco_ptr<Instruction>>(instr));
}

void
insert_instruction(split_vector_state* state, unsigned block,
                   const std::function<bool(const aco_ptr<Instruction>&)>& insert_pred,
                   aco_ptr<Instruction>&& instr)
{
   if (state->working_block.first == block)
      state->working_block.second->insert(
         std::find_if(state->working_block.second->begin(), state->working_block.second->end(),
                      insert_pred),
         std::forward<aco_ptr<Instruction>>(instr));
   else
      state->program->blocks[block].instructions.insert(
         std::find_if(state->program->blocks[block].instructions.begin(),
                      state->program->blocks[block].instructions.end(), insert_pred),
         std::forward<aco_ptr<Instruction>>(instr));
}

void update_operand(split_vector_state& state, unsigned block, Operand& op, Builder& bld) {
   if (!op.isTemp())
      return;

   auto remap = state.scalar_states.find(op.tempId());
   Operand new_op = op;
   if (remap != state.scalar_states.end())
      new_op = remap->second.get_output(block);

   Temp tmp;
   if (!new_op.isTemp())
      tmp = bld.copy(bld.def(op.regClass()), new_op);
   else
      tmp = new_op.getTemp();

   if (tmp.regClass() != op.regClass()) {
      if (op.regClass().type() == RegType::sgpr)
         tmp = bld.as_uniform(tmp);
      else
         tmp = bld.copy(bld.def(op.regClass()), tmp);
   }
   op.setTemp(tmp);
}

Operand
get_phi_operand(split_vector_state& state, unsigned block, unsigned pred, RegType def_type,
                const var_state_reference& ref)
{
   auto& var = state.resolve_var_state_ref(ref);
   Operand op = var.get_output(pred);
   if (!op.isTemp())
      return op;

   unsigned move_block = var.get_output_block(pred);
   if (move_block == block)
      move_block = pred;

   std::vector<aco_ptr<Instruction>> moves;
   Builder move_bld(state.program, &moves);

   if (def_type != op.regClass().type() && def_type == RegType::sgpr)
      op = Operand(move_bld.as_uniform(op.getTemp()));

   for (auto& move_instr : moves) {
      insert_instruction(
         &state, move_block, [](const auto& instr)
         { return instr->opcode == aco_opcode::p_logical_end || instr->isBranch(); },
         std::move(move_instr));
   }
   return op;
}

void
lower_phi(split_vector_state& state, unsigned block, const phi_info& phi,
          std::vector<aco_ptr<Instruction>>& instructions)
{
   RegClass rc = phi.rc;
   auto* preds = rc.is_linear() ? &state.program->blocks[block].linear_preds
                                : &state.program->blocks[block].logical_preds;
   std::vector<std::vector<Operand>> pred_operands(preds->size());
   for (unsigned i = 0; i < preds->size(); ++i) {
      auto pred = (*preds)[i];

      if (std::holds_alternative<var_state_reference>(phi.operands[i])) {
         assert(rc.size() == 1);
         Operand op =
            get_phi_operand(state, block, pred, rc.type(), std::get<var_state_reference>(phi.operands[i]));
         pred_operands[i] = {op};
         continue;
      }
      auto op = std::get<Operand>(phi.operands[i]);

      if (op.isUndefined()) {
         pred_operands[i] = std::vector<Operand>(rc.size(), Operand());
         continue;
      } else if (op.isConstant()) {
         assert(rc.size() == 2);
         pred_operands[i] = {
            Operand::c32(op.constantValue64() >> 32),
            Operand::c32(op.constantValue64() & 0xFFFFFFFF),
         };
         continue;
      }

      assert(op.isTemp());

      pred_operands[i].reserve(rc.size());

      auto split_temps = state.vector_splits.find(op.tempId());
      if (split_temps != state.vector_splits.end()) {
         for (auto& var_ref : split_temps->second)
            pred_operands[i].push_back(get_phi_operand(state, block, pred, rc.type(), var_ref));
      } else {
         if (op.isTemp() && state.scalar_states.find(op.tempId()) != state.scalar_states.end())
            op = get_phi_operand(state, block, pred, rc.type(), var_state_reference(false, op.tempId()));
         pred_operands[i] = {op};
      }
   }

   for (unsigned i = 0; i < rc.size(); ++i) {
      auto new_phi =
         create_instruction(rc.is_linear() ? aco_opcode::p_linear_phi : aco_opcode::p_phi,
                            Format::PSEUDO, preds->size(), 1);
      for (unsigned j = 0; j < preds->size(); ++j) {
         Operand& op = pred_operands[j][i];
         new_phi->operands[j] = op;
      }
      new_phi->definitions[0] = Definition(phi.split_defs[i]);

      instructions.insert(instructions.end(), aco_ptr<Instruction>(new_phi));
   }
}

} // namespace

void
split_vectors(Program* program)
{
   struct split_vector_state state(program);

   bool progress;
   do {
      progress = false;
      for (auto& block : program->blocks) {
         for (auto& instr : block.instructions) {
            if (instr->opcode == aco_opcode::p_phi || instr->opcode == aco_opcode::p_linear_phi ||
                instr->opcode == aco_opcode::p_parallelcopy) {
               /* It doesn't make much sense to split subdword vectors into (dword-sized) scalars. */
               if (instr->definitions[0].regClass().is_subdword() ||
                   std::any_of(instr->operands.begin(), instr->operands.end(),
                               [](const Operand& op) { return op.regClass().is_subdword(); })) {
                  progress |= state.temps_to_preserve.insert(instr->definitions[0].tempId()).second;
                  continue;
               }

               bool needs_split = state.temps_to_split.find(instr->definitions[0].tempId()) !=
                                  state.temps_to_split.end();
               bool needs_preserve = state.temps_to_preserve.find(instr->definitions[0].tempId()) != state.temps_to_preserve.end();
               for (auto& op : instr->operands) {
                  if (!op.isTemp())
                     continue;
                  if (state.temps_to_split.find(op.tempId()) != state.temps_to_split.end())
                     needs_split = true;
                  if (state.temps_to_preserve.find(op.tempId()) != state.temps_to_preserve.end())
                     needs_preserve = true;
                  if (needs_split && needs_preserve)
                     break;
               }
               if (needs_split) {
                  progress |= state.temps_to_split.insert(instr->definitions[0].tempId()).second;
                  for (auto& op2 : instr->operands) {
                     progress |= state.temps_to_split.insert(op2.tempId()).second;
                  }
               }
               if (needs_preserve) {
                  progress |= state.temps_to_preserve.insert(instr->definitions[0].tempId()).second;
                  for (auto& op2 : instr->operands) {
                     progress |= state.temps_to_preserve.insert(op2.tempId()).second;
                  }
               }
            }
            if (instr->opcode == aco_opcode::p_create_vector && instr->definitions[0].size() > 1) {
               if (instr->definitions[0].regClass().is_subdword() ||
                   std::any_of(instr->operands.begin(), instr->operands.end(),
                               [](const Operand& op) { return op.regClass().is_subdword(); })) {
                  progress |= state.temps_to_preserve.insert(instr->definitions[0].tempId()).second;
                  continue;
               }
               progress |= state.temps_to_split.insert(instr->definitions[0].tempId()).second;
               for (auto& op : instr->operands) {
                  if (op.isTemp() && op.size() > 1)
                     progress |= state.temps_to_split.insert(op.tempId()).second;
               }
               continue;
            }
            if ((instr->isVMEM() || instr->isSMEM()) && instr->opcode != aco_opcode::image_bvh64_intersect_ray) {
               for (auto& op : instr->operands) {
                  if (op.isTemp())
                     progress |= state.temps_to_preserve.insert(op.tempId()).second;
               }
            }
            if (instr->opcode != aco_opcode::p_split_vector &&
                instr->opcode != aco_opcode::p_extract_vector)
               continue;
            if (instr->definitions[0].regClass().is_subdword() ||
                std::any_of(instr->operands.begin(), instr->operands.end(),
                            [](const Operand& op) { return op.regClass().is_subdword(); })) {
               progress |= state.temps_to_preserve.insert(instr->definitions[0].tempId()).second;
               continue;
            }
            if (!instr->operands[0].isTemp() || instr->operands[0].size() == 1)
               continue;
            progress |= state.temps_to_split.insert(instr->operands[0].tempId()).second;
         }
      }
   } while (progress);

   for (auto temp : state.temps_to_preserve)
      state.temps_to_split.erase(temp);

   for (auto& block : program->blocks) {
      std::vector<aco_ptr<Instruction>> instructions;

      state.working_block = {
         block.index,
         &instructions,
      };

      for (auto& instr : block.instructions) {
         Builder bld(program, &instructions);

         instr->pass_flags = 0;

         if (instr->opcode == aco_opcode::p_create_vector) {
            if (state.temps_to_split.find(instr->definitions[0].tempId()) ==
                state.temps_to_split.end()) {
               /* Keep the definition as a vector - only rename operands if necessary */
               bool should_rewrite_operands = false;
               for (auto& op : instr->operands) {
                  if (!op.isTemp())
                     continue;
                  update_operand(state, block.index, op, bld);
                  /* If we create a larger vector from smaller vectors, and one of the smaller
                   * vectors has been split, we need to rewrite the operands to include each
                   * split component separately.
                   */
                  if (state.vector_splits.find(op.tempId()) != state.vector_splits.end())
                     should_rewrite_operands = true;
               }

               if (should_rewrite_operands) {
                  uint32_t num_ops = 0;
                  for (auto& op : instr->operands) {
                     if (op.isTemp() &&
                         state.vector_splits.find(op.tempId()) != state.vector_splits.end())
                        num_ops += op.size();
                     else
                        ++num_ops;
                  }
                  auto new_instr =
                     create_instruction(aco_opcode::p_create_vector, Format::PSEUDO, num_ops, 1);
                  new_instr->definitions[0] = instr->definitions[0];

                  uint32_t op_idx = 0;
                  for (auto& op : instr->operands) {
                     if (op.isTemp() &&
                         state.vector_splits.find(op.tempId()) != state.vector_splits.end()) {
                        auto& vars = state.vector_splits[op.tempId()];
                        for (unsigned i = 0; i < op.size(); ++i) {
                           new_instr->operands[op_idx++] =
                              state.resolve_var_state_ref(vars[i]).get_output(block.index);
                        }
                     } else {
                        new_instr->operands[op_idx++] = op;
                     }
                  }
                  instr = aco_ptr<Instruction>(new_instr);
               }

               instructions.emplace_back(std::move(instr));
               continue;
            }
            assert(!instr->definitions[0].regClass().is_subdword());

            if (instr->definitions[0].size() == 1 &&
                !instr->definitions[0].regClass().is_subdword()) {
               assert(false);
               update_operand(state, block.index, instr->operands[0], bld);
               continue;
            }

            std::vector<Operand> vec_defs;
            vec_defs.reserve(instr->operands.size());
            for (const auto& op : instr->operands) {
               if (!op.isTemp()) {
                  for (unsigned i = 0; i < op.size(); ++i) {
                     if (op.isConstant())
                        vec_defs.emplace_back(Operand::c32((op.constantValue64() >> (i * 32))));
                     else
                        vec_defs.emplace_back();
                  }
                  continue;
               }
               if (op.size() != 1) {
                  const auto& temps = state.vector_splits[op.tempId()];
                  for (auto ref : temps)
                     vec_defs.emplace_back(
                        state.resolve_var_state_ref(ref).get_output(block.index));
                  continue;
               }

               Operand new_op = op;
               update_operand(state, block.index, new_op, bld);
               vec_defs.emplace_back(new_op);
            }

            auto& def = instr->definitions[0];
            state.split_vector(def.tempId(), block.index, vec_defs);
            continue;
         } else if ((instr->opcode == aco_opcode::p_split_vector ||
                    instr->opcode == aco_opcode::p_parallelcopy) &&
                    state.temps_to_split.find(instr->operands[0].tempId()) !=
                       state.temps_to_split.end()) {
            if (instr->operands[0].isConstant()) {
               assert(instr->operands[0].size() == 2);

               Operand comps[2] = {
                  bld.copy(bld.def(s1), Operand::c32(instr->operands[0].constantValue64() >> 32)),
                  bld.copy(bld.def(s1),
                           Operand::c32(instr->operands[0].constantValue64() & 0xFFFFFFFF)),
               };

               if (instr->definitions.size() == 2) {
                  state.remap_temp(instr->definitions[0].tempId(), block.index, comps[0]);
                  state.remap_temp(instr->definitions[1].tempId(), block.index, comps[1]);
               } else {
                  state.split_vector(
                     instr->definitions[0].tempId(), block.index,
                     {comps[0], comps[1]});
               }
            } else if (!instr->operands[0].isTemp() && instr->operands[0].isFixed()) {
               PhysReg reg = instr->operands[0].physReg();

               std::vector<std::pair<Definition, Operand>> copies;
               copies.reserve(instr->definitions.size());

               for (unsigned i = 0; i < instr->definitions.size(); ++i) {
                  if (instr->definitions[i].size() == 1) {
                     copies.emplace_back(instr->definitions[i], instr->operands[0]);
                     continue;
                  }

                  std::vector<Operand> vec_defs;
                  vec_defs.reserve(instr->definitions[i].size());
                  for (unsigned j = 0; j < instr->definitions[i].size(); ++j) {
                     RegClass elem_rc = RegClass(instr->definitions[i].regClass().type(), 1);
                     if (instr->definitions[i].regClass().is_linear_vgpr())
                        elem_rc = elem_rc.as_linear();
                     Temp tmp = program->allocateTmp(elem_rc);
                     state.remap_temp(tmp.id(), block.index, Operand(tmp));
                     copies.emplace_back(Definition(tmp), Operand(reg, elem_rc));
                     vec_defs.emplace_back(tmp);

                     reg = reg.advance(4);
                  }
                  state.split_vector(instr->definitions[i].tempId(), block.index, vec_defs);
               }

               for (unsigned i = 0; i < copies.size(); ++i) {
                  auto parallelcopy =
                     create_instruction(aco_opcode::p_parallelcopy, Format::PSEUDO, 1, 1);
                  parallelcopy->definitions[0] = copies[i].first;
                  parallelcopy->operands[0] = copies[i].second;
                  instructions.emplace_back(aco_ptr<Instruction>(parallelcopy));
               }
            } else {
               auto id = instr->operands[0].tempId();
               auto& vars = state.vector_splits[id];

               unsigned component_idx = 0;
               for (unsigned i = 0; i < instr->definitions.size(); ++i) {
                  if (instr->definitions[i].size() == 1) {
                     state.remap_temp(
                        instr->definitions[i].tempId(), block.index,
                        state.resolve_var_state_ref(vars[component_idx++]).get_output(block.index));
                     continue;
                  }

                  std::vector<Operand> vec_defs;
                  vec_defs.reserve(instr->definitions[i].size());
                  for (unsigned j = 0; j < instr->definitions[i].size(); ++j) {
                     RegClass elem_rc = RegClass(instr->definitions[i].regClass().type(), 1);
                     if (instr->definitions[i].regClass().is_linear_vgpr())
                        elem_rc = elem_rc.as_linear();
                     vec_defs.push_back(
                        state.resolve_var_state_ref(vars[component_idx++]).get_output(block.index));
                  }
                  state.split_vector(instr->definitions[i].tempId(), block.index, vec_defs);
               }
            }
            continue;
         } else if (instr->opcode == aco_opcode::p_extract_vector
            && state.temps_to_split.find(instr->definitions[0].tempId()) !=
                state.temps_to_split.end()) {
            if (std::any_of(instr->operands.begin(), instr->operands.end(),
                            [](const auto& operand) { return operand.hasRegClass() && operand.regClass().is_subdword(); })) {
               instructions.emplace_back(std::move(instr));
               continue;
            }
            auto& def = instr->definitions[0];
            auto id = instr->operands[0].tempId();
            RegClass elem_rc = RegClass(def.regClass().type(), 1);
            if (def.regClass().is_linear_vgpr())
               elem_rc = elem_rc.as_linear();

            auto& vars = state.vector_splits[id];

            if (def.size() == 1) {
               state.remap_temp(
                  def.tempId(), block.index,
                  state.resolve_var_state_ref(vars[instr->operands[1].constantValue()])
                     .get_output(block.index));
            } else {
               std::vector<Operand> vec_defs;
               vec_defs.reserve(instr->operands[0].size());
               for (unsigned i = 0; i < def.size(); ++i)
                  vec_defs.push_back(
                     state.resolve_var_state_ref(vars[instr->operands[1].constantValue() + i])
                        .get_output(block.index));
               state.split_vector(instr->definitions[0].tempId(), block.index, vec_defs);
            }
            continue;
         } else if (instr->opcode == aco_opcode::p_phi ||
                    instr->opcode == aco_opcode::p_linear_phi) {
            if (instr->definitions[0].size() == 1 ||
                state.temps_to_split.find(instr->definitions[0].tempId()) ==
                   state.temps_to_split.end()) {
               auto* preds = instr->opcode == aco_opcode::p_linear_phi ? &block.linear_preds
                                                                       : &block.logical_preds;
               if (!(block.kind & block_kind_loop_header)) {
                  for (unsigned i = 0; i < instr->operands.size(); ++i) {
                     auto& op = instr->operands[i];
                     if (op.isTemp()) {
                        if (state.scalar_states.find(op.tempId()) == state.scalar_states.end())
                           continue;
                        op = get_phi_operand(state, block.index, (*preds)[i],
                                             instr->definitions[0].regClass().type(),
                                             var_state_reference(false, op.tempId()));
                     }
                  }
               } else {
                  state.loop_header_worklist = std::min(state.loop_header_worklist, block.index);
               }
               if (instr->definitions[0].size() == 1)
                  state.remap_temp(instr->definitions[0].tempId(), block.index,
                                   Operand(instr->definitions[0].getTemp()));
               instructions.emplace_back(std::move(instr));
               continue;
            }
            RegClass elem_rc = RegClass(instr->definitions[0].regClass().type(), 1);
            if (instr->definitions[0].regClass().is_linear_vgpr())
               elem_rc = elem_rc.as_linear();

            std::vector<Temp> defs;
            std::vector<Operand> split_ops;
            defs.reserve(instr->definitions[0].size());
            split_ops.reserve(instr->definitions[0].size());
            for (unsigned i = 0; i < instr->definitions[0].size(); ++i) {
               auto tmp = state.program->allocateTmp(elem_rc);
               defs.push_back(tmp);
               split_ops.emplace_back(tmp);
            }
            state.split_vector(instr->definitions[0].tempId(), block.index, split_ops);

            std::vector<std::variant<var_state_reference, Operand>> operands;
            operands.reserve(instr->operands.size());
            std::copy(instr->operands.begin(), instr->operands.end(), std::back_inserter(operands));
            auto phi =
               phi_info{instr->definitions[0].regClass(), std::move(defs), std::move(operands)};
            /* We can't process header phis just yet. We'll come back to this when we handled all
             * continue blocks.
             */
            if (block.kind & block_kind_loop_header) {
               add_header_phi(&state, block.index, std::move(phi));
               continue;
            }

            lower_phi(state, block.index, phi, instructions);

            continue;
         }

         std::vector<aco_ptr<Instruction>> split_vectors;

         BITSET_DECLARE(mask, 128) = {0};
         for (unsigned i = 0; i < instr->operands.size(); ++i) {
            const auto& op = instr->operands[i];
            if (!op.isTemp())
               continue;
            if (op.regClass().is_linear_vgpr())
               continue;
            if (op.regClass().is_subdword())
               continue;
            BITSET_SET(mask, i);
         }

         for (unsigned i = 0; i < instr->operands.size(); ++i) {
            if (!BITSET_TEST(mask, i))
               continue;

            auto& op = instr->operands[i];
            if (op.size() == 1) {
               update_operand(state, block.index, op, bld);
               continue;
            }

            const auto id = op.tempId();
            auto it = state.vector_splits.find(id);
            if (it == state.vector_splits.end())
               continue;

            auto& vars = it->second;

            auto create_vec =
               create_instruction(aco_opcode::p_create_vector, Format::PSEUDO, op.size(), 1);
            create_vec->definitions[0] = bld.def(op.regClass());
            for (unsigned j = 0; j < op.size(); ++j)
               create_vec->operands[j] =
                  state.resolve_var_state_ref(vars[j]).get_output(block.index);
            instructions.emplace_back(aco_ptr<Instruction>(create_vec));

            op.setTemp(create_vec->definitions[0].getTemp());
            for (unsigned j = i + 1; j < instr->operands.size(); ++j) {
               if (instr->operands[j].isTemp() && instr->operands[j].tempId() == id) {
                  instr->operands[j].setTemp(op.getTemp());
                  BITSET_CLEAR(mask, j);
               }
            }

            if (instr->isBranch())
               continue;

            std::vector<aco_ptr<Instruction>> split_copies;

            std::vector<Operand> split_vec_defs;
            split_vec_defs.reserve(op.size());
            auto split_vec =
               create_instruction(aco_opcode::p_split_vector, Format::PSEUDO, 1, op.size());
            split_vec->operands[0] = Operand(create_vec->definitions[0].getTemp());

            for (unsigned j = 0; j < op.size(); ++j) {
               split_vec->definitions[j] = bld.def(op.regClass().type(), 1);
               Temp tmp = split_vec->definitions[j].getTemp();
               split_vec_defs.emplace_back(tmp);
            }

            split_vec->pass_flags = id;

            state.split_vector(id, block.index, split_vec_defs);
            split_vectors.emplace_back(aco_ptr<Instruction>(split_vec));
            split_vectors.insert(split_vectors.end(), std::move_iterator(split_copies.begin()),
                                 std::move_iterator(split_copies.end()));
         }

         for (auto& def : instr->definitions) {
            if (instr->isBranch())
               break;
            if (!def.isTemp())
               continue;
            if (def.regClass().is_linear_vgpr() || def.regClass().is_subdword())
               continue;
            if (def.size() == 1) {
               state.remap_temp(def.tempId(), block.index, Operand(def.getTemp()));
               continue;
            }
            if (state.temps_to_split.find(def.tempId()) == state.temps_to_split.end())
               continue;

            RegClass elem_rc = RegClass(def.regClass().type(), 1);
            if (def.regClass().is_linear_vgpr())
               elem_rc = elem_rc.as_linear();

            std::vector<Operand> split_vec_defs;
            split_vec_defs.reserve(def.size());
            auto split_vec =
               create_instruction(aco_opcode::p_split_vector, Format::PSEUDO, 1, def.size());
            split_vec->operands[0] = Operand(def.getTemp());

            for (unsigned i = 0; i < def.size(); ++i) {
               split_vec->definitions[i] = bld.def(elem_rc);
               split_vec_defs.emplace_back(split_vec->definitions[i].getTemp());
            }

            split_vec->pass_flags = def.tempId();

            state.split_vector(def.tempId(), block.index, split_vec_defs);
            split_vectors.emplace_back(aco_ptr<Instruction>(split_vec));
         }

         instructions.emplace_back(std::move(instr));

         instructions.insert(instructions.end(), std::move_iterator(split_vectors.begin()),
                             std::move_iterator(split_vectors.end()));
      }

      block.instructions = std::move(instructions);
   }

   Block* header_block =
      state.loop_header_worklist != -1u ? &program->blocks[state.loop_header_worklist] : NULL;

   while (state.loop_header_worklist != -1u) {
      std::vector<aco_ptr<Instruction>> new_phis;
      state.working_block = {
         header_block->index,
         &new_phis,
      };

      for (auto& instr : header_block->instructions) {
         if (instr->opcode != aco_opcode::p_phi && instr->opcode != aco_opcode::p_linear_phi)
            break;

         auto* preds = instr->opcode == aco_opcode::p_linear_phi ? &header_block->linear_preds
                                                                 : &header_block->logical_preds;
         for (unsigned i = 0; i < instr->operands.size(); ++i) {
            auto& op = instr->operands[i];
            if (!op.isTemp())
               continue;
            if (state.scalar_states.find(op.tempId()) == state.scalar_states.end())
               continue;

            op = get_phi_operand(state, header_block->index, (*preds)[i], instr->definitions[0].regClass().type(),
                                 var_state_reference(false, op.tempId()));
         }
      }

      auto phis = state.header_phis[header_block->index];
      for (auto& phi : phis)
         lower_phi(state, header_block->index, phi, new_phis);
      state.header_phis[header_block->index].clear();
      header_block->instructions.insert(header_block->instructions.begin(),
                                        std::move_iterator(new_phis.begin()),
                                        std::move_iterator(new_phis.end()));

      if (state.loop_header_worklist == header_block->index)
         ++state.loop_header_worklist;

      header_block = &program->blocks[state.loop_header_worklist];
      bool done = false;
      while (!(header_block->kind & block_kind_loop_header)) {
         if (state.loop_header_worklist == program->blocks.size() - 1) {
            done = true;
            break;
         }
         header_block = &program->blocks[++state.loop_header_worklist];
      }
      if (done)
         break;
   }

   for (auto& block : program->blocks) {
      for (auto it = block.instructions.begin(); it != block.instructions.end();) {
         if ((*it)->opcode != aco_opcode::p_split_vector || !(*it)->pass_flags) {
            ++it;
            continue;
         }

         auto components = state.vector_splits[(*it)->pass_flags];
         bool used = false;
         for (unsigned i = 0; i < components.size(); ++i) {
            bool is_dup = false;
            for (unsigned j = i + 1; j < components.size(); ++j) {
               if (components[j] == components[i]) {
                  is_dup = true;
                  break;
               }
            }
            if (is_dup)
               continue;
            var_state& var = state.resolve_var_state_ref(components[i]);
            if (var.output_is_used(block.index, (*it)->definitions[i].tempId())) {
               used = true;
               break;
            }
         }
         if (used)
            ++it;
         else
            it = block.instructions.erase(it);
      }
   }
}

} // namespace aco