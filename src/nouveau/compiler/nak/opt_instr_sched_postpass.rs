// Copyright © 2024 Valve Corporation
// SPDX-License-Identifier: MIT

use crate::ir::*;
use crate::opt_instr_sched_common::*;
use crate::sched_common::{
    paw_latency, raw_latency, war_latency, waw_latency, RegTracker,
};
use std::cmp::max;
use std::cmp::Reverse;
use std::collections::BinaryHeap;

struct RegUse<T: Clone> {
    reads: Vec<T>,
    write: Option<T>,
}

impl<T: Clone> RegUse<T> {
    pub fn new() -> Self {
        RegUse {
            reads: Vec::new(),
            write: None,
        }
    }

    pub fn add_read(&mut self, dep: T) {
        self.reads.push(dep);
    }

    pub fn set_write(&mut self, dep: T) {
        self.write = Some(dep);
        self.reads.clear();
    }
}

fn generate_dep_graph(
    sm: &dyn ShaderModel,
    instrs: &Vec<Box<Instr>>,
) -> DepGraph {
    let mut g = DepGraph::new((0..instrs.len()).map(|_| Default::default()));

    // Maps registers to RegUse<ip, src_dst_idx>.  Predicates are
    // represented by  src_idx = usize::MAX.
    let mut uses: RegTracker<RegUse<(usize, usize)>> =
        RegTracker::new_with(&|| RegUse::new());

    let mut last_memory_op = None;

    for ip in (0..instrs.len()).rev() {
        let instr = &instrs[ip];

        if side_effect_type(&instr.op) == SideEffect::Memory {
            if let Some(mem_ip) = last_memory_op {
                g.add_edge(ip, mem_ip, EdgeLabel { latency: 0 });
            }
            last_memory_op = Some(ip);
        }

        uses.for_each_instr_dst_mut(instr, |i, u| {
            if let Some((w_ip, w_dst_idx)) = u.write {
                let latency = waw_latency(
                    sm.sm(),
                    &instr.op,
                    i,
                    &instrs[w_ip].op,
                    w_dst_idx,
                );
                g.add_edge(ip, w_ip, EdgeLabel { latency });
            }

            for &(r_ip, r_src_idx) in &u.reads {
                let mut latency = if r_src_idx == usize::MAX {
                    paw_latency(sm.sm(), &instr.op, i)
                } else {
                    raw_latency(
                        sm.sm(),
                        &instr.op,
                        i,
                        &instrs[r_ip].op,
                        r_src_idx,
                    )
                };
                if !instr.has_fixed_latency(sm.sm()) {
                    latency = max(
                        latency,
                        estimate_variable_latency(sm.sm(), &instr.op),
                    );
                }
                g.add_edge(ip, r_ip, EdgeLabel { latency });
            }
        });
        uses.for_each_instr_src_mut(instr, |i, u| {
            if let Some((w_ip, w_dst_idx)) = u.write {
                let latency = war_latency(
                    sm.sm(),
                    &instr.op,
                    i,
                    &instrs[w_ip].op,
                    w_dst_idx,
                );
                g.add_edge(ip, w_ip, EdgeLabel { latency });
            }
        });

        uses.for_each_instr_pred_mut(instr, |c| {
            c.add_read((ip, usize::MAX));
        });
        uses.for_each_instr_src_mut(instr, |i, c| {
            c.add_read((ip, i));
        });
        uses.for_each_instr_dst_mut(instr, |i, c| {
            c.set_write((ip, i));
        });
    }

    g
}

fn generate_order(g: &mut DepGraph, init_ready_list: Vec<usize>) -> Vec<usize> {
    let mut ready_instrs: BinaryHeap<ReadyInstr> = init_ready_list
        .into_iter()
        .map(|i| ReadyInstr::new(g, i))
        .collect();
    let mut future_ready_instrs = BinaryHeap::new();

    let mut current_cycle = 0;
    let mut instr_order = Vec::with_capacity(g.nodes.len());
    loop {
        // Move ready instructions to the ready list
        loop {
            match future_ready_instrs.peek() {
                None => break,
                Some(FutureReadyInstr {
                    ready_cycle: std::cmp::Reverse(ready_cycle),
                    index,
                }) => {
                    if current_cycle >= *ready_cycle {
                        ready_instrs.push(ReadyInstr::new(g, *index));
                        future_ready_instrs.pop();
                    } else {
                        break;
                    }
                }
            }
        }

        // Pick a ready instruction
        let next_idx = match ready_instrs.pop() {
            None => {
                match future_ready_instrs.peek() {
                    None => break, // Both lists are empty. We're done!
                    Some(&FutureReadyInstr {
                        ready_cycle: Reverse(ready_cycle),
                        ..
                    }) => {
                        // Fast-forward time to when the next instr is ready
                        assert!(ready_cycle > current_cycle);
                        current_cycle = ready_cycle;
                        continue;
                    }
                }
            }
            Some(ReadyInstr { index, .. }) => index,
        };

        // Schedule the instuction
        let outgoing_edges =
            std::mem::take(&mut g.nodes[next_idx].outgoing_edges);
        for edge in outgoing_edges.into_iter() {
            let dep_instr = &mut g.nodes[edge.head_idx].label;
            dep_instr.ready_cycle =
                max(dep_instr.ready_cycle, current_cycle + edge.label.latency);
            dep_instr.num_uses -= 1;
            if dep_instr.num_uses <= 0 {
                future_ready_instrs
                    .push(FutureReadyInstr::new(g, edge.head_idx));
            }
        }

        instr_order.push(next_idx);
        current_cycle += 1;
    }
    return instr_order;
}

fn sched_buffer(
    sm: &dyn ShaderModel,
    instrs: Vec<Box<Instr>>,
) -> impl Iterator<Item = Box<Instr>> {
    let mut g = generate_dep_graph(sm, &instrs);
    let init_ready_list = calc_statistics(&mut g);
    // save_graphviz(&instrs, &g).unwrap();
    g.reverse();
    let new_order = generate_order(&mut g, init_ready_list);

    // Apply the new instruction order
    let mut instrs: Vec<Option<Box<Instr>>> =
        instrs.into_iter().map(|instr| Some(instr)).collect();
    new_order.into_iter().rev().map(move |i| {
        std::mem::take(&mut instrs[i]).expect("Instruction scheduled twice")
    })
}

impl Function {
    pub fn opt_instr_sched_postpass(&mut self, sm: &dyn ShaderModel) {
        for block in &mut self.blocks {
            let orig_instr_count = block.instrs.len();
            let mut reorder_buffer = Vec::new();
            for instr in std::mem::take(&mut block.instrs) {
                match side_effect_type(&instr.op) {
                    SideEffect::None | SideEffect::Memory => {
                        reorder_buffer.push(instr);
                    }
                    SideEffect::Barrier => {
                        if !reorder_buffer.is_empty() {
                            block.instrs.extend(sched_buffer(
                                sm,
                                std::mem::take(&mut reorder_buffer),
                            ));
                        }
                        block.instrs.push(instr);
                    }
                }
            }
            if !reorder_buffer.is_empty() {
                block.instrs.extend(sched_buffer(sm, reorder_buffer));
            }
            assert_eq!(orig_instr_count, block.instrs.len());
        }
    }
}

impl Shader<'_> {
    /// Post-RA instruction scheduling
    ///
    /// Uses the popular latency-weighted-depth heuristic.
    /// See eg. Cooper & Torczon's "Engineering A Compiler", 3rd ed.
    /// Chapter 12.3 "Local scheduling"
    pub fn opt_instr_sched_postpass(&mut self) {
        for f in &mut self.functions {
            f.opt_instr_sched_postpass(self.sm);
        }
    }
}
