#![allow(non_camel_case_types)]

// This contains the register scheduling information provided by NVIDIA.

// Coupled instructions are ones with fixed latencies, they need delays but not scoreboards.
// Decoupled instructions are ones with variable latencies, need scoreboards but not delays.
// There are also redirected instructions which depending on the SM, can be
// coupled or decoupled so both delays and scoreboards needs to be provided.
//

#[allow(dead_code)]
#[derive(Debug)]
pub enum RegLatencySM75 {
    CoupledDisp64,
    CoupledDisp,
    CoupledAlu,
    CoupledFMA,
    IMADLo,
    IMADWideLower,
    IMADWideUpper,
    RedirectedFP64,
    RedirectedFP16,
    RedirectedHMMA_884_F16,
    RedirectedHMMA_884_F32,
    RedirectedHMMA_1688,
    RedirectedHMMA_16816,
    IMMA,
    Decoupled,
    DecoupledOther,
    BMov,
    GuardPredicate,
}

macro_rules! pred {
    ($has_pred: expr, $b: literal, $p: literal) => {
        if $has_pred {
            $b + $p
        } else {
            $p
        }
    }
}

impl RegLatencySM75 {
    pub fn read_after_write(writer: RegLatencySM75,
                            reader: RegLatencySM75, imma_reader_index: u8) -> u32 {
        match reader {
            RegLatencySM75::CoupledDisp64 |
            RegLatencySM75::CoupledDisp |
            RegLatencySM75::CoupledAlu => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 4,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 5,
                    RegLatencySM75::IMADWideLower => 3,
                    RegLatencySM75::IMADWideUpper => 5,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            },
            RegLatencySM75::CoupledFMA |
            RegLatencySM75::IMADLo => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 5,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 4,
                    RegLatencySM75::IMADWideLower => 2,
                    RegLatencySM75::IMADWideUpper => 4,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            }
            RegLatencySM75::IMADWideLower |
            RegLatencySM75::IMADWideUpper => {
                match imma_reader_index {
                    0 | 1 => {//A, B
                        match writer {
                            RegLatencySM75::CoupledDisp64 => 6,
                            RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 5,
                            RegLatencySM75::CoupledFMA  | RegLatencySM75::IMADLo => 4,
                            RegLatencySM75::IMADWideLower => 4,
                            RegLatencySM75::IMADWideUpper => 4,
                            RegLatencySM75::RedirectedFP64 => 9,
                            RegLatencySM75::RedirectedFP16 => 8,
                            RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                            RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                            RegLatencySM75::RedirectedHMMA_1688 => 14,
                            RegLatencySM75::RedirectedHMMA_16816 => 22,
                            RegLatencySM75::IMMA => 10,
                            _ => 1
                        }
                    },
                    2 => { // C low/high
                        match reader {
                            RegLatencySM75::IMADWideLower => {
                                match writer {
                                        RegLatencySM75::CoupledDisp64 => 6,
                                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 5,
                                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 4,
                                    RegLatencySM75::IMADWideLower => 2,
                                    RegLatencySM75::IMADWideUpper => 2,
                                    RegLatencySM75::RedirectedFP64 => 9,
                                    RegLatencySM75::RedirectedFP16 => 8,
                                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                                    RegLatencySM75::IMMA => 10,
                                    _ => 1
                                }
                            }
                            RegLatencySM75::IMADWideUpper => {
                                match writer {
                                        RegLatencySM75::CoupledDisp64 => 4,
                                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 3,
                                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 2,
                                    RegLatencySM75::IMADWideLower => 2,
                                    RegLatencySM75::IMADWideUpper => 2,
                                    RegLatencySM75::RedirectedFP64 => 7,
                                    RegLatencySM75::RedirectedFP16 => 6,
                                    RegLatencySM75::RedirectedHMMA_884_F16 => 11,
                                    RegLatencySM75::RedirectedHMMA_884_F32 => 8,
                                    RegLatencySM75::RedirectedHMMA_1688 => 12,
                                    RegLatencySM75::RedirectedHMMA_16816 => 20,
                                    RegLatencySM75::IMMA => 8,
                                    _ => 1
                                }
                            }
                            _ => { panic!("Illegal IMAD field"); }
                        }
                    }
                    _ => { panic!("Illegal IMAD field"); }
                }
            }
            RegLatencySM75::RedirectedFP64 => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 6,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 6,
                    RegLatencySM75::IMADWideLower => 6,
                    RegLatencySM75::IMADWideUpper => 6,
                    RegLatencySM75::RedirectedFP64 => 8,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            }
            RegLatencySM75::RedirectedFP16 => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 6,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 6,
                    RegLatencySM75::IMADWideLower => 6,
                    RegLatencySM75::IMADWideUpper => 6,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 6,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            }
            RegLatencySM75::RedirectedHMMA_884_F16 |
            RegLatencySM75::RedirectedHMMA_884_F32 |
            RegLatencySM75::RedirectedHMMA_1688    |
            RegLatencySM75::RedirectedHMMA_16816 |
            RegLatencySM75::Decoupled => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 6,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 6,
                    RegLatencySM75::IMADWideLower => 6,
                    RegLatencySM75::IMADWideUpper => 6,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,//4 for back to back FMA for 884
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,//4 for back o back FMA for 884
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            }
            RegLatencySM75::IMMA |
            RegLatencySM75::DecoupledOther => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 8,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 8,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 8,
                    RegLatencySM75::IMADWideLower => 8,
                    RegLatencySM75::IMADWideUpper => 8,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10, // 4 for back to back IMMA
                    _ => 1
                }
            }
            RegLatencySM75::BMov |
            RegLatencySM75::GuardPredicate => {
                panic!("Not a RAW category")
            }
        }
    }

    pub fn write_after_write(writer1: RegLatencySM75,
                             writer2: RegLatencySM75,
                             has_pred: bool) -> u32 {
        match writer2 {
            RegLatencySM75::CoupledDisp64 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 1,
                    RegLatencySM75::RedirectedFP64 => 4,
                    RegLatencySM75::RedirectedFP16 => 3,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedHMMA_1688 => 9,
                    RegLatencySM75::RedirectedHMMA_16816 => 17,
                    RegLatencySM75::IMMA => 5,
                    _ => 1,
                }
            },
            RegLatencySM75::CoupledDisp |
            RegLatencySM75::CoupledAlu => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 => 2,
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 1,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 1),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 1),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 8, 1),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 5, 1),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 9, 1),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 17, 1),
                    RegLatencySM75::IMMA => pred!(has_pred, 5, 1),
                    _ => 1,
                }
            },
            RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 => 2,
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower => 1,
                    RegLatencySM75::IMADWideUpper => pred!(has_pred, 1, 1),
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 1),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 1),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 8, 1),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 5, 1),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 9, 1),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 17, 1),
                    RegLatencySM75::IMMA => pred!(has_pred, 5, 1),
                    _ => 1,
                }
            }
            RegLatencySM75::IMADWideLower => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 => pred!(has_pred, 2, 2),
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu => pred!(has_pred, 2, 1),
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo => pred!(has_pred, 1, 1),
                    RegLatencySM75::IMADWideLower => 1,
                    RegLatencySM75::IMADWideUpper => 1,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 3),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 3),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 8, 3),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 5, 3),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 9, 3),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 17, 3),
                    RegLatencySM75::IMMA => pred!(has_pred, 5, 3),
                    _ => 1,
                }
            },
            RegLatencySM75::IMADWideUpper => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 => pred!(has_pred, 1, 1),
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 1,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 1),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 1),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 8, 1),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 5, 1),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 9, 1),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 17, 1),
                    RegLatencySM75::IMMA => pred!(has_pred, 5, 1),
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedFP64 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 2,
                    RegLatencySM75::RedirectedFP64 => 1,
                    RegLatencySM75::RedirectedFP16 => 2,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 5,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 2,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::IMMA => 2,
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedFP16 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 2,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 1, 1),
                    RegLatencySM75::RedirectedFP16 => 1,
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 6, 1),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 3, 1),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 7, 1),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 15, 1),
                    RegLatencySM75::IMMA => pred!(has_pred, 3, 1),
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedHMMA_884_F16 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 2,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 3, 2),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedHMMA_884_F16 => 1,
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 2, 4),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 6, 4),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 16, 2),
                    RegLatencySM75::IMMA => pred!(has_pred, 2, 4),
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedHMMA_884_F32 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 2,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 3, 2),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 4, 5),
                    RegLatencySM75::RedirectedHMMA_884_F32 => 1,
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 6, 4),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 16, 2),
                    RegLatencySM75::IMMA => pred!(has_pred, 2, 4),
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedHMMA_1688 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::RedirectedFP64 |
                    RegLatencySM75::RedirectedFP16 => 2,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 4,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 2,
                    RegLatencySM75::RedirectedHMMA_1688 => 1,
                    RegLatencySM75::RedirectedHMMA_16816 => 16,
                    RegLatencySM75::IMMA => 2,
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedHMMA_16816 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::RedirectedFP64 |
                    RegLatencySM75::RedirectedFP16 => 2,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 4,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 2,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 1,
                    RegLatencySM75::IMMA => 2,
                    _ => 1,
                }
            },
            RegLatencySM75::IMMA => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 2, 3),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 2, 7),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 2, 4),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 6, 4),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 14, 4),
                    RegLatencySM75::IMMA => 1,
                    _ => 1,
                }
            },
            RegLatencySM75::Decoupled => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::RedirectedFP64 |
                    RegLatencySM75::RedirectedFP16 |
                    RegLatencySM75::RedirectedHMMA_884_F16 |
                    RegLatencySM75::RedirectedHMMA_884_F32 |
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::IMMA => 2,
                    _ => 1,
                }
            },
            RegLatencySM75::BMov => {// BMOV Writing to RF?
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::RedirectedFP64 |
                    RegLatencySM75::RedirectedFP16 |
                    RegLatencySM75::RedirectedHMMA_884_F16 |
                    RegLatencySM75::RedirectedHMMA_884_F32 |
                    RegLatencySM75::RedirectedHMMA_1688 => 9,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::IMMA => 9,
                    _ => 1,
                }
            },
            RegLatencySM75::DecoupledOther | RegLatencySM75::GuardPredicate => {
                panic!("Not a WAW category")
            }
        }
    }

    pub fn write_after_read(reader: RegLatencySM75,
                            writer: RegLatencySM75) -> u32 {
        match writer {
            RegLatencySM75::CoupledDisp64 |
            RegLatencySM75::CoupledDisp |
            RegLatencySM75::CoupledAlu |
            RegLatencySM75::CoupledFMA |
            RegLatencySM75::IMADLo |
            RegLatencySM75::IMADWideLower |
            RegLatencySM75::IMADWideUpper => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 5,
                    RegLatencySM75::RedirectedHMMA_16816 => 13,
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedFP64 => {
                match reader {
                    RegLatencySM75::RedirectedFP64 => 1,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::RedirectedFP16 => {
                match reader {
                    RegLatencySM75::RedirectedFP16 => 1,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::RedirectedHMMA_884_F16 => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_884_F16 => 1,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::RedirectedHMMA_884_F32 => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_884_F32 => 1,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::RedirectedHMMA_1688 => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 1,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::RedirectedHMMA_16816 => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 1,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::IMMA => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::IMMA => 1,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::Decoupled => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 2,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::BMov => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 9,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 9,
                }
            },
            RegLatencySM75::DecoupledOther | RegLatencySM75::GuardPredicate => {
                panic!("Illegal in WAR");
            }
        }
    }

    pub fn pred_read_after_write(writer: RegLatencySM75,
                                 reader: RegLatencySM75) -> u32 {
        match reader {
            RegLatencySM75::CoupledDisp => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 12,
                    RegLatencySM75::RedirectedFP64 => 15,
                    RegLatencySM75::RedirectedFP16 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::CoupledAlu => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu => 4,
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 5,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::CoupledFMA |
            RegLatencySM75::IMADLo => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu => 5,
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 4,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::IMADWideUpper |
            RegLatencySM75::IMADWideLower => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu => 5,
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo => 4,
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 2,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::RedirectedFP64 => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 12,
                    RegLatencySM75::RedirectedFP64 => 8,
                    RegLatencySM75::RedirectedFP16 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::RedirectedFP16 => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 12,
                    RegLatencySM75::RedirectedFP64 => 15,
                    RegLatencySM75::RedirectedFP16 => 6,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::Decoupled |
            RegLatencySM75::GuardPredicate => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 12,
                    RegLatencySM75::RedirectedFP64 => 15,
                    RegLatencySM75::RedirectedFP16 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            _ => { panic!("Illegal reader in reg predicate"); }
        }
    }

    pub fn pred_write_after_write(writer1: RegLatencySM75,
                                  writer2: RegLatencySM75,
                                  has_pred: bool) -> u32 {
        match writer2 {
            RegLatencySM75::CoupledDisp |
            RegLatencySM75::CoupledAlu |
            RegLatencySM75::CoupledFMA |
            RegLatencySM75::IMADLo => {
                match writer1 {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 1,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 1),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 1),
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::IMADWideUpper |
            RegLatencySM75::IMADWideLower => {
                match writer1 {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu => pred!(has_pred, 1, 2),
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo => pred!(has_pred, 1, 1),
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 1,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 3),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 3),
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::RedirectedFP64 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedFP64 => 1,
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 2, 4),
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::RedirectedFP16 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => pred!(has_pred, 2, 4),
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 2, 7),
                    RegLatencySM75::RedirectedFP16 => 1,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::Decoupled => {
                match writer1 {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::RedirectedFP64 |
                    RegLatencySM75::RedirectedFP16 => 2,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            _ => {
                panic!("Illegal WAR category in Predicates");
            }
        }
    }

    pub fn pred_write_after_read(reader: RegLatencySM75,
                                 writer: RegLatencySM75) -> u32 {
        match writer {
            RegLatencySM75::CoupledDisp |
            RegLatencySM75::CoupledAlu |
            RegLatencySM75::CoupledFMA |
            RegLatencySM75::IMADLo |
            RegLatencySM75::IMADWideUpper |
            RegLatencySM75::IMADWideLower => { 1 },
            RegLatencySM75::RedirectedFP64 => {
                match reader {
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::RedirectedFP16 => 2,
                    _ => 1,
                }
            }
            RegLatencySM75::RedirectedFP16 => {
                match reader {
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::RedirectedFP64 => 2,
                    _ => 1,
                }
            }
            RegLatencySM75::Decoupled => {
                match reader {
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::RedirectedFP16 |
                    RegLatencySM75::RedirectedFP64 => 2,
                    _ => 1,
                }
            }
            _ => {
                panic!("Illegal WAR category in Predicates");
            }
        }
    }
}

#[allow(non_camel_case_types)]
#[allow(dead_code)]
#[derive(Debug)]
pub enum URegLatencySM75 {
    Udp,
    VectorCoupled,
    VectorDecoupled,
    Uldc,
    Umov,
    VectorCoupledBindless,
    VectorDecoupledBindless,
    VoteU,
    GuardPredicate,
    R2UR,
}

impl URegLatencySM75 {
    pub fn read_after_write(writer: URegLatencySM75,
                            reader: URegLatencySM75) -> u32 {
        match reader {
            URegLatencySM75::Udp => {
                match writer {
                    URegLatencySM75::Udp => 4,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 2,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM75::VectorCoupled => {
                match writer {
                    URegLatencySM75::Udp => 6,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 2,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM75::VectorDecoupled => {
                match writer {
                    URegLatencySM75::Udp => 9,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 2,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM75::Uldc |
            URegLatencySM75::VectorCoupledBindless |
            URegLatencySM75::VectorDecoupledBindless => {
                match writer {
                    URegLatencySM75::Udp => 12,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 5,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM75::Umov => {
                match writer {
                    URegLatencySM75::Udp => 7,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 2,
                    _ => { panic!("Illegal writer in raw ureg latency") },
                }
            }
            _ => { panic!("Illegal read in ureg raw latency") },
        }
    }

    pub fn write_after_write(writer1: URegLatencySM75,
                             writer2: URegLatencySM75,
                             has_pred: bool) -> u32 {
        match writer2 {
            URegLatencySM75::Udp => {
                match writer1 {
                    URegLatencySM75::Udp => 1,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 1,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            },
            URegLatencySM75::R2UR => {
                match writer1 {
                    URegLatencySM75::Udp => pred!(has_pred, 4, 6),
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 4,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            },
            URegLatencySM75::Uldc |
            URegLatencySM75::VoteU |
            URegLatencySM75::Umov => {
                match writer1 {
                    URegLatencySM75::Udp => 7,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 1,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            }
            _ => { panic!("Illegal writer in ureg waw latency") },
        }
    }

    pub fn write_after_read(reader: URegLatencySM75,
                            writer: URegLatencySM75) -> u32 {
        match writer {
            URegLatencySM75::Udp => 1,
            URegLatencySM75::R2UR => 1,
            URegLatencySM75::Uldc |
            URegLatencySM75::VoteU |
            URegLatencySM75::Umov => {
                match reader {
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 3,
                    _ => 1,
                }
            }
            _ => { panic!("Illegal writer in ureg war latency") }
        }
    }

    pub fn pred_read_after_write(writer: URegLatencySM75,
                                 reader: URegLatencySM75) -> u32 {
        match reader {
            URegLatencySM75::Udp => {
                match writer {
                    URegLatencySM75::Udp => 4,
                    URegLatencySM75::VoteU => 1,
                    _ => { panic!("Illegal writer in upred raw latency") }
                }
            }
            URegLatencySM75::VectorCoupled => {
                match writer {
                    URegLatencySM75::Udp => 6,
                    URegLatencySM75::VoteU => 1,
                    _ => { panic!("Illegal writer in upred raw latency") }
                }
            }
            URegLatencySM75::GuardPredicate => {
                match writer {
                    URegLatencySM75::Udp => 11,
                    URegLatencySM75::VoteU => 5,
                    _ => { panic!("Illegal writer in upred raw latency") }
                }
            }
            _ => { panic!("Illegal reader in upred raw latency") }
        }
    }

    pub fn pred_write_after_write(writer1: URegLatencySM75,
                                  writer2: URegLatencySM75) -> u32 {
        match writer2 {
            URegLatencySM75::Udp => 1,
            URegLatencySM75::VoteU => {
                match writer1 {
                    URegLatencySM75::Udp => 7,
                    URegLatencySM75::VoteU => 1,
                    _ => { panic!("Illegal writer1 in upred raw latency") }
                }
            }
            _ => { panic!("Illegal writer2 in upred raw latency") }
        }
    }

    pub fn pred_write_after_read(reader: URegLatencySM75,
                                 writer: URegLatencySM75) -> u32 {
        match writer {
            URegLatencySM75::Udp => 1,
            URegLatencySM75::VoteU => {
                match reader {
                    URegLatencySM75::Udp => 2,
                    _ => 1,
                }
            }
            _ => { panic!("Illegal writer2 in upred raw latency") }
        }
    }
}
