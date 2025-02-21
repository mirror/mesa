// Copyright © 2025 Valve Corporation
// SPDX-License-Identifier: MIT

use crate::ir::*;
use crate::opt_jump_thread;

pub struct PredicationInfo {
    // Blocks which are predecessors of an if-then-else
    if_blocks: Vec<usize>,
}

struct IfBranches([usize; 2]);

impl Function {
    fn block_precedes_if_then_else(
        &self,
        a: usize,
    ) -> Option<(IfBranches, usize)> {
        //    a
        //   / \
        //  b   c
        //   \ /
        //    d
        let &[b, c] = self.blocks.succ_indices(a) else {
            return None;
        };
        let &[d] = self.blocks.succ_indices(b) else {
            return None;
        };
        if &[d] == self.blocks.succ_indices(c) {
            Some((IfBranches([b, c]), d))
        } else {
            None
        }
    }

    /// Is the if statement cheap enough to predicate?
    fn should_predicate(&mut self, branches: IfBranches) -> bool {
        let mut cost = 0;
        for branch in branches.0 {
            cost += self.blocks[branch].instrs.len();
        }
        if cost > 4 {
            return false;
        }
        for branch in branches.0 {
            for instr in &self.blocks[branch].instrs {
                if !instr.pred.is_true() {
                    return false;
                }
            }
        }
        true
    }

    pub fn opt_predication_prepare(&mut self) -> PredicationInfo {
        let mut predication_info = PredicationInfo {
            if_blocks: Vec::new(),
        };

        for i in 0..self.blocks.len() {
            if let Some((branches, succ)) = self.block_precedes_if_then_else(i)
            {
                if self.should_predicate(branches) {
                    predication_info.if_blocks.push(i);
                    self.blocks[succ]
                        .instrs
                        .retain(|instr| !matches!(instr.op, Op::BSync(_)));
                }
            }
        }

        if !predication_info.if_blocks.is_empty() {
            self.opt_dce();
        }

        predication_info
    }

    pub fn opt_predication(&mut self, predication_info: PredicationInfo) {
        let mut changed = false;
        for block in predication_info.if_blocks {
            let Some((branches, succ)) =
                self.block_precedes_if_then_else(block)
            else {
                panic!("CFG modified between opt_predication_prepare and opt_predication")
            };

            assert_eq!(branches.0[0], block + 1);
            assert_eq!(branches.0[1], block + 2);
            assert_eq!(succ, block + 3);

            let mut instrs0 =
                std::mem::take(&mut self.blocks[branches.0[0]].instrs);
            let mut instrs1 =
                std::mem::take(&mut self.blocks[branches.0[1]].instrs);

            let branch_to_succ0 = instrs0.pop().unwrap();
            assert!(branch_to_succ0.is_branch());
            assert!(branch_to_succ0.pred.is_true());
            let branch_to_succ1 = instrs1.pop().unwrap();
            assert!(branch_to_succ1.is_branch());
            assert!(branch_to_succ1.pred.is_true());

            let head_instrs = &mut self.blocks[block].instrs;
            let orig_branch = head_instrs.pop().unwrap();
            assert!(orig_branch.is_branch());
            let predicate = orig_branch.pred;

            for instr in &mut instrs0 {
                instr.pred = predicate.bnot();
            }

            for instr in &mut instrs1 {
                instr.pred = predicate;
            }

            head_instrs.append(&mut instrs0);
            head_instrs.append(&mut instrs1);
            head_instrs.push(branch_to_succ0);
            changed = true;
        }

        if changed {
            opt_jump_thread::rewrite_cfg(self);
        }
    }
}

impl Shader<'_> {
    pub fn opt_predication_prepare(&mut self) -> PredicationInfo {
        assert_eq!(self.functions.len(), 1);
        self.functions[0].opt_predication_prepare()
    }

    pub fn opt_predication(&mut self, predication_info: PredicationInfo) {
        assert_eq!(self.functions.len(), 1);
        self.functions[0].opt_predication(predication_info)
    }
}
