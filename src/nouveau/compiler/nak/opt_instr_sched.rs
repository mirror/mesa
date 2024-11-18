// Copyright © 2024 Valve Corporation
// SPDX-License-Identifier: MIT

use crate::ir::*;
use crate::sched_common::{
    paw_latency, raw_latency, war_latency, waw_latency, RegTracker,
};
use std::cmp::max;
use std::cmp::Reverse;
use std::collections::BinaryHeap;

mod graph {
    pub struct Edge<EdgeLabel> {
        pub label: EdgeLabel,
        pub head_idx: usize,
    }

    pub struct Node<NodeLabel, EdgeLabel> {
        pub label: NodeLabel,
        pub outgoing_edges: Vec<Edge<EdgeLabel>>,
    }

    pub struct Graph<NodeLabel, EdgeLabel> {
        pub nodes: Vec<Node<NodeLabel, EdgeLabel>>,
    }

    impl<NodeLabel, EdgeLabel> Graph<NodeLabel, EdgeLabel> {
        pub fn new(node_labels: impl Iterator<Item = NodeLabel>) -> Self {
            let nodes = node_labels
                .map(|label| Node {
                    label,
                    outgoing_edges: Vec::new(),
                })
                .collect();

            Graph { nodes }
        }

        pub fn add_edge(
            &mut self,
            tail_idx: usize,
            head_idx: usize,
            label: EdgeLabel,
        ) {
            assert!(head_idx < self.nodes.len());
            self.nodes[tail_idx]
                .outgoing_edges
                .push(Edge { label, head_idx });
        }

        pub fn reverse(&mut self) {
            let old_edges: Vec<_> = self
                .nodes
                .iter_mut()
                .map(|node| std::mem::take(&mut node.outgoing_edges))
                .collect();

            for (old_tail_idx, old_outgoing_edges) in
                old_edges.into_iter().enumerate()
            {
                for Edge {
                    label,
                    head_idx: old_head_idx,
                } in old_outgoing_edges.into_iter()
                {
                    self.add_edge(old_head_idx, old_tail_idx, label);
                }
            }
        }
    }
}

#[derive(Eq, PartialEq)]
enum SideEffect {
    /// No side effect (ALU-like)
    None,

    /// Instruction reads or writes memory
    ///
    /// This will be serialized with respect to other
    /// SideEffect::Memory instructions
    Memory,

    /// This instcuction is a full code motion barrier
    ///
    /// No other instruction will be re-ordered with respect to this one
    Barrier,
}

fn side_effect_type(op: &Op) -> SideEffect {
    match op {
        // Float ALU
        Op::F2FP(_)
        | Op::FAdd(_)
        | Op::FFma(_)
        | Op::FMnMx(_)
        | Op::FMul(_)
        | Op::FSet(_)
        | Op::FSetP(_)
        | Op::HAdd2(_)
        | Op::HFma2(_)
        | Op::HMul2(_)
        | Op::HSet2(_)
        | Op::HSetP2(_)
        | Op::HMnMx2(_)
        | Op::FSwzAdd(_) => SideEffect::None,

        // Multi-function unit
        Op::Rro(_) | Op::MuFu(_) => SideEffect::None,

        // Double-precision float ALU
        Op::DAdd(_)
        | Op::DFma(_)
        | Op::DMnMx(_)
        | Op::DMul(_)
        | Op::DSetP(_) => SideEffect::None,

        // Integer ALU
        Op::BRev(_)
        | Op::Flo(_)
        | Op::PopC(_)
        | Op::IMad(_)
        | Op::IMul(_)
        | Op::BMsk(_)
        | Op::IAbs(_)
        | Op::IAdd2(_)
        | Op::IAdd2X(_)
        | Op::IAdd3(_)
        | Op::IAdd3X(_)
        | Op::IDp4(_)
        | Op::IMad64(_)
        | Op::IMnMx(_)
        | Op::ISetP(_)
        | Op::Lop2(_)
        | Op::Lop3(_)
        | Op::Shf(_)
        | Op::Shl(_)
        | Op::Shr(_)
        | Op::Bfe(_) => SideEffect::None,

        // Conversions
        Op::F2F(_) | Op::F2I(_) | Op::I2F(_) | Op::I2I(_) | Op::FRnd(_) => {
            SideEffect::None
        }

        // Move ops
        Op::Mov(_) | Op::Prmt(_) | Op::Sel(_) => SideEffect::None,
        Op::Shfl(_) => SideEffect::None,

        // Predicate ops
        Op::PLop3(_) | Op::PSetP(_) => SideEffect::None,

        // Uniform ops
        Op::R2UR(_) => SideEffect::None,

        // Texture ops
        Op::Tex(_)
        | Op::Tld(_)
        | Op::Tld4(_)
        | Op::Tmml(_)
        | Op::Txd(_)
        | Op::Txq(_) => SideEffect::Memory,

        // Surface ops
        Op::SuLd(_) | Op::SuSt(_) | Op::SuAtom(_) => SideEffect::Memory,

        // Memory ops
        Op::Ipa(_) | Op::Ldc(_) => SideEffect::None,
        Op::Ld(_)
        | Op::St(_)
        | Op::Atom(_)
        | Op::AL2P(_)
        | Op::ALd(_)
        | Op::ASt(_)
        | Op::CCtl(_)
        | Op::LdTram(_)
        | Op::MemBar(_) => SideEffect::Memory,

        // Control-flow ops
        Op::BClear(_)
        | Op::Break(_)
        | Op::BSSy(_)
        | Op::BSync(_)
        | Op::SSy(_)
        | Op::Sync(_)
        | Op::Brk(_)
        | Op::PBk(_)
        | Op::Cont(_)
        | Op::PCnt(_)
        | Op::Bra(_)
        | Op::Exit(_)
        | Op::WarpSync(_) => SideEffect::Barrier,

        // We don't model the barrier register yet, so serialize these
        Op::BMov(_) => SideEffect::Memory,

        // Geometry ops
        Op::Out(_) | Op::OutFinal(_) => SideEffect::Barrier,

        // Miscellaneous ops
        Op::Bar(_)
        | Op::CS2R(_)
        | Op::Isberd(_)
        | Op::Kill(_)
        | Op::PixLd(_)
        | Op::S2R(_) => SideEffect::Barrier,
        Op::Nop(_) | Op::Vote(_) => SideEffect::None,

        // Virtual ops
        Op::Undef(_)
        | Op::SrcBar(_)
        | Op::PhiSrcs(_)
        | Op::PhiDsts(_)
        | Op::Copy(_)
        | Op::Pin(_)
        | Op::Unpin(_)
        | Op::Swap(_)
        | Op::ParCopy(_)
        | Op::RegOut(_)
        | Op::Annotate(_) => {
            panic!("Not a hardware opcode")
        }
    }
}

/// Try to guess how many cycles a variable latency instruction will take
///
/// These values are based on the cycle estimates from "Dissecting the NVidia
/// Turing T4 GPU via Microbenchmarking" https://arxiv.org/pdf/1903.07486
/// Memory instructions were copied from L1 data cache latencies.
/// For instructions not mentioned in the paper, I made up numbers.
/// This could probably be improved.
fn estimate_variable_latency(sm: u8, op: &Op) -> u32 {
    match op {
        // Multi-function unit
        Op::Rro(_) | Op::MuFu(_) => 15,

        // Double-precision float ALU
        Op::DFma(_) | Op::DSetP(_) => 54,
        Op::DAdd(_) | Op::DMnMx(_) | Op::DMul(_) => 48,

        // Integer ALU
        Op::BRev(_) | Op::Flo(_) | Op::PopC(_) => 15,
        Op::IMad(_) | Op::IMul(_) => {
            assert!(sm < 70);
            86
        }

        // Conversions
        Op::F2F(_) | Op::F2I(_) | Op::I2F(_) | Op::I2I(_) | Op::FRnd(_) => 15,

        // Move ops
        Op::Shfl(_) => 15,

        // Uniform ops
        Op::R2UR(_) => 15,

        // Texture ops
        Op::Tex(_)
        | Op::Tld(_)
        | Op::Tld4(_)
        | Op::Tmml(_)
        | Op::Txd(_)
        | Op::Txq(_) => 32,

        // Surface ops
        Op::SuLd(_) | Op::SuSt(_) | Op::SuAtom(_) => 32,

        // Memory ops
        Op::Ldc(_) => 4,

        Op::Ld(_)
        | Op::St(_)
        | Op::Atom(_)
        | Op::AL2P(_)
        | Op::ALd(_)
        | Op::ASt(_)
        | Op::Ipa(_)
        | Op::CCtl(_)
        | Op::LdTram(_)
        | Op::MemBar(_) => 32,

        _ => panic!("Unknown variable latency op {op}"),
    }
}

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

struct NodeLabel {
    cycles_to_end: u32,
    num_uses: u32,
    ready_cycle: u32,
}

type DepGraph = graph::Graph<NodeLabel, u32>;

fn generate_dep_graph(
    sm: &dyn ShaderModel,
    instrs: &Vec<Box<Instr>>,
) -> DepGraph {
    let mut g = DepGraph::new((0..instrs.len()).map(|_| NodeLabel {
        cycles_to_end: 0,
        num_uses: 0,
        ready_cycle: 0,
    }));

    // Maps registers to RegUse<ip, src_dst_idx>.  Predicates are
    // represented by  src_idx = usize::MAX.
    let mut uses: RegTracker<RegUse<(usize, usize)>> =
        RegTracker::new_with(&|| RegUse::new());

    let mut last_memory_op = None;

    for ip in (0..instrs.len()).rev() {
        let instr = &instrs[ip];

        if side_effect_type(&instr.op) == SideEffect::Memory {
            if let Some(mem_ip) = last_memory_op {
                g.add_edge(ip, mem_ip, 0);
            }
            last_memory_op = Some(ip);
        }

        uses.for_each_instr_dst_mut(instr, |i, u| {
            if let Some((w_ip, w_dst_idx)) = u.write {
                g.add_edge(
                    ip,
                    w_ip,
                    waw_latency(
                        sm.sm(),
                        &instr.op,
                        i,
                        &instrs[w_ip].op,
                        w_dst_idx,
                    ),
                );
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
                g.add_edge(ip, r_ip, latency);
            }
        });
        uses.for_each_instr_src_mut(instr, |i, u| {
            if let Some((w_ip, w_dst_idx)) = u.write {
                g.add_edge(
                    ip,
                    w_ip,
                    war_latency(
                        sm.sm(),
                        &instr.op,
                        i,
                        &instrs[w_ip].op,
                        w_dst_idx,
                    ),
                );
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

fn calc_statistics(g: &mut DepGraph) -> Vec<usize> {
    let mut initial_ready_list = Vec::new();
    for i in (0..g.nodes.len()).rev() {
        let node = &g.nodes[i];
        let mut max_delay = 0;
        for edge in &node.outgoing_edges {
            assert!(edge.head_idx > i);
            max_delay = max(
                max_delay,
                g.nodes[edge.head_idx].label.cycles_to_end + edge.label,
            );
        }
        let node = &mut g.nodes[i];
        node.label.cycles_to_end = max_delay;
        node.label.num_uses = node.outgoing_edges.len().try_into().unwrap();
        if node.label.num_uses == 0 {
            initial_ready_list.push(i);
        }
    }
    return initial_ready_list;
}

#[derive(PartialEq, Eq, PartialOrd, Ord)]
struct ReadyInstr {
    cycles_to_end: u32,
    index: usize,
}

impl ReadyInstr {
    fn new(g: &DepGraph, i: usize) -> Self {
        ReadyInstr {
            cycles_to_end: g.nodes[i].label.cycles_to_end,
            index: i,
        }
    }
}

#[derive(PartialEq, Eq, PartialOrd, Ord)]
struct FutureReadyInstr {
    ready_cycle: Reverse<u32>,
    index: usize,
}

impl FutureReadyInstr {
    fn new(g: &DepGraph, i: usize) -> Self {
        FutureReadyInstr {
            ready_cycle: Reverse(g.nodes[i].label.ready_cycle),
            index: i,
        }
    }
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
                max(dep_instr.ready_cycle, current_cycle + edge.label);
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

#[allow(dead_code)]
fn save_graphviz(
    instrs: &Vec<Box<Instr>>,
    g: &DepGraph,
) -> std::io::Result<()> {
    // dot /tmp/instr_dep_graph.dot -Tsvg > /tmp/instr_dep_graph.svg

    use std::fs::File;
    use std::io::{BufWriter, Write};

    let file = File::create("/tmp/instr_dep_graph.dot")?;
    let mut w = BufWriter::new(file);

    writeln!(w, "digraph {{")?;
    for (i, instr) in instrs.iter().enumerate() {
        let l = &g.nodes[i].label;
        writeln!(
            w,
            "    {i} [label=\"{}\\n{}, {}\"];",
            instr, l.cycles_to_end, l.num_uses
        )?;
    }
    for (i, node) in g.nodes.iter().enumerate() {
        for j in &node.outgoing_edges {
            writeln!(w, "    {i} -> {} [label=\"{}\"];", j.head_idx, j.label)?;
        }
    }
    writeln!(w, "}}")?;
    w.flush()?;
    Ok(())
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
    pub fn opt_instr_sched(&mut self, sm: &dyn ShaderModel) {
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
    pub fn opt_instr_sched(&mut self) {
        for f in &mut self.functions {
            f.opt_instr_sched(self.sm);
        }
    }
}
