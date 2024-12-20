// Copyright © 2024 Collabora, Ltd.
// SPDX-License-Identifier: MIT
//! Misc tracing utilities for working with video decoding.

#![allow(dead_code)]

use nvk_video_bindings::_nvdec_h264_pic_s;
use std::io;
use std::io::Write;

fn dump_nvdec_h264_pic_s(
    pic: &_nvdec_h264_pic_s,
    file_name: &str,
) -> io::Result<()> {
    let mut log_file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(file_name)?;

    writeln!(log_file, "encryption_params: ...")?;
    write!(log_file, "eos: ")?;
    for &byte in &pic.eos {
        write!(log_file, "{:02x} ", byte)?;
    }
    writeln!(log_file)?;
    writeln!(
        log_file,
        "explicitEOSPresentFlag: {}",
        pic.explicitEOSPresentFlag
    )?;
    writeln!(log_file, "hint_dump_en: {}", pic.hint_dump_en)?;
    writeln!(log_file, "stream_len: {}", pic.stream_len)?;
    writeln!(log_file, "slice_count: {}", pic.slice_count)?;
    writeln!(log_file, "mbhist_buffer_size: {}", pic.mbhist_buffer_size)?;
    writeln!(
        log_file,
        "gptimer_timeout_value: {}",
        pic.gptimer_timeout_value
    )?;
    writeln!(
        log_file,
        "log2_max_pic_order_cnt_lsb_minus4: {}",
        pic.log2_max_pic_order_cnt_lsb_minus4
    )?;
    writeln!(
        log_file,
        "delta_pic_order_always_zero_flag: {}",
        pic.delta_pic_order_always_zero_flag
    )?;
    writeln!(log_file, "frame_mbs_only_flag: {}", pic.frame_mbs_only_flag)?;
    writeln!(log_file, "PicWidthInMbs: {}", pic.PicWidthInMbs)?;
    writeln!(log_file, "FrameHeightInMbs: {}", pic.FrameHeightInMbs)?;
    writeln!(log_file, "tileFormat: {}", pic.tileFormat())?;
    writeln!(log_file, "gob_height: {}", pic.gob_height())?;
    writeln!(
        log_file,
        "reserverd_surface_format: {}",
        pic.reserverd_surface_format()
    )?;
    writeln!(
        log_file,
        "entropy_coding_mode_flag: {}",
        pic.entropy_coding_mode_flag
    )?;
    writeln!(
        log_file,
        "pic_order_present_flag: {}",
        pic.pic_order_present_flag
    )?;
    writeln!(
        log_file,
        "num_ref_idx_l0_active_minus1: {}",
        pic.num_ref_idx_l0_active_minus1
    )?;
    writeln!(
        log_file,
        "num_ref_idx_l1_active_minus1: {}",
        pic.num_ref_idx_l1_active_minus1
    )?;
    writeln!(
        log_file,
        "deblocking_filter_control_present_flag: {}",
        pic.deblocking_filter_control_present_flag
    )?;
    writeln!(
        log_file,
        "redundant_pic_cnt_present_flag: {}",
        pic.redundant_pic_cnt_present_flag
    )?;
    writeln!(
        log_file,
        "transform_8x8_mode_flag: {}",
        pic.transform_8x8_mode_flag
    )?;
    writeln!(log_file, "pitch_luma: {}", pic.pitch_luma)?;
    writeln!(log_file, "pitch_chroma: {}", pic.pitch_chroma)?;
    writeln!(log_file, "luma_top_offset: {}", pic.luma_top_offset)?;
    writeln!(log_file, "luma_bot_offset: {}", pic.luma_bot_offset)?;
    writeln!(log_file, "luma_frame_offset: {}", pic.luma_frame_offset)?;
    writeln!(log_file, "chroma_top_offset: {}", pic.chroma_top_offset)?;
    writeln!(log_file, "chroma_bot_offset: {}", pic.chroma_bot_offset)?;
    writeln!(log_file, "chroma_frame_offset: {}", pic.chroma_frame_offset)?;
    writeln!(log_file, "HistBufferSize: {}", pic.HistBufferSize)?;
    writeln!(log_file, "MbaffFrameFlag: {}", pic.MbaffFrameFlag())?;
    writeln!(
        log_file,
        "direct_8x8_inference_flag: {}",
        pic.direct_8x8_inference_flag()
    )?;
    writeln!(log_file, "weighted_pred_flag: {}", pic.weighted_pred_flag())?;
    writeln!(
        log_file,
        "constrained_intra_pred_flag: {}",
        pic.constrained_intra_pred_flag()
    )?;
    writeln!(log_file, "ref_pic_flag: {}", pic.ref_pic_flag())?;
    writeln!(log_file, "field_pic_flag: {}", pic.field_pic_flag())?;
    writeln!(log_file, "bottom_field_flag: {}", pic.bottom_field_flag())?;
    writeln!(log_file, "second_field: {}", pic.second_field())?;
    writeln!(
        log_file,
        "log2_max_frame_num_minus4: {}",
        pic.log2_max_frame_num_minus4()
    )?;
    writeln!(log_file, "chroma_format_idc: {}", pic.chroma_format_idc())?;
    writeln!(log_file, "pic_order_cnt_type: {}", pic.pic_order_cnt_type())?;
    writeln!(
        log_file,
        "pic_init_qp_minus26: {}",
        pic.pic_init_qp_minus26()
    )?;
    writeln!(
        log_file,
        "chroma_qp_index_offset: {}",
        pic.chroma_qp_index_offset()
    )?;
    writeln!(
        log_file,
        "second_chroma_qp_index_offset: {}",
        pic.second_chroma_qp_index_offset()
    )?;
    writeln!(
        log_file,
        "weighted_bipred_idc: {}",
        pic.weighted_bipred_idc()
    )?;
    writeln!(log_file, "CurrPicIdx: {}", pic.CurrPicIdx())?;
    writeln!(log_file, "CurrColIdx: {}", pic.CurrColIdx())?;
    writeln!(log_file, "frame_num: {}", pic.frame_num())?;
    writeln!(log_file, "frame_surfaces: {}", pic.frame_surfaces())?;
    writeln!(
        log_file,
        "output_memory_layout: {}",
        pic.output_memory_layout()
    )?;
    writeln!(
        log_file,
        "CurrFieldOrderCnt: [{}, {}]",
        pic.CurrFieldOrderCnt[0], pic.CurrFieldOrderCnt[1]
    )?;

    for i in 0..16 {
        writeln!(log_file, "dpb[{}]:", i)?;
        writeln!(log_file, "  dpb[{}].index: {}", i, pic.dpb[i].index())?;
        writeln!(log_file, "  dpb[{}].col_idx: {}", i, pic.dpb[i].col_idx())?;
        writeln!(log_file, "  dpb[{}].state: {}", i, pic.dpb[i].state())?;
        writeln!(
            log_file,
            "  dpb[{}].is_long_term: {}",
            i,
            pic.dpb[i].is_long_term()
        )?;
        writeln!(
            log_file,
            "  dpb[{}].not_existing: {}",
            i,
            pic.dpb[i].not_existing()
        )?;
        writeln!(log_file, "  dpb[{}].is_field: {}", i, pic.dpb[i].is_field())?;
        writeln!(
            log_file,
            "  dpb[{}].top_field_marking: {}",
            i,
            pic.dpb[i].top_field_marking()
        )?;
        writeln!(
            log_file,
            "  dpb[{}].bottom_field_marking: {}",
            i,
            pic.dpb[i].bottom_field_marking()
        )?;
        writeln!(
            log_file,
            "  dpb[{}].output_memory_layout: {}",
            i,
            pic.dpb[i].output_memory_layout()
        )?;
        writeln!(
            log_file,
            "  dpb[{}].FieldOrderCnt: [{}, {}]",
            i, pic.dpb[i].FieldOrderCnt[0], pic.dpb[i].FieldOrderCnt[1]
        )?;
        writeln!(log_file, "  dpb[{}].FrameIdx: {}", i, pic.dpb[i].FrameIdx)?;
    }

    writeln!(log_file, "WeightScale:")?;
    for i in 0..6 {
        for j in 0..4 {
            for k in 0..4 {
                writeln!(
                    log_file,
                    "  WeightScale[{}][{}][{}]: {}",
                    i, j, k, pic.WeightScale[i][j][k]
                )?;
            }
        }
    }

    writeln!(log_file, "WeightScale8x8:")?;
    for i in 0..2 {
        for j in 0..8 {
            for k in 0..8 {
                writeln!(
                    log_file,
                    "  WeightScale8x8[{}][{}][{}]: {}",
                    i, j, k, pic.WeightScale8x8[i][j][k]
                )?;
            }
        }
    }

    writeln!(
        log_file,
        "num_inter_view_refs_lX: [{}, {}]",
        pic.num_inter_view_refs_lX[0], pic.num_inter_view_refs_lX[1]
    )?;

    writeln!(log_file, "inter_view_refidx_lX:")?;
    for i in 0..2 {
        for j in 0..16 {
            writeln!(
                log_file,
                "  inter_view_refidx_lX[{}][{}]: {}",
                i, j, pic.inter_view_refidx_lX[i][j]
            )?;
        }
    }

    writeln!(
        log_file,
        "lossless_ipred8x8_filter_enable: {}",
        pic.lossless_ipred8x8_filter_enable()
    )?;
    writeln!(
        log_file,
        "qpprime_y_zero_transform_bypass_flag: {}",
        pic.qpprime_y_zero_transform_bypass_flag()
    )?;
    writeln!(log_file, "displayPara: ...")?;
    writeln!(log_file, "ssm: ...")?;

    Ok(())
}
