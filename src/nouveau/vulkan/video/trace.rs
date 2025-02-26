// Copyright © 2024 Collabora, Ltd.
// SPDX-License-Identifier: MIT
//! Misc tracing utilities for working with video decoding.

#![allow(dead_code)]

use nvk_video_bindings::_nvdec_h264_pic_s;
use nvk_video_bindings::_nvdec_hevc_pic_s;
use std::io;
use std::io::Write;

pub(crate) fn dump_nvdec_h264_pic_s(
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

pub(crate) fn dump_nvdec_hevc_pic_s(
    pic: &_nvdec_hevc_pic_s,
    file_name: &str,
) -> io::Result<()> {
    let mut log_file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(file_name)?;

    writeln!(log_file, "wrapped_session_key: ")?;
    for &key in &pic.wrapped_session_key {
        write!(log_file, "{:08x} ", key)?;
    }
    writeln!(log_file)?;

    writeln!(log_file, "wrapped_content_key: ")?;
    for &key in &pic.wrapped_content_key {
        write!(log_file, "{:08x} ", key)?;
    }
    writeln!(log_file)?;

    writeln!(log_file, "initialization_vector: ")?;
    for &iv in &pic.initialization_vector {
        write!(log_file, "{:08x} ", iv)?;
    }
    writeln!(log_file)?;

    writeln!(log_file, "stream_len: {}", pic.stream_len)?;
    writeln!(log_file, "enable_encryption: {}", pic.enable_encryption)?;
    writeln!(log_file, "key_increment: {}", pic.key_increment())?;
    writeln!(log_file, "encryption_mode: {}", pic.encryption_mode())?;
    writeln!(log_file, "key_slot_index: {}", pic.key_slot_index())?;
    writeln!(log_file, "ssm_en: {}", pic.ssm_en())?;
    writeln!(log_file, "enable_histogram: {}", pic.enable_histogram())?;
    writeln!(
        log_file,
        "enable_substream_decoding: {}",
        pic.enable_substream_decoding()
    )?;
    writeln!(
        log_file,
        "gptimer_timeout_value: {}",
        pic.gptimer_timeout_value
    )?;
    writeln!(log_file, "tileformat: {}", pic.tileformat())?;
    writeln!(log_file, "gob_height: {}", pic.gob_height())?;
    writeln!(
        log_file,
        "reserverd_surface_format: {}",
        pic.reserverd_surface_format()
    )?;
    writeln!(log_file, "sw_start_code_e: {}", pic.sw_start_code_e)?;
    writeln!(log_file, "disp_output_mode: {}", pic.disp_output_mode)?;
    writeln!(log_file, "reserved1: {}", pic.reserved1)?;
    writeln!(
        log_file,
        "framestride: [{}, {}]",
        pic.framestride[0], pic.framestride[1]
    )?;
    writeln!(log_file, "colMvBuffersize: {}", pic.colMvBuffersize)?;
    writeln!(log_file, "HevcSaoBufferOffset: {}", pic.HevcSaoBufferOffset)?;
    writeln!(log_file, "HevcBsdCtrlOffset: {}", pic.HevcBsdCtrlOffset)?;
    writeln!(
        log_file,
        "pic_width_in_luma_samples: {}",
        pic.pic_width_in_luma_samples
    )?;
    writeln!(
        log_file,
        "pic_height_in_luma_samples: {}",
        pic.pic_height_in_luma_samples
    )?;
    writeln!(log_file, "chroma_format_idc: {}", pic.chroma_format_idc())?;
    writeln!(log_file, "bit_depth_luma: {}", pic.bit_depth_luma())?;
    writeln!(log_file, "bit_depth_chroma: {}", pic.bit_depth_chroma())?;
    writeln!(
        log_file,
        "log2_min_luma_coding_block_size: {}",
        pic.log2_min_luma_coding_block_size()
    )?;
    writeln!(
        log_file,
        "log2_max_luma_coding_block_size: {}",
        pic.log2_max_luma_coding_block_size()
    )?;
    writeln!(
        log_file,
        "log2_min_transform_block_size: {}",
        pic.log2_min_transform_block_size()
    )?;
    writeln!(
        log_file,
        "log2_max_transform_block_size: {}",
        pic.log2_max_transform_block_size()
    )?;
    writeln!(
        log_file,
        "max_transform_hierarchy_depth_inter: {}",
        pic.max_transform_hierarchy_depth_inter()
    )?;
    writeln!(
        log_file,
        "max_transform_hierarchy_depth_intra: {}",
        pic.max_transform_hierarchy_depth_intra()
    )?;
    writeln!(log_file, "scalingListEnable: {}", pic.scalingListEnable())?;
    writeln!(log_file, "amp_enable_flag: {}", pic.amp_enable_flag())?;
    writeln!(
        log_file,
        "sample_adaptive_offset_enabled_flag: {}",
        pic.sample_adaptive_offset_enabled_flag()
    )?;
    writeln!(log_file, "pcm_enabled_flag: {}", pic.pcm_enabled_flag())?;
    writeln!(
        log_file,
        "pcm_sample_bit_depth_luma: {}",
        pic.pcm_sample_bit_depth_luma()
    )?;
    writeln!(
        log_file,
        "pcm_sample_bit_depth_chroma: {}",
        pic.pcm_sample_bit_depth_chroma()
    )?;
    writeln!(
        log_file,
        "log2_min_pcm_luma_coding_block_size: {}",
        pic.log2_min_pcm_luma_coding_block_size()
    )?;
    writeln!(
        log_file,
        "log2_max_pcm_luma_coding_block_size: {}",
        pic.log2_max_pcm_luma_coding_block_size()
    )?;
    writeln!(
        log_file,
        "pcm_loop_filter_disabled_flag: {}",
        pic.pcm_loop_filter_disabled_flag()
    )?;
    writeln!(
        log_file,
        "sps_temporal_mvp_enabled_flag: {}",
        pic.sps_temporal_mvp_enabled_flag()
    )?;
    writeln!(
        log_file,
        "strong_intra_smoothing_enabled_flag: {}",
        pic.strong_intra_smoothing_enabled_flag()
    )?;
    writeln!(
        log_file,
        "dependent_slice_segments_enabled_flag: {}",
        pic.dependent_slice_segments_enabled_flag()
    )?;
    writeln!(
        log_file,
        "output_flag_present_flag: {}",
        pic.output_flag_present_flag()
    )?;
    writeln!(
        log_file,
        "num_extra_slice_header_bits: {}",
        pic.num_extra_slice_header_bits()
    )?;
    writeln!(
        log_file,
        "sign_data_hiding_enabled_flag: {}",
        pic.sign_data_hiding_enabled_flag()
    )?;
    writeln!(
        log_file,
        "cabac_init_present_flag: {}",
        pic.cabac_init_present_flag()
    )?;
    writeln!(
        log_file,
        "num_ref_idx_l0_default_active: {}",
        pic.num_ref_idx_l0_default_active()
    )?;
    writeln!(
        log_file,
        "num_ref_idx_l1_default_active: {}",
        pic.num_ref_idx_l1_default_active()
    )?;
    writeln!(log_file, "init_qp: {}", pic.init_qp())?;
    writeln!(
        log_file,
        "constrained_intra_pred_flag: {}",
        pic.constrained_intra_pred_flag()
    )?;
    writeln!(
        log_file,
        "transform_skip_enabled_flag: {}",
        pic.transform_skip_enabled_flag()
    )?;
    writeln!(
        log_file,
        "cu_qp_delta_enabled_flag: {}",
        pic.cu_qp_delta_enabled_flag()
    )?;
    writeln!(
        log_file,
        "diff_cu_qp_delta_depth: {}",
        pic.diff_cu_qp_delta_depth()
    )?;
    writeln!(log_file, "pps_cb_qp_offset: {}", pic.pps_cb_qp_offset)?;
    writeln!(log_file, "pps_cr_qp_offset: {}", pic.pps_cr_qp_offset)?;
    writeln!(log_file, "pps_beta_offset: {}", pic.pps_beta_offset)?;
    writeln!(log_file, "pps_tc_offset: {}", pic.pps_tc_offset)?;
    writeln!(
        log_file,
        "pps_slice_chroma_qp_offsets_present_flag: {}",
        pic.pps_slice_chroma_qp_offsets_present_flag()
    )?;
    writeln!(log_file, "weighted_pred_flag: {}", pic.weighted_pred_flag())?;
    writeln!(
        log_file,
        "weighted_bipred_flag: {}",
        pic.weighted_bipred_flag()
    )?;
    writeln!(
        log_file,
        "transquant_bypass_enabled_flag: {}",
        pic.transquant_bypass_enabled_flag()
    )?;
    writeln!(log_file, "tiles_enabled_flag: {}", pic.tiles_enabled_flag())?;
    writeln!(
        log_file,
        "entropy_coding_sync_enabled_flag: {}",
        pic.entropy_coding_sync_enabled_flag()
    )?;
    writeln!(log_file, "num_tile_columns: {}", pic.num_tile_columns())?;
    writeln!(log_file, "num_tile_rows: {}", pic.num_tile_rows())?;
    writeln!(
        log_file,
        "loop_filter_across_tiles_enabled_flag: {}",
        pic.loop_filter_across_tiles_enabled_flag()
    )?;
    writeln!(
        log_file,
        "loop_filter_across_slices_enabled_flag: {}",
        pic.loop_filter_across_slices_enabled_flag()
    )?;
    writeln!(
        log_file,
        "deblocking_filter_control_present_flag: {}",
        pic.deblocking_filter_control_present_flag()
    )?;
    writeln!(
        log_file,
        "deblocking_filter_override_enabled_flag: {}",
        pic.deblocking_filter_override_enabled_flag()
    )?;
    writeln!(
        log_file,
        "pps_deblocking_filter_disabled_flag: {}",
        pic.pps_deblocking_filter_disabled_flag()
    )?;
    writeln!(
        log_file,
        "lists_modification_present_flag: {}",
        pic.lists_modification_present_flag()
    )?;
    writeln!(
        log_file,
        "log2_parallel_merge_level: {}",
        pic.log2_parallel_merge_level()
    )?;
    writeln!(
        log_file,
        "slice_segment_header_extension_present_flag: {}",
        pic.slice_segment_header_extension_present_flag()
    )?;
    writeln!(log_file, "num_ref_frames: {}", pic.num_ref_frames)?;
    writeln!(log_file, "reserved6: {}", pic.reserved6)?;
    writeln!(log_file, "longtermflag: {}", pic.longtermflag)?;
    writeln!(log_file, "initreflistidxl0: {:?}", pic.initreflistidxl0)?;
    writeln!(log_file, "initreflistidxl1: {:?}", pic.initreflistidxl1)?;
    writeln!(
        log_file,
        "RefDiffPicOrderCnts: {:?}",
        pic.RefDiffPicOrderCnts
    )?;
    writeln!(log_file, "IDR_picture_flag: {}", pic.IDR_picture_flag)?;
    writeln!(log_file, "RAP_picture_flag: {}", pic.RAP_picture_flag)?;
    writeln!(log_file, "curr_pic_idx: {}", pic.curr_pic_idx)?;
    writeln!(log_file, "pattern_id: {}", pic.pattern_id)?;
    writeln!(log_file, "sw_hdr_skip_length: {}", pic.sw_hdr_skip_length)?;
    writeln!(log_file, "reserved7: {}", pic.reserved7)?;
    writeln!(
        log_file,
        "separate_colour_plane_flag: {}",
        pic.separate_colour_plane_flag()
    )?;
    writeln!(
        log_file,
        "log2_max_pic_order_cnt_lsb_minus4: {}",
        pic.log2_max_pic_order_cnt_lsb_minus4()
    )?;
    writeln!(
        log_file,
        "num_short_term_ref_pic_sets: {}",
        pic.num_short_term_ref_pic_sets()
    )?;
    writeln!(
        log_file,
        "num_long_term_ref_pics_sps: {}",
        pic.num_long_term_ref_pics_sps()
    )?;
    writeln!(log_file, "bBitParsingDisable: {}", pic.bBitParsingDisable())?;
    writeln!(
        log_file,
        "num_delta_pocs_of_rps_idx: {}",
        pic.num_delta_pocs_of_rps_idx()
    )?;
    writeln!(
        log_file,
        "long_term_ref_pics_present_flag: {}",
        pic.long_term_ref_pics_present_flag()
    )?;
    writeln!(log_file, "reserved_dxva: {}", pic.reserved_dxva())?;
    writeln!(
        log_file,
        "num_bits_short_term_ref_pics_in_slice: {}",
        pic.num_bits_short_term_ref_pics_in_slice
    )?;
    writeln!(log_file, "v1: ...",)?;
    writeln!(log_file, "v2: ...",)?;
    writeln!(log_file, "v3: ...",)?;
    writeln!(log_file, "ssm: ...",)?;

    Ok(())
}
