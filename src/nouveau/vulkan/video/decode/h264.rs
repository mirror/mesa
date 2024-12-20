// Copyright © 2024 Collabora, Ltd and Red Hat, Inc.
// SPDX-License-Identifier: MIT

//! H264 decode implementation. Takes inspiration from the early C version
//! written by Dave Airlie.

use std::collections::HashMap;
use std::collections::HashSet;

use nv_push_rs::*;
use nvidia_headers::classes::clc5b0::mthd as clc5b0;
use nvk_video_bindings::_nvdec_h264_pic_s;
use nvk_video_bindings::nvk_cmd_buffer;
use nvk_video_bindings::nvk_image;
use nvk_video_bindings::nvk_image_view;
use nvk_video_bindings::StdVideoDecodeH264PictureInfo;
use nvk_video_bindings::StdVideoH264SequenceParameterSet;
use nvk_video_bindings::VkVideoBeginCodingInfoKHR;
use nvk_video_bindings::VkVideoReferenceSlotInfoKHR;
use nvk_video_bindings::VK_SUCCESS;

use crate::align_u32;
use crate::append_rust_push;
use crate::decode::VideoDecoder;
use crate::use_video_engine;
use crate::vk_find_struct_const;

/// The type of picture being decoded.
#[derive(Debug, Default, Clone, Copy)]
enum PictureType {
    /// Top field has been decoded.
    Top = 1,
    /// Bottom field has been decoded.
    Bottom = 2,
    /// A frame, i.e.: either progressive content or both fields have
    /// been decoded.
    #[default]
    Frame = 3,
}

#[derive(Debug, Default)]
struct FrameData {
    /// The `pic_idx` value associated with this frame.
    pic_idx: Option<u32>,
    /// The `dpb_idx` value associated with this frame.
    dpb_idx: Option<u32>,
    /// Is this the first field or a complementary field for a given picture?
    first_field_or_complementary: bool,
    /// The type of picture being decoded. This keeps track of what we have seen
    /// so far.
    picture_ty: PictureType,
}

fn compute_opaque_buffer_sizes(
    sps: &StdVideoH264SequenceParameterSet,
) -> (u32, u32, u32) {
    let pic_height_in_map_units = sps.pic_height_in_map_units_minus1 + 1;
    let pic_width_in_mbs = sps.pic_width_in_mbs_minus1 + 1;
    let max_num_ref_frames = sps.max_num_ref_frames + 1;

    let mut coloc_size = align_u32(
        align_u32(pic_height_in_map_units, 2) * pic_width_in_mbs * 64 - 63,
        0x100,
    );
    coloc_size *= u32::from(max_num_ref_frames);

    let mbhist_size = align_u32(pic_width_in_mbs * 104, 0x100);
    let history_size = align_u32(pic_width_in_mbs * 0x300, 0x200);

    (coloc_size, mbhist_size, history_size)
}

fn get_opaque_mem_addrs(nvk_cmd: *mut nvk_cmd_buffer) -> (u64, u64, u64) {
    unsafe {
        let vid = (*nvk_cmd).video.vid;

        let mem0 = (*(*vid).mems[0].mem).mem;
        let mem0_addr = (*(*mem0).va).addr + (*vid).mems[0].offset;

        let mem1 = (*(*vid).mems[1].mem).mem;
        let mem1_addr = (*(*mem1).va).addr + (*vid).mems[1].offset;

        let mem2 = (*(*vid).mems[2].mem).mem;
        let mem2_addr = (*(*mem2).va).addr + (*vid).mems[2].offset;

        (mem0_addr >> 8, mem1_addr >> 8, mem2_addr >> 8)
    }
}

static EOS_ARRAY: [u8; 16] = [
    0x0, 0x0, 0x1, 0xb, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0xb, 0x0, 0x0, 0x0,
    0x0,
];

fn to_gob_value(y_log2: u8) -> u32 {
    match 1 << y_log2 {
        2 => 0,
        4 => 1,
        8 => 2,
        16 => 3,
        32 => 4,
        other => panic!("unsupported GOB value: {other}"),
    }
}

#[derive(Debug, Clone, Copy)]
struct GpuBufferAddresses {
    pic: u64,
    slice_offsets: u64,
    mbstatus: u64,
}

/// Upload the parameters to the GPU.
fn upload_to_the_gpu(
    nvk_cmd: *mut nvk_cmd_buffer,
    nvh264: _nvdec_h264_pic_s,
    slice_offsets: [u32; 256],
) -> GpuBufferAddresses {
    let mut nvh264_ptr = std::ptr::null_mut();
    let mut pic_gpu_addr = 0;
    unsafe {
        let res = nvk_video_bindings::nvk_cmd_buffer_upload_alloc(
            nvk_cmd,
            std::mem::size_of::<nvk_video_bindings::nvdec_h264_pic_s>()
                .try_into()
                .unwrap(),
            256,
            &mut pic_gpu_addr,
            &mut nvh264_ptr as *mut *mut _ as *mut *mut std::ffi::c_void,
        );

        assert!(res == VK_SUCCESS);
        std::ptr::copy_nonoverlapping(&nvh264, nvh264_ptr, 1);
    }

    let mut slice_offsets_ptr = std::ptr::null_mut();
    let mut slice_offsets_address = 0;

    unsafe {
        let res = nvk_video_bindings::nvk_cmd_buffer_upload_alloc(
            nvk_cmd,
            std::mem::size_of_val(&slice_offsets).try_into().unwrap(),
            256,
            &mut slice_offsets_address,
            &mut slice_offsets_ptr as *mut *mut _ as *mut *mut std::ffi::c_void,
        );

        assert!(res == VK_SUCCESS);
        std::ptr::copy_nonoverlapping(
            slice_offsets.as_ptr(),
            slice_offsets_ptr,
            slice_offsets.len(),
        );
    }

    // Just upload this to the GPU for now, we will hook it up later.
    let mut mbstatus = std::ptr::null_mut();
    let mut mbstatus_address = 0;
    unsafe {
        let res = nvk_video_bindings::nvk_cmd_buffer_upload_alloc(
            nvk_cmd,
            4096,
            256,
            &mut mbstatus_address,
            &mut mbstatus as *mut *mut _ as *mut *mut std::ffi::c_void,
        );

        assert!(res == VK_SUCCESS);
    }

    GpuBufferAddresses {
        pic: pic_gpu_addr,
        slice_offsets: slice_offsets_address,
        mbstatus: mbstatus_address,
    }
}

/// Session data stored in opaque `nvk_video_session::rust` pointer.
#[derive(Default, Debug)]
pub(crate) struct Decoder {
    /// Data associated with each image view.
    slots: HashMap<*const nvk_image_view, FrameData>,
    /// A counter for the frame number. Note that the hardware wants a u32.
    frame_num: u32,
    /// The free picture slots.
    free_pic_slots: HashSet<u32>,
    /// The free DPB slots.
    free_dpb_slots: HashSet<u32>,
}

impl Decoder {
    /// Gets the ith slot from the `begin_info`. These slots are the ones the
    /// application plans to use during the `vkCmdBeginVideoCodingKHR` and
    /// `vkCmdEndVideoCodingKHR` calls.
    fn get_ith_planned_slot(
        begin_info: &VkVideoBeginCodingInfoKHR,
        i: usize,
    ) -> (VkVideoReferenceSlotInfoKHR, *const nvk_image_view) {
        if i >= begin_info.referenceSlotCount as usize {
            panic!("Invalid reference slot index {i}");
        }

        let ref_slot = unsafe { *begin_info.pReferenceSlots.add(i as usize) };
        let f_dpb_iv = unsafe { *ref_slot.pPictureResource }.imageViewBinding;

        let iv =
            unsafe { nvk_video_bindings::nvk_image_view_from_handle(f_dpb_iv) };

        (ref_slot, iv)
    }

    /// Gets the ith slot for the frame currently being decoded. This is a slot
    /// that is referenced by the current frame.
    fn get_ith_slot_for_frame(
        frame_info: &nvk_video_bindings::VkVideoDecodeInfoKHR,
        i: usize,
    ) -> (VkVideoReferenceSlotInfoKHR, *const nvk_image_view) {
        if i >= frame_info.referenceSlotCount as usize {
            panic!("Invalid reference slot index {i}");
        }

        let ref_slot = unsafe { *frame_info.pReferenceSlots.add(i) };
        let f_dpb_iv = unsafe { *ref_slot.pPictureResource }.imageViewBinding;

        let iv =
            unsafe { nvk_video_bindings::nvk_image_view_from_handle(f_dpb_iv) };

        (ref_slot, iv)
    }

    fn remove_invalid_slots(&mut self, begin_info: &VkVideoBeginCodingInfoKHR) {
        let mut entries_to_remove = Vec::new();

        for &key in self.slots.keys() {
            let mut found = false;

            for i in 0..begin_info.referenceSlotCount {
                let (ref_slot, f_dpb_iv) =
                    Self::get_ith_planned_slot(begin_info, i as usize);

                if key == f_dpb_iv && ref_slot.slotIndex >= 0 {
                    found = true;
                    break;
                }
            }

            if !found {
                entries_to_remove.push(key);
            }
        }

        for key in entries_to_remove {
            self.slots.remove(&key).unwrap();
        }

        // i.e.: everything is free.
        self.free_pic_slots = (0..=16).collect();
        self.free_dpb_slots = (0..=16).collect();
        for frame_slot in self.slots.values() {
            if let Some(pic_idx) = frame_slot.pic_idx {
                self.free_pic_slots.remove(&pic_idx);
            }
            if let Some(dpb_idx) = frame_slot.dpb_idx {
                self.free_dpb_slots.remove(&dpb_idx);
            }
        }
    }

    /// Forcibly find a frame. If the frame has not been submitted, it's an
    /// application error.
    fn find_submitted_frame<'a>(
        slots: &'a mut HashMap<*const nvk_image_view, FrameData>,
        iv: *const nvk_image_view,
    ) -> &'a mut FrameData {
        slots.get_mut(&iv).expect(
        "Frame data not found. Either this picture was not submitted or invalidated.",
    )
    }

    /// Get the `pic_idx` value associated with the given image view. If this is
    /// the first time we are seeing this image view, then allocate a new
    /// `pic_idx` value.
    fn get_pic_idx(&mut self, iv: *const nvk_image_view) -> u32 {
        if let Some(frame_data) = self.slots.get(&iv) {
            if let Some(pic_idx) = frame_data.pic_idx {
                return pic_idx;
            }
        }

        let pic_idx = *self
            .free_pic_slots
            .iter()
            .min()
            .expect("Bad DPB management");

        self.free_pic_slots.remove(&pic_idx);

        let frame_data = FrameData {
            pic_idx: Some(pic_idx),
            ..Default::default()
        };

        self.slots.insert(iv, frame_data);

        pic_idx
    }

    /// Get the `dpb_idx` value associated with the given image view or assign
    /// one if needed. This image view *must* have been submitted already.
    fn get_dpb_idx(&mut self, iv: *const nvk_image_view) -> u32 {
        let frame_data = Self::find_submitted_frame(&mut self.slots, iv);

        if let Some(dpb_idx) = frame_data.dpb_idx {
            return dpb_idx;
        } else {
            let dpb_idx = *self
                .free_dpb_slots
                .iter()
                .min()
                .expect("Bad DPB management");

            self.free_dpb_slots.remove(&dpb_idx);

            frame_data.dpb_idx = Some(dpb_idx);

            dpb_idx
        }
    }

    fn is_field(&mut self, iv: *const nvk_image_view) -> bool {
        Self::find_submitted_frame(&mut self.slots, iv)
            .first_field_or_complementary
    }

    fn set_field(&mut self, iv: *const nvk_image_view, is_field: bool) {
        Self::find_submitted_frame(&mut self.slots, iv)
            .first_field_or_complementary = is_field;
    }

    fn get_picture_type(&mut self, iv: *const nvk_image_view) -> PictureType {
        Self::find_submitted_frame(&mut self.slots, iv).picture_ty
    }

    fn set_picture_type(
        &mut self,
        iv: *const nvk_image_view,
        picture_ty: PictureType,
    ) {
        Self::find_submitted_frame(&mut self.slots, iv).picture_ty = picture_ty;
    }

    fn set_reference_frames(
        &mut self,
        nvh264: &mut nvk_video_bindings::nvdec_h264_pic_s,
        frame_info: &nvk_video_bindings::VkVideoDecodeInfoKHR,
        (luma_base, chroma_base): (&mut [u32; 17], &mut [u32; 17]),
    ) {
        for i in 0..frame_info.referenceSlotCount as usize {
            let (vk_ref_slot, iv) =
                Decoder::get_ith_slot_for_frame(frame_info, i);

            let img = unsafe { (*iv).vk.image as *mut nvk_image };

            let dpb_slot = vk_find_struct_const!(
                vk_ref_slot.pNext,
                VIDEO_DECODE_H264_DPB_SLOT_INFO,
                KHR
            );

            let vk_ref_info = unsafe { *dpb_slot.pStdReferenceInfo };

            let dpb_idx = self.get_dpb_idx(iv);
            let pic_idx = self.get_pic_idx(iv);

            let is_field = self.is_field(iv);
            let picture_ty = self.get_picture_type(iv);

            let marking =
                if vk_ref_info.flags.used_for_long_term_reference() != 0 {
                    2
                } else {
                    1
                };

            let top_field_marking = match picture_ty {
                PictureType::Top | PictureType::Frame => marking,
                _ => 0,
            };

            let bottom_field_marking = match picture_ty {
                PictureType::Bottom | PictureType::Frame => marking,
                _ => 0,
            };

            let dpb_entry = &mut nvh264.dpb[dpb_idx as usize];

            dpb_entry.set_index(pic_idx);
            dpb_entry.set_col_idx(pic_idx);
            dpb_entry.set_is_field(is_field as u32);
            dpb_entry.set_state(picture_ty as u32);
            dpb_entry.set_top_field_marking(top_field_marking);
            dpb_entry.set_bottom_field_marking(bottom_field_marking);

            dpb_entry.FieldOrderCnt[0] =
                if vk_ref_info.PicOrderCnt[0] != i32::MAX {
                    vk_ref_info.PicOrderCnt[0].try_into().unwrap()
                } else {
                    vk_ref_info.PicOrderCnt[1].try_into().unwrap()
                };

            dpb_entry.FieldOrderCnt[1] =
                if vk_ref_info.PicOrderCnt[1] != i32::MAX {
                    vk_ref_info.PicOrderCnt[1].try_into().unwrap()
                } else {
                    vk_ref_info.PicOrderCnt[0].try_into().unwrap()
                };

            dpb_entry.FrameIdx = vk_ref_info.FrameNum.try_into().unwrap();

            dpb_entry.set_is_long_term(
                vk_ref_info.flags.used_for_long_term_reference(),
            );

            dpb_entry.set_not_existing(vk_ref_info.flags.is_non_existing());

            luma_base[pic_idx as usize] = unsafe {
                nvk_video_bindings::nvk_image_base_address(img, 0) >> 8
            }
            .try_into()
            .unwrap();

            chroma_base[pic_idx as usize] = unsafe {
                nvk_video_bindings::nvk_image_base_address(img, 1) >> 8
            }
            .try_into()
            .unwrap();
        }
    }

    fn set_current_picture_slot(
        &mut self,
        iv: *const nvk_image_view,
        std_pic_info: &StdVideoDecodeH264PictureInfo,
        interlaced: bool,
    ) -> u32 {
        if !interlaced {
            assert!(
            !self.slots.contains_key(&iv),
            "This slot is in use, the application should have invalidated it"
        );
        }

        let pic_idx = self.get_pic_idx(iv);

        let is_field_pic = std_pic_info.flags.field_pic_flag() != 0;
        let is_complementary_field_pair =
            std_pic_info.flags.complementary_field_pair() != 0;
        let is_bottom_field = std_pic_info.flags.bottom_field_flag() != 0;

        self.set_field(iv, is_field_pic || is_complementary_field_pair);

        if is_field_pic {
            if is_complementary_field_pair {
                self.set_picture_type(iv, PictureType::Frame);
            } else if is_bottom_field {
                self.set_picture_type(iv, PictureType::Bottom);
            } else {
                self.set_picture_type(iv, PictureType::Top);
            }
        } else {
            self.set_picture_type(iv, PictureType::Frame);
        }

        pic_idx
    }
}

impl VideoDecoder for Decoder {
    fn begin(
        &mut self,
        nvk_cmd: *mut nvk_cmd_buffer,
        begin_info: &nvk_video_bindings::VkVideoBeginCodingInfoKHR,
    ) {
        self.remove_invalid_slots(&begin_info);

        let mut push = Push::new();

        use_video_engine(&mut push);
        append_rust_push(push, nvk_cmd);
    }

    fn decode(
        &mut self,
        nvk_cmd: *mut nvk_cmd_buffer,
        frame_info: &nvk_video_bindings::VkVideoDecodeInfoKHR,
    ) {
        let h264_pic_info = vk_find_struct_const!(
            frame_info.pNext,
            VIDEO_DECODE_H264_PICTURE_INFO,
            KHR
        );

        let std_pic_info = unsafe { *h264_pic_info.pStdPictureInfo };

        let sps = unsafe {
            *nvk_video_bindings::vk_video_find_h264_dec_std_sps(
                (*nvk_cmd).video.params as *const _,
                std_pic_info.seq_parameter_set_id.into(),
            )
        };

        let pps = unsafe {
            *nvk_video_bindings::vk_video_find_h264_dec_std_pps(
                (*nvk_cmd).video.params as *const _,
                std_pic_info.pic_parameter_set_id.into(),
            )
        };

        // I do not know why the size of the coloc buffer is not passed to the hardware.
        let (_coloc_size, mbhist_size, history_size) =
            compute_opaque_buffer_sizes(&sps);

        let dst_iv = unsafe {
            nvk_video_bindings::nvk_image_view_from_handle(
                frame_info.dstPictureResource.imageViewBinding,
            )
        };
        let dst_img_ptr =
            unsafe { *dst_iv }.vk.image as *mut nvk_video_bindings::nvk_image;
        let dst_img = unsafe { &mut *dst_img_ptr };

        let mut nvh264 = _nvdec_h264_pic_s::default();

        nvh264.explicitEOSPresentFlag = 1;
        nvh264.eos = EOS_ARRAY;

        nvh264.slice_count = h264_pic_info.sliceCount.into();
        nvh264.stream_len = u32::try_from(frame_info.srcBufferRange).unwrap()
            + std::mem::size_of_val(&EOS_ARRAY) as u32;

        nvh264.mbhist_buffer_size = mbhist_size;
        nvh264.log2_max_pic_order_cnt_lsb_minus4 =
            sps.log2_max_pic_order_cnt_lsb_minus4.into();
        nvh264.delta_pic_order_always_zero_flag =
            sps.flags.delta_pic_order_always_zero_flag() as i32;
        nvh264.frame_mbs_only_flag =
            sps.flags.frame_mbs_only_flag().try_into().unwrap();
        nvh264.PicWidthInMbs = sps.pic_width_in_mbs_minus1 + 1;

        nvh264.FrameHeightInMbs = sps.pic_height_in_map_units_minus1 + 1;
        if nvh264.frame_mbs_only_flag == 0 {
            nvh264.FrameHeightInMbs *= 2;
        }

        nvh264.set_tileFormat(1);

        let y_log2 = dst_img.planes[0].nil.levels[0].tiling.y_log2;
        nvh264.set_gob_height(to_gob_value(y_log2));

        nvh264.entropy_coding_mode_flag =
            pps.flags.entropy_coding_mode_flag() as _;
        nvh264.pic_order_present_flag =
            pps.flags.bottom_field_pic_order_in_frame_present_flag() as _;
        nvh264.num_ref_idx_l0_active_minus1 =
            pps.num_ref_idx_l0_default_active_minus1.into();
        nvh264.num_ref_idx_l1_active_minus1 =
            pps.num_ref_idx_l1_default_active_minus1.into();
        nvh264.deblocking_filter_control_present_flag =
            pps.flags.deblocking_filter_control_present_flag() as _;
        nvh264.redundant_pic_cnt_present_flag =
            pps.flags.redundant_pic_cnt_present_flag() as _;
        nvh264.transform_8x8_mode_flag =
            pps.flags.transform_8x8_mode_flag() as _;
        nvh264.pitch_luma = dst_img.planes[0].nil.levels[0].row_stride_B;
        nvh264.pitch_chroma = dst_img.planes[1].nil.levels[0].row_stride_B;
        nvh264.luma_bot_offset = nvh264.PicWidthInMbs * 16;
        nvh264.chroma_bot_offset = nvh264.pitch_chroma / 2;

        nvh264.HistBufferSize = history_size >> 8;

        let is_field = std_pic_info.flags.field_pic_flag() != 0;
        let mbaff_frame_flag = sps.flags.mb_adaptive_frame_field_flag() != 0;
        let mbaff_frame_flag = mbaff_frame_flag && !is_field;
        nvh264.set_MbaffFrameFlag(mbaff_frame_flag.into());

        nvh264.set_direct_8x8_inference_flag(
            sps.flags.direct_8x8_inference_flag() as _,
        );
        nvh264.set_weighted_pred_flag(pps.flags.weighted_pred_flag() as _);
        nvh264.set_constrained_intra_pred_flag(
            pps.flags.constrained_intra_pred_flag() as _,
        );
        nvh264.set_ref_pic_flag(std_pic_info.flags.is_reference() as _);
        nvh264.set_field_pic_flag(std_pic_info.flags.field_pic_flag() as _);
        nvh264
            .set_bottom_field_flag(std_pic_info.flags.bottom_field_flag() as _);
        nvh264.set_second_field(
            std_pic_info.flags.complementary_field_pair() as _
        );
        nvh264.set_log2_max_frame_num_minus4(
            sps.log2_max_frame_num_minus4.into(),
        );
        nvh264.set_chroma_format_idc(sps.chroma_format_idc);
        nvh264.set_pic_order_cnt_type(sps.pic_order_cnt_type);
        nvh264.set_pic_init_qp_minus26(pps.pic_init_qp_minus26.into());
        nvh264.set_chroma_qp_index_offset(pps.chroma_qp_index_offset.into());
        nvh264.set_second_chroma_qp_index_offset(
            pps.second_chroma_qp_index_offset.into(),
        );

        nvh264.set_weighted_bipred_idc(pps.weighted_bipred_idc);
        nvh264.set_frame_num(std_pic_info.frame_num.into());

        nvh264.CurrFieldOrderCnt[0] = if std_pic_info.PicOrderCnt[0] != i32::MAX
        {
            std_pic_info.PicOrderCnt[0]
        } else {
            std_pic_info.PicOrderCnt[1]
        };

        nvh264.CurrFieldOrderCnt[1] = if std_pic_info.PicOrderCnt[1] != i32::MAX
        {
            std_pic_info.PicOrderCnt[1]
        } else {
            std_pic_info.PicOrderCnt[0]
        };

        nvh264.WeightScale = [[[0x10; 4]; 4]; 6];
        nvh264.WeightScale8x8 = [[[0x10; 8]; 8]; 2];

        let mut slice_offsets = [0; 256];
        for i in 0..(h264_pic_info.sliceCount + 1) as usize {
            slice_offsets[i] = unsafe { *h264_pic_info.pSliceOffsets.add(i) };
        }

        let mut luma_base = [0; 17];
        let mut chroma_base = [0; 17];

        self.set_reference_frames(
            &mut nvh264,
            &frame_info,
            (&mut luma_base, &mut chroma_base),
        );

        let cur_pic_idx = self.set_current_picture_slot(
            dst_iv,
            &std_pic_info,
            nvh264.frame_mbs_only_flag == 0,
        );
        nvh264.set_CurrPicIdx(cur_pic_idx);
        nvh264.set_CurrColIdx(cur_pic_idx);

        nvh264.set_lossless_ipred8x8_filter_enable(0);
        nvh264.set_qpprime_y_zero_transform_bypass_flag(
            sps.flags.qpprime_y_zero_transform_bypass_flag(),
        );

        luma_base[cur_pic_idx as usize] = unsafe {
            nvk_video_bindings::nvk_image_base_address(dst_img, 0) >> 8
        }
        .try_into()
        .unwrap();
        chroma_base[cur_pic_idx as usize] = unsafe {
            nvk_video_bindings::nvk_image_base_address(dst_img, 1) >> 8
        }
        .try_into()
        .unwrap();

        let (mem0_addr, mem1_addr, mem2_addr) = get_opaque_mem_addrs(nvk_cmd);

        let src_buffer = unsafe {
            nvk_video_bindings::nvk_buffer_from_handle(frame_info.srcBuffer)
        };
        let src_address = unsafe {
            nvk_video_bindings::nvk_buffer_address(
                src_buffer,
                frame_info.srcBufferOffset,
            )
        };

        let GpuBufferAddresses {
            pic: pic_gpu_address,
            slice_offsets: slice_offsets_address,
            mbstatus: mbstatus_address,
        } = upload_to_the_gpu(nvk_cmd, nvh264, slice_offsets);

        let mut push = Push::new();

        push.push_method(clc5b0::SetApplicationId {
            id: clc5b0::SetApplicationIdId::H264,
        });

        push.push_method(clc5b0::SetControlParams {
            codec_type: clc5b0::SetControlParamsCodecType::H264,
            gptimer_on: 1,
            err_conceal_on: 1,
            mbtimer_on: 1,
            error_frm_idx: self.frame_num % u32::from(sps.max_num_ref_frames),
            ret_error: 0,
            ec_intra_frame_using_pslc: 0,
            all_intra_frame: 0,
            reserved: Default::default(),
        });

        push.push_method(clc5b0::SetDrvPicSetupOffset {
            offset: (pic_gpu_address >> 8).try_into().unwrap(),
        });

        push.push_method(clc5b0::SetInBufBaseOffset {
            offset: (src_address >> 8).try_into().unwrap(),
        });

        push.push_method(clc5b0::SetPictureIndex {
            index: self.frame_num,
        });

        push.push_method(clc5b0::SetSliceOffsetsBufOffset {
            offset: (slice_offsets_address >> 8).try_into().unwrap(),
        });

        push.push_method(clc5b0::SetColocDataOffset {
            offset: mem0_addr.try_into().unwrap(),
        });

        push.push_method(clc5b0::SetHistoryOffset {
            offset: mem2_addr.try_into().unwrap(),
        });

        push.push_method(clc5b0::SetNvdecStatusOffset {
            offset: (mbstatus_address >> 8).try_into().unwrap(),
        });

        push.push_method(clc5b0::SetPictureLumaOffset0 {
            offset: luma_base[0].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset1 {
            offset: luma_base[1].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset2 {
            offset: luma_base[2].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset3 {
            offset: luma_base[3].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset4 {
            offset: luma_base[4].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset5 {
            offset: luma_base[5].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset6 {
            offset: luma_base[6].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset7 {
            offset: luma_base[7].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset8 {
            offset: luma_base[8].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset9 {
            offset: luma_base[9].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset10 {
            offset: luma_base[10].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset11 {
            offset: luma_base[11].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset12 {
            offset: luma_base[12].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset13 {
            offset: luma_base[13].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset14 {
            offset: luma_base[14].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset15 {
            offset: luma_base[15].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureLumaOffset16 {
            offset: luma_base[16].try_into().unwrap(),
        });

        push.push_method(clc5b0::SetPictureChromaOffset0 {
            offset: chroma_base[0].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset1 {
            offset: chroma_base[1].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset2 {
            offset: chroma_base[2].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset3 {
            offset: chroma_base[3].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset4 {
            offset: chroma_base[4].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset5 {
            offset: chroma_base[5].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset6 {
            offset: chroma_base[6].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset7 {
            offset: chroma_base[7].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset8 {
            offset: chroma_base[8].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset9 {
            offset: chroma_base[9].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset10 {
            offset: chroma_base[10].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset11 {
            offset: chroma_base[11].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset12 {
            offset: chroma_base[12].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset13 {
            offset: chroma_base[13].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset14 {
            offset: chroma_base[14].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset15 {
            offset: chroma_base[15].try_into().unwrap(),
        });
        push.push_method(clc5b0::SetPictureChromaOffset16 {
            offset: chroma_base[16].try_into().unwrap(),
        });

        push.push_method(clc5b0::H264SetMbhistBufOffset {
            offset: mem1_addr.try_into().unwrap(),
        });

        push.push_method(clc5b0::Execute {
            notify: clc5b0::ExecuteNotify::Disable,
            notify_on: clc5b0::ExecuteNotifyOn::End,
            awaken: clc5b0::ExecuteAwaken::Disable,
        });

        append_rust_push(push, nvk_cmd);
        self.frame_num = self.frame_num.wrapping_add(1);
    }
}
