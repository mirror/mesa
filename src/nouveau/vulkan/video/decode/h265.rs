// Copyright © 2024 Collabora, Ltd
// SPDX-License-Identifier: MIT

//! H265 decode implementation.

use std::collections::HashMap;
use std::collections::HashSet;

use crate::align_u32;
use crate::append_rust_push;
use crate::decode::VideoDecoder;
use crate::use_video_engine;
use crate::vk_find_struct_const;

use nv_push_rs::Push;
use nvidia_headers::classes::clc5b0::mthd as clc5b0;
use nvidia_headers::classes::clc9b0::mthd as clc9b0;
use nvk_video_bindings::nvk_cmd_buffer;
use nvk_video_bindings::nvk_image;
use nvk_video_bindings::nvk_image_view;
use nvk_video_bindings::StdVideoDecodeH265PictureInfo;
use nvk_video_bindings::StdVideoH265PictureParameterSet;
use nvk_video_bindings::StdVideoH265SequenceParameterSet;
use nvk_video_bindings::VkVideoBeginCodingInfoKHR;
use nvk_video_bindings::VkVideoReferenceSlotInfoKHR;
use nvk_video_bindings::_nvdec_hevc_pic_s;
use nvk_video_bindings::VK_SUCCESS;

const CTU_SIZE: u32 = 64;
const MB_SIZE: u32 = 16;
const FILTER_SIZE: u32 = 608;
const SAO_SIZE: u32 = 4864;

// Sourced from nvdec_drv.h, in bytes.
const GIP_ASIC_TILE_SIZE: usize = (20 * 22 * 2 * 2 + 16 + 15) & !0xf;

const MAX_TILE_SIZE_ENTRIES: usize =
    GIP_ASIC_TILE_SIZE / std::mem::size_of::<u16>();

#[repr(C)]
#[allow(non_snake_case)]
// This was sourced from the Tegra implementation in ffmpeg, which was also
// reverse engineered. It's the best we can do without any official
// documentation.
struct ScalingList {
    pub ScalingListDCCoeff16x16: [u8; 6],
    pub ScalingListDCCoeff32x32: [u8; 2],
    pub reserved0: [u8; 8],

    pub ScalingList4x4: [[u8; 16]; 6],
    pub ScalingList8x8: [[u8; 64]; 6],
    pub ScalingList16x16: [[u8; 64]; 6],
    pub ScalingList32x32: [[u8; 64]; 2],
}

impl ScalingList {
    fn from_vulkan_params(
        sps: &StdVideoH265SequenceParameterSet,
        pps: &StdVideoH265PictureParameterSet,
    ) -> Self {
        let sl = if pps.flags.pps_scaling_list_data_present_flag() != 0 {
            unsafe { *pps.pScalingLists }
        } else {
            unsafe { *sps.pScalingLists }
        };

        Self {
            ScalingListDCCoeff16x16: sl.ScalingListDCCoef16x16,
            ScalingListDCCoeff32x32: sl.ScalingListDCCoef32x32,
            reserved0: [0; 8],
            ScalingList4x4: Self::rearrange_scaling_list_4x4(sl.ScalingList4x4),
            ScalingList8x8: Self::rearrange_scaling_list_16x16(
                sl.ScalingList8x8,
            ),
            ScalingList16x16: Self::rearrange_scaling_list_16x16(
                sl.ScalingList16x16,
            ),
            ScalingList32x32: Self::rearrange_scaling_list_32x32(
                sl.ScalingList32x32,
            ),
        }
    }

    fn rearrange_scaling_list_4x4(
        scaling_list: [[u8; 16]; 6],
    ) -> [[u8; 16]; 6] {
        // I have no idea whether that's zigzag, raster or up-right diagonal.
        // This was straight up reverse engineered by poking at the values from
        // the blob.
        const PERMUTATION: [usize; 16] =
            [0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15];

        let mut rearranged = [[0u8; 16]; 6];
        for i in 0..6 {
            for (j, &index) in PERMUTATION.iter().enumerate() {
                rearranged[i][index] = scaling_list[i][j];
            }
        }
        rearranged
    }

    fn rearrange_scaling_list_16x16(
        scaling_list: [[u8; 64]; 6],
    ) -> [[u8; 64]; 6] {
        // Same here.
        const PERMUTATION: [usize; 64] = [
            0, 8, 16, 24, 32, 40, 48, 56, 1, 9, 17, 25, 33, 41, 49, 57, 2, 10,
            18, 26, 34, 42, 50, 58, 3, 11, 19, 27, 35, 43, 51, 59, 4, 12, 20,
            28, 36, 44, 52, 60, 5, 13, 21, 29, 37, 45, 53, 61, 6, 14, 22, 30,
            38, 46, 54, 62, 7, 15, 23, 31, 39, 47, 55, 63,
        ];

        let mut rearranged = [[0u8; 64]; 6];
        for i in 0..6 {
            for (j, &index) in PERMUTATION.iter().enumerate() {
                rearranged[i][index] = scaling_list[i][j];
            }
        }
        rearranged
    }

    // XXX: this can be combined with 16x16, it's the same scan order.
    fn rearrange_scaling_list_32x32(
        scaling_list: [[u8; 64]; 2],
    ) -> [[u8; 64]; 2] {
        const PERMUTATION: [usize; 64] = [
            0, 8, 16, 24, 32, 40, 48, 56, 1, 9, 17, 25, 33, 41, 49, 57, 2, 10,
            18, 26, 34, 42, 50, 58, 3, 11, 19, 27, 35, 43, 51, 59, 4, 12, 20,
            28, 36, 44, 52, 60, 5, 13, 21, 29, 37, 45, 53, 61, 6, 14, 22, 30,
            38, 46, 54, 62, 7, 15, 23, 31, 39, 47, 55, 63,
        ];

        let mut rearranged = [[0u8; 64]; 2];
        for i in 0..1 {
            for (j, &index) in PERMUTATION.iter().enumerate() {
                rearranged[i][index] = scaling_list[i][j];
            }
        }
        rearranged
    }
}

fn to_gob_value(y_log2: u8) -> u8 {
    match 1 << y_log2 {
        2 => 0,
        4 => 1,
        8 => 2,
        16 => 3,
        32 => 4,
        other => panic!("unsupported GOB value: {other}"),
    }
}

struct GpuBuffers {
    coloc_address: u64,
    filter_address: u64,

    sao_offset: u32,
    bsd_offset: u32,

    colmv_size: u32,
}

impl GpuBuffers {
    fn new(
        nvk_cmd: *mut nvk_cmd_buffer,
        sps: &StdVideoH265SequenceParameterSet,
    ) -> Self {
        let aligned_w = align_u32(sps.pic_width_in_luma_samples, CTU_SIZE);
        let aligned_h = align_u32(sps.pic_height_in_luma_samples, CTU_SIZE);
        let addreses = unsafe {
            let vid = (*nvk_cmd).video.vid;

            let mem0 = (*(*vid).mems[0].mem).mem;
            let mem0_addr = (*(*mem0).va).addr + (*vid).mems[0].offset;

            let mem1 = (*(*vid).mems[1].mem).mem;
            let mem1_addr = (*(*mem1).va).addr + (*vid).mems[1].offset;

            let mem2 = (*(*vid).mems[2].mem).mem;
            let mem2_addr = (*(*mem2).va).addr + (*vid).mems[2].offset;

            (mem0_addr >> 8, mem1_addr >> 8, mem2_addr >> 8)
        };

        // The SAO data is co-located with the filter buffer.
        let sao_offset = FILTER_SIZE * sps.pic_height_in_luma_samples;
        // Same for this.
        let bsd_offset = sao_offset + SAO_SIZE * sps.pic_height_in_luma_samples;

        GpuBuffers {
            coloc_address: addreses.0,
            filter_address: addreses.1,
            sao_offset,
            bsd_offset,
            colmv_size: aligned_w * aligned_h / MB_SIZE,
        }
    }
}

#[derive(Debug, Clone, Copy)]
struct Uploads {
    pic: u64,
    tile_offsets: u64,
    mbstatus: u64,
    scaling_list: u64,
    // Only for c9b0?
    intra_top: u64,
}

impl Uploads {
    /// Upload the parameters to the GPU.
    fn upload_to_the_gpu(
        nvk_cmd: *mut nvk_cmd_buffer,
        nvh265: _nvdec_hevc_pic_s,
        tile_offsets: [u16; MAX_TILE_SIZE_ENTRIES as usize],
        scaling_lists: Option<ScalingList>,
    ) -> Self {
        let mut nvh265_ptr = std::ptr::null_mut();
        let mut pic_gpu_addr = 0;
        unsafe {
            let res = nvk_video_bindings::nvk_cmd_buffer_upload_alloc(
                nvk_cmd,
                std::mem::size_of::<nvk_video_bindings::nvdec_hevc_pic_s>()
                    .try_into()
                    .unwrap(),
                256,
                &mut pic_gpu_addr,
                &mut nvh265_ptr as *mut *mut _ as *mut *mut std::ffi::c_void,
            );

            assert!(res == VK_SUCCESS);
            std::ptr::copy_nonoverlapping(&nvh265, nvh265_ptr, 1);
        }

        let mut tile_offsets_ptr = std::ptr::null_mut();
        let mut tile_offsets_address = 0;

        unsafe {
            let res = nvk_video_bindings::nvk_cmd_buffer_upload_alloc(
                nvk_cmd,
                std::mem::size_of_val(&tile_offsets).try_into().unwrap(),
                256,
                &mut tile_offsets_address,
                &mut tile_offsets_ptr as *mut *mut _
                    as *mut *mut std::ffi::c_void,
            );

            assert!(res == VK_SUCCESS);
            std::ptr::copy_nonoverlapping(
                tile_offsets.as_ptr(),
                tile_offsets_ptr,
                tile_offsets.len(),
            );
        }

        // I have no idea why, since this is supposed to be a small buffer
        // containing some status from the decode job, but if you use a lower
        // size like h264, you regress on the test suite. I guess this just
        // highlights that there's something more going on here that we do not
        // know.
        //
        // TODO: place this in its own vkDeviceMemory.
        let mut mbstatus_ptr = std::ptr::null_mut();
        let mut mbstatus_address = 0;
        unsafe {
            let res = nvk_video_bindings::nvk_cmd_buffer_upload_alloc(
                nvk_cmd,
                65536,
                256,
                &mut mbstatus_address,
                &mut mbstatus_ptr as *mut *mut _ as *mut *mut std::ffi::c_void,
            );

            assert!(res == VK_SUCCESS);
        }

        let mut scaling_list_ptr = std::ptr::null_mut();
        let mut scaling_list_address = 0;
        const SL_SIZE: usize = std::mem::size_of::<ScalingList>();
        unsafe {
            let res = nvk_video_bindings::nvk_cmd_buffer_upload_alloc(
                nvk_cmd,
                SL_SIZE as _,
                256,
                &mut scaling_list_address,
                &mut scaling_list_ptr as *mut *mut _
                    as *mut *mut std::ffi::c_void,
            );

            assert!(res == VK_SUCCESS);

            match scaling_lists {
                None => {
                    let zero = [0u8; SL_SIZE];
                    std::ptr::copy_nonoverlapping(
                        zero.as_ptr(),
                        scaling_list_ptr,
                        SL_SIZE,
                    );
                }
                Some(scaling_list) => {
                    std::ptr::copy_nonoverlapping(
                        &scaling_list as *const _ as *const u8,
                        scaling_list_ptr,
                        SL_SIZE,
                    );
                }
            }
        }

        // TODO: make this its own VkDeviceMemory, there is no reason to use the
        // upload BO, specially since this thing is 65K bytes.
        let mut intra_top = std::ptr::null_mut();
        let mut intra_top_address = 0;
        unsafe {
            let res = nvk_video_bindings::nvk_cmd_buffer_upload_alloc(
                nvk_cmd,
                65536,
                256,
                &mut intra_top_address,
                &mut intra_top as *mut *mut _ as *mut *mut std::ffi::c_void,
            );

            assert!(res == VK_SUCCESS);
        }

        Uploads {
            pic: pic_gpu_addr,
            tile_offsets: tile_offsets_address,
            mbstatus: mbstatus_address,
            scaling_list: scaling_list_address,
            intra_top: intra_top_address,
        }
    }
}

#[derive(Debug, Default)]
struct FrameData {
    /// The `pic_idx` value associated with this frame.
    pic_idx: Option<u32>,
    slot_index: Option<i32>,
}

#[derive(Debug, Default)]
pub(crate) struct Decoder {
    /// Data associated with each image view.
    slots: HashMap<i32, FrameData>,
    frame_num: u32,

    // TODO: this can be a simple bitmask
    free_pic_slots: HashSet<u32>,
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

    /// Get the `pic_idx` value associated with the given image view. If this is
    /// the first time we are seeing this image view, then allocate a new
    /// `pic_idx` value.
    fn get_pic_idx(&mut self, slot_idx: i32) -> u32 {
        if let Some(frame_data) = self.slots.get(&slot_idx) {
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

        self.slots.insert(slot_idx, frame_data);

        pic_idx
    }

    fn find_by_slot_idx<'a>(
        slots: &'a mut HashMap<i32, FrameData>,
        slot_idx: i32,
    ) -> &'a FrameData {
        slots.values()
        .find(|f| f.slot_index.unwrap() == slot_idx)
        .expect("Frame data not found. Either this picture was not submitted or invalidated.")
    }

    fn rps_to_ref_list(
        &mut self,
        rps: &[u8; 8],
        list: &mut [u8],
        start_index: &mut usize,
    ) {
        for &slot_idx in rps.iter().take(8) {
            if slot_idx == 0xff {
                break;
            }

            let frame_data =
                Self::find_by_slot_idx(&mut self.slots, slot_idx as i32);
            list[*start_index] = frame_data.pic_idx.unwrap() as u8;
            *start_index += 1;
        }
    }

    fn set_reference_frames(
        &mut self,
        nvh265: &mut nvk_video_bindings::nvdec_hevc_pic_s,
        frame_info: &nvk_video_bindings::VkVideoDecodeInfoKHR,
        std_pic_info: &StdVideoDecodeH265PictureInfo,
        (luma_base, chroma_base): (&mut [u32; 17], &mut [u32; 17]),
    ) {
        let mut current_index = 0;
        self.rps_to_ref_list(
            &std_pic_info.RefPicSetStCurrBefore,
            &mut nvh265.initreflistidxl0,
            &mut current_index,
        );
        self.rps_to_ref_list(
            &std_pic_info.RefPicSetStCurrAfter,
            &mut nvh265.initreflistidxl0,
            &mut current_index,
        );
        self.rps_to_ref_list(
            &std_pic_info.RefPicSetLtCurr,
            &mut nvh265.initreflistidxl0,
            &mut current_index,
        );

        let mut current_index = 0;
        self.rps_to_ref_list(
            &std_pic_info.RefPicSetStCurrAfter,
            &mut nvh265.initreflistidxl1,
            &mut current_index,
        );
        self.rps_to_ref_list(
            &std_pic_info.RefPicSetStCurrBefore,
            &mut nvh265.initreflistidxl1,
            &mut current_index,
        );
        self.rps_to_ref_list(
            &std_pic_info.RefPicSetLtCurr,
            &mut nvh265.initreflistidxl1,
            &mut current_index,
        );

        for i in 0..frame_info.referenceSlotCount as usize {
            let (vk_ref_slot, iv) =
                Decoder::get_ith_slot_for_frame(frame_info, i);

            let dpb_slot = vk_find_struct_const!(
                vk_ref_slot.pNext,
                VIDEO_DECODE_H265_DPB_SLOT_INFO,
                KHR
            );

            let vk_ref_info = unsafe { *dpb_slot.pStdReferenceInfo };
            let pic_idx = self.get_pic_idx(vk_ref_slot.slotIndex);

            nvh265.RefDiffPicOrderCnts[pic_idx as usize] =
                (std_pic_info.PicOrderCntVal - vk_ref_info.PicOrderCntVal)
                    as i16;

            let img = unsafe { (*iv).vk.image as *mut nvk_image };

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

    fn remove_invalid_slots(&mut self, begin_info: &VkVideoBeginCodingInfoKHR) {
        let mut entries_to_remove = Vec::new();

        for (&slot_idx, frame_data) in &mut self.slots {
            let mut found = false;

            for i in 0..begin_info.referenceSlotCount {
                let (ref_slot, _) =
                    Self::get_ith_planned_slot(begin_info, i as usize);

                if slot_idx == ref_slot.slotIndex {
                    found = true;
                    frame_data.slot_index = Some(ref_slot.slotIndex);
                    break;
                }
            }

            if !found {
                entries_to_remove.push(slot_idx);
            }
        }

        for key in entries_to_remove {
            self.slots.remove(&key).unwrap();
        }

        // i.e.: everything is free.
        self.free_pic_slots = (0..=16).collect();
        for frame_slot in self.slots.values() {
            if let Some(pic_idx) = frame_slot.pic_idx {
                self.free_pic_slots.remove(&pic_idx);
            }
        }
    }

    fn build_push(
        &mut self,
        gpu_addrs: &Uploads,
        gpu_buffers: &GpuBuffers,
        src_address: u64,
        (luma_base, chroma_base): (&mut [u32; 17], &mut [u32; 17]),
    ) -> Push {
        let mut push = Push::new();

        push.push_method(clc5b0::SetApplicationId {
            id: clc5b0::SetApplicationIdId::Hevc,
        });

        push.push_method(clc5b0::SetControlParams {
            codec_type: clc5b0::SetControlParamsCodecType::Hevc,
            gptimer_on: 1,
            err_conceal_on: 1,
            mbtimer_on: 1,
            error_frm_idx: 0, //(self.frame_num), // XXX TODO
            ret_error: 0,
            ec_intra_frame_using_pslc: 0,
            all_intra_frame: 0,
            reserved: Default::default(),
        });

        push.push_method(clc5b0::SetPictureIndex {
            index: self.frame_num,
        });

        push.push_method(clc5b0::SetDrvPicSetupOffset {
            offset: (gpu_addrs.pic >> 8).try_into().unwrap(),
        });

        push.push_method(clc5b0::SetInBufBaseOffset {
            offset: (src_address >> 8).try_into().unwrap(),
        });

        push.push_method(clc5b0::SetNvdecStatusOffset {
            offset: (gpu_addrs.mbstatus >> 8).try_into().unwrap(),
        });

        push.push_method(clc5b0::SetColocDataOffset {
            offset: (gpu_buffers.coloc_address).try_into().unwrap(),
        });

        push.push_method(clc5b0::HevcSetFilterBufferOffset {
            offset: (gpu_buffers.filter_address).try_into().unwrap(),
        });

        push.push_method(clc5b0::HevcSetTileSizesOffset {
            offset: (gpu_addrs.tile_offsets >> 8).try_into().unwrap(),
        });

        push.push_method(clc5b0::HevcSetScalingListOffset {
            offset: (gpu_addrs.scaling_list >> 8).try_into().unwrap(),
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

        push.push_method(clc9b0::SetIntraTopBufOffset {
            offset: (gpu_addrs.intra_top >> 8).try_into().unwrap(),
        });

        push.push_method(clc5b0::Execute {
            notify: clc5b0::ExecuteNotify::Disable,
            notify_on: clc5b0::ExecuteNotifyOn::End,
            awaken: clc5b0::ExecuteAwaken::Disable,
        });

        push
    }

    /// Compute some obscure skip value needed by the hardware, in bits.
    ///
    /// This encompasses all bits read from `if (pps->output_present_flag)` up
    /// into `delta_poc_msb_cycle_lt`.
    ///
    /// Unfortunately, this value is not passed
    /// in by Vulkan Video, and we do not have access to the slice header
    /// directly, so we must use the flags we have as a proxy.
    fn compute_sw_hdr_skip_len(
        sps: &StdVideoH265SequenceParameterSet,
        pps: &StdVideoH265PictureParameterSet,
        std_pic_info: &StdVideoDecodeH265PictureInfo,
    ) -> u16 {
        let mut sw_hdr_skip_len = 0;

        if pps.flags.output_flag_present_flag() != 0 {
            sw_hdr_skip_len += 1; // output_flag_present_flag
        }

        if sps.flags.separate_colour_plane_flag() != 0 {
            sw_hdr_skip_len += 2; // colour_plane_id
        }

        if std_pic_info.flags.IdrPicFlag() == 0 {
            sw_hdr_skip_len +=
                u16::from(sps.log2_max_pic_order_cnt_lsb_minus4) + 4;
            sw_hdr_skip_len += 1; // short_term_ref_pic_set_sps_flag

            if std_pic_info.flags.short_term_ref_pic_set_sps_flag() == 0 {
                sw_hdr_skip_len += std_pic_info.NumBitsForSTRefPicSetInSlice;
            } else if sps.num_short_term_ref_pic_sets > 1 {
                sw_hdr_skip_len +=
                    sps.num_short_term_ref_pic_sets.ilog2() as u16;
                if !sps.num_short_term_ref_pic_sets.is_power_of_two() {
                    sw_hdr_skip_len += 1; // ceil
                }
            }

            // XXX: we also need to compute the bits for the long term RPS.
            // Unfortunately this is not in the Vulkan Video API, but it should
            // be doable by inferring from the values in the SPS, PPS and
            // StdVideoH265PictureInfo.
            //
            // It would be great if we had some
            // `std_pic_info.NumBitsForLTRefPicSetInSlice` instead.
        }

        sw_hdr_skip_len
    }

    /// Fill the tile size buffer. This is reverse engineered, but so far, the
    /// assumption that it's based on 6.5.1 of the spec seems to hold.
    ///
    /// The hexdump for a 144x144 file shows 0x00030003. For now it's unclear
    /// how non-square tiles should be represented.
    fn fill_tile_size_buffer(
        tile_sizes: &mut [u16],
        sps: &StdVideoH265SequenceParameterSet,
        pps: &StdVideoH265PictureParameterSet,
    ) {
        if pps.flags.uniform_spacing_flag() != 0 {
            let (pic_width_in_ctbs_y, pic_height_in_ctbs_y) =
                Self::compute_sizes_in_ctbs(sps);

            let num_col = u32::from(pps.num_tile_columns_minus1 + 1);
            let num_row = u32::from(pps.num_tile_rows_minus1 + 1);

            // See the HEVC spec, 6-3 and 6-4.
            let mut entries = 0;
            for i in 0..num_row {
                let row_height = (i + 1) * pic_height_in_ctbs_y / num_row
                    - i * pic_height_in_ctbs_y / num_row;

                for j in 0..num_col {
                    let column_width = (j + 1) * pic_width_in_ctbs_y / num_col
                        - j * pic_width_in_ctbs_y / num_col;

                    tile_sizes[entries] = u16::try_from(column_width).unwrap();
                    tile_sizes[entries + 1] =
                        u16::try_from(row_height).unwrap();
                    entries += 2;
                }
            }
        } else {
            let num_col = usize::from(pps.num_tile_columns_minus1 + 1);
            let num_row = usize::from(pps.num_tile_rows_minus1 + 1);

            let mut entries = 0;
            for i in 0..num_row {
                for j in 0..num_col {
                    tile_sizes[entries] = pps.column_width_minus1[j] + 1;
                    tile_sizes[entries + 1] = pps.row_height_minus1[i] + 1;
                    entries += 2;
                }
            }
        }
    }

    /// Compute the value of PicWidthInCtbsY and PicHeightInCtbsY.
    ///
    /// See the HEVC spec, 7-10 through 7-19.
    fn compute_sizes_in_ctbs(
        sps: &StdVideoH265SequenceParameterSet,
    ) -> (u32, u32) {
        let min_cb_log2_size_y = sps.log2_min_luma_coding_block_size_minus3 + 3;
        let ctb_log2_size_y =
            min_cb_log2_size_y + sps.log2_diff_max_min_luma_coding_block_size;
        let ctb_size_y = 1 << ctb_log2_size_y;

        let pic_height_in_ctbs_y =
            sps.pic_height_in_luma_samples.div_ceil(ctb_size_y);
        let pic_width_in_ctbs_y =
            sps.pic_width_in_luma_samples.div_ceil(ctb_size_y);

        (pic_width_in_ctbs_y, pic_height_in_ctbs_y)
    }
}

impl VideoDecoder for Decoder {
    fn begin(
        &mut self,
        nvk_cmd: *mut nvk_cmd_buffer,
        begin_info: &nvk_video_bindings::VkVideoBeginCodingInfoKHR,
    ) {
        self.remove_invalid_slots(begin_info);
        let mut push = Push::new();

        use_video_engine(&mut push);
        append_rust_push(push, nvk_cmd);
    }

    fn decode(
        &mut self,
        nvk_cmd: *mut nvk_cmd_buffer,
        frame_info: &nvk_video_bindings::VkVideoDecodeInfoKHR,
    ) {
        let h265_pic_info = vk_find_struct_const!(
            frame_info.pNext,
            VIDEO_DECODE_H265_PICTURE_INFO,
            KHR
        );

        let std_pic_info = unsafe { *h265_pic_info.pStdPictureInfo };

        let pps = unsafe {
            *nvk_video_bindings::vk_video_find_h265_dec_std_pps(
                (*nvk_cmd).video.params as *const _,
                std_pic_info.pps_pic_parameter_set_id.into(),
            )
        };
        let sps = unsafe {
            *nvk_video_bindings::vk_video_find_h265_dec_std_sps(
                (*nvk_cmd).video.params as *const _,
                pps.sps_video_parameter_set_id.into(),
            )
        };

        let dst_iv = unsafe {
            nvk_video_bindings::nvk_image_view_from_handle(
                frame_info.dstPictureResource.imageViewBinding,
            )
        };
        let dst_img =
            unsafe { *dst_iv }.vk.image as *mut nvk_video_bindings::nvk_image;

        let mut nvh265 = nvk_video_bindings::_nvdec_hevc_pic_s::default();

        nvh265.stream_len = u32::try_from(frame_info.srcBufferRange).unwrap();
        nvh265.set_tileformat(1);

        let y_log2 = unsafe { *dst_img }.planes[0].nil.levels[0].tiling.y_log2;
        nvh265.set_gob_height(to_gob_value(y_log2));
        nvh265.sw_start_code_e = 1;

        let bit_depth = sps.bit_depth_luma_minus8 + 8;
        let output_mode = if bit_depth >= 10 { 1 } else { 0 };

        nvh265.framestride[0] =
            unsafe { *dst_img }.planes[0].nil.levels[0].row_stride_B;
        nvh265.framestride[1] =
            unsafe { *dst_img }.planes[1].nil.levels[0].row_stride_B;

        if output_mode == 1 {
            nvh265.framestride[0] /= 2;
            nvh265.framestride[1] /= 2;
        }

        let gpu_buffers = GpuBuffers::new(nvk_cmd, &sps);
        nvh265.colMvBuffersize = gpu_buffers.colmv_size >> 8;
        nvh265.HevcSaoBufferOffset = gpu_buffers.sao_offset >> 8;
        nvh265.HevcBsdCtrlOffset = gpu_buffers.bsd_offset >> 8;

        nvh265.pic_width_in_luma_samples =
            sps.pic_width_in_luma_samples.try_into().unwrap();
        nvh265.pic_height_in_luma_samples =
            sps.pic_height_in_luma_samples.try_into().unwrap();

        /* we only support 4:2:0 for now */
        nvh265.set_chroma_format_idc(1);

        nvh265.set_bit_depth_luma(u32::from(sps.bit_depth_luma_minus8) + 8);
        nvh265.set_bit_depth_chroma(u32::from(sps.bit_depth_chroma_minus8) + 8);

        nvh265.set_log2_min_luma_coding_block_size(
            u32::from(sps.log2_min_luma_coding_block_size_minus3) + 3,
        );
        nvh265.set_log2_max_luma_coding_block_size(u32::from(
            sps.log2_min_luma_coding_block_size_minus3
                + 3
                + sps.log2_diff_max_min_luma_coding_block_size,
        ));

        nvh265.set_log2_min_transform_block_size(
            u32::from(sps.log2_min_luma_transform_block_size_minus2) + 2,
        );

        nvh265.set_log2_max_transform_block_size(u32::from(
            sps.log2_min_luma_transform_block_size_minus2
                + 2
                + sps.log2_diff_max_min_luma_transform_block_size,
        ));

        nvh265.set_max_transform_hierarchy_depth_inter(u32::from(
            sps.max_transform_hierarchy_depth_inter,
        ));

        nvh265.set_max_transform_hierarchy_depth_intra(u32::from(
            sps.max_transform_hierarchy_depth_intra,
        ));

        nvh265.set_scalingListEnable(sps.flags.scaling_list_enabled_flag());

        nvh265.set_amp_enable_flag(sps.flags.amp_enabled_flag());

        nvh265.set_sample_adaptive_offset_enabled_flag(
            sps.flags.sample_adaptive_offset_enabled_flag(),
        );

        nvh265.set_pcm_enabled_flag(sps.flags.pcm_enabled_flag());

        if sps.flags.pcm_enabled_flag() != 0 {
            nvh265.set_pcm_sample_bit_depth_luma(
                u32::from(sps.pcm_sample_bit_depth_luma_minus1) + 1,
            );

            nvh265.set_pcm_sample_bit_depth_chroma(
                u32::from(sps.pcm_sample_bit_depth_chroma_minus1) + 1,
            );

            nvh265.set_log2_min_pcm_luma_coding_block_size(
                u32::from(sps.log2_min_pcm_luma_coding_block_size_minus3) + 3,
            );
            nvh265.set_log2_max_pcm_luma_coding_block_size(u32::from(
                sps.log2_min_pcm_luma_coding_block_size_minus3
                    + 3
                    + sps.log2_diff_max_min_pcm_luma_coding_block_size,
            ));
        }

        nvh265.set_pcm_loop_filter_disabled_flag(
            sps.flags.pcm_loop_filter_disabled_flag(),
        );

        nvh265.set_sps_temporal_mvp_enabled_flag(
            sps.flags.sps_temporal_mvp_enabled_flag(),
        );

        nvh265.set_strong_intra_smoothing_enabled_flag(
            sps.flags.strong_intra_smoothing_enabled_flag(),
        );

        nvh265.set_dependent_slice_segments_enabled_flag(
            pps.flags.dependent_slice_segments_enabled_flag(),
        );

        nvh265
            .set_output_flag_present_flag(pps.flags.output_flag_present_flag());

        nvh265.set_num_extra_slice_header_bits(u32::from(
            pps.num_extra_slice_header_bits,
        ));

        nvh265.set_sign_data_hiding_enabled_flag(
            pps.flags.sign_data_hiding_enabled_flag(),
        );

        nvh265.set_cabac_init_present_flag(pps.flags.cabac_init_present_flag());

        nvh265.set_num_ref_idx_l0_default_active(
            u32::from(pps.num_ref_idx_l0_default_active_minus1) + 1,
        );

        nvh265.set_num_ref_idx_l1_default_active(
            u32::from(pps.num_ref_idx_l1_default_active_minus1) + 1,
        );

        let init_qp = pps.init_qp_minus26 + 26;
        let init_qp =
            i32::from(init_qp) + i32::from(sps.bit_depth_luma_minus8 * 6);
        nvh265.set_init_qp(init_qp.try_into().unwrap());

        nvh265.set_constrained_intra_pred_flag(
            pps.flags.constrained_intra_pred_flag(),
        );

        nvh265.set_transform_skip_enabled_flag(
            pps.flags.transform_skip_enabled_flag(),
        );

        nvh265
            .set_cu_qp_delta_enabled_flag(pps.flags.cu_qp_delta_enabled_flag());

        nvh265
            .set_diff_cu_qp_delta_depth(u32::from(pps.diff_cu_qp_delta_depth));

        nvh265.pps_cb_qp_offset = pps.pps_cb_qp_offset;
        nvh265.pps_cr_qp_offset = pps.pps_cr_qp_offset;
        nvh265.pps_beta_offset = pps.pps_beta_offset_div2 * 2;

        nvh265.pps_tc_offset = pps.pps_tc_offset_div2 * 2;
        nvh265.set_pps_slice_chroma_qp_offsets_present_flag(
            pps.flags.pps_slice_chroma_qp_offsets_present_flag(),
        );

        nvh265.set_weighted_pred_flag(pps.flags.weighted_pred_flag());
        nvh265.set_weighted_bipred_flag(pps.flags.weighted_bipred_flag());
        nvh265.set_transquant_bypass_enabled_flag(
            pps.flags.transquant_bypass_enabled_flag(),
        );

        nvh265.set_tiles_enabled_flag(pps.flags.tiles_enabled_flag());
        nvh265.set_entropy_coding_sync_enabled_flag(
            pps.flags.entropy_coding_sync_enabled_flag(),
        );

        if pps.flags.tiles_enabled_flag() != 0 {
            nvh265.set_num_tile_rows(u32::from(pps.num_tile_rows_minus1) + 1);
            nvh265.set_num_tile_columns(
                u32::from(pps.num_tile_columns_minus1) + 1,
            );
        }

        nvh265.set_loop_filter_across_tiles_enabled_flag(
            pps.flags.loop_filter_across_tiles_enabled_flag(),
        );

        nvh265.set_loop_filter_across_slices_enabled_flag(
            pps.flags.pps_loop_filter_across_slices_enabled_flag(),
        );

        nvh265.set_deblocking_filter_control_present_flag(
            pps.flags.deblocking_filter_control_present_flag(),
        );

        nvh265.set_deblocking_filter_override_enabled_flag(
            pps.flags.deblocking_filter_override_enabled_flag(),
        );

        nvh265.set_pps_deblocking_filter_disabled_flag(
            pps.flags.pps_deblocking_filter_disabled_flag(),
        );

        nvh265.set_lists_modification_present_flag(
            pps.flags.lists_modification_present_flag(),
        );

        nvh265.set_log2_parallel_merge_level(
            u32::from(pps.log2_parallel_merge_level_minus2) + 2,
        );

        nvh265.set_slice_segment_header_extension_present_flag(
            pps.flags.slice_segment_header_extension_present_flag(),
        );

        // XXX: this probably won't work always?
        nvh265.num_ref_frames = self.slots.len().try_into().unwrap();
        if pps.flags.pps_curr_pic_ref_enabled_flag() != 0 {
            nvh265.num_ref_frames += 1;
        }

        nvh265.IDR_picture_flag =
            std_pic_info.flags.IdrPicFlag().try_into().unwrap();
        nvh265.RAP_picture_flag =
            std_pic_info.flags.IrapPicFlag().try_into().unwrap();

        // XXX: traced from the blob, not sure what this is..
        nvh265.pattern_id = 2;

        nvh265.sw_hdr_skip_length =
            Self::compute_sw_hdr_skip_len(&sps, &pps, &std_pic_info);

        nvh265.set_separate_colour_plane_flag(
            sps.flags.separate_colour_plane_flag(),
        );

        nvh265.set_log2_max_pic_order_cnt_lsb_minus4(u32::from(
            sps.log2_max_pic_order_cnt_lsb_minus4,
        ));

        nvh265.set_num_short_term_ref_pic_sets(
            sps.num_short_term_ref_pic_sets.try_into().unwrap(),
        );

        nvh265.set_num_long_term_ref_pics_sps(
            sps.num_long_term_ref_pics_sps.try_into().unwrap(),
        );

        nvh265.set_long_term_ref_pics_present_flag(
            sps.flags.long_term_ref_pics_present_flag(),
        );

        nvh265.set_num_delta_pocs_of_rps_idx(
            std_pic_info.NumDeltaPocsOfRefRpsIdx.try_into().unwrap(),
        );

        if std_pic_info.flags.short_term_ref_pic_set_sps_flag() == 0 {
            nvh265.num_bits_short_term_ref_pics_in_slice = std_pic_info
                .NumBitsForSTRefPicSetInSlice
                .try_into()
                .unwrap();
        } else if sps.num_short_term_ref_pic_sets > 1 {
            nvh265.num_bits_short_term_ref_pics_in_slice +=
                sps.num_short_term_ref_pic_sets.ilog2();
            if !sps.num_short_term_ref_pic_sets.is_power_of_two() {
                nvh265.num_bits_short_term_ref_pics_in_slice += 1; // ceil
            }
        }

        nvh265.v3.set_slice_ec_mv_type(1); // i.e.: colocated MVs.
        nvh265.v3.set_slice_ec_slice_type(
            nvh265.IDR_picture_flag.try_into().unwrap(),
        );

        let aligned_w = align_u32(sps.pic_width_in_luma_samples, CTU_SIZE);
        let aligned_h = align_u32(sps.pic_height_in_luma_samples, CTU_SIZE);
        nvh265.v3.HevcSliceEdgeOffset =
            ((624 * aligned_h) + (5016 * aligned_h) + (aligned_w * aligned_h))
                >> 8;

        let ext = &mut nvh265.v1.hevc_main10_444_ext;

        ext.HevcSaoAboveOffset = nvh265.v3.HevcSliceEdgeOffset;
        ext.HevcFltAboveOffset =
            (FILTER_SIZE * aligned_h) + (5016 * aligned_h) >> 8;

        ext.set_transformSkipRotationEnableFlag(
            sps.flags.transform_skip_rotation_enabled_flag(),
        );
        ext.set_transformSkipContextEnableFlag(
            sps.flags.transform_skip_context_enabled_flag(),
        );

        ext.set_implicitRdpcmEnableFlag(
            sps.flags.implicit_rdpcm_enabled_flag(),
        );
        ext.set_explicitRdpcmEnableFlag(
            sps.flags.explicit_rdpcm_enabled_flag(),
        );

        ext.set_extendedPrecisionProcessingFlag(
            sps.flags.extended_precision_processing_flag(),
        );
        ext.set_intraSmoothingDisabledFlag(
            sps.flags.intra_smoothing_disabled_flag(),
        );
        ext.set_highPrecisionOffsetsEnableFlag(
            sps.flags.high_precision_offsets_enabled_flag(),
        );
        ext.set_fastRiceAdaptationEnableFlag(
            sps.flags.persistent_rice_adaptation_enabled_flag(),
        );
        ext.set_cabacBypassAlignmentEnableFlag(
            sps.flags.cabac_bypass_alignment_enabled_flag(),
        );
        ext.set_log2MaxTransformSkipSize(
            u32::from(pps.log2_max_transform_skip_block_size_minus2) + 2,
        );
        ext.set_crossComponentPredictionEnableFlag(
            pps.flags.cross_component_prediction_enabled_flag(),
        );
        ext.set_chromaQpAdjustmentEnableFlag(
            pps.flags.chroma_qp_offset_list_enabled_flag(),
        );
        ext.set_diffCuChromaQpAdjustmentDepth(u32::from(
            pps.diff_cu_chroma_qp_offset_depth,
        ));
        ext.set_chromaQpAdjustmentTableSize(
            u32::from(pps.chroma_qp_offset_list_len_minus1) + 1,
        );
        ext.set_log2SaoOffsetScaleLuma(pps.log2_sao_offset_scale_luma.into());
        ext.set_log2SaoOffsetScaleChroma(
            pps.log2_sao_offset_scale_chroma.into(),
        );

        ext.cb_qp_adjustment = pps.cb_qp_offset_list;
        ext.cr_qp_adjustment = pps.cr_qp_offset_list;

        // Everything in nvh265.v2 comes from vps_extension(), which is not in Vulkan Video

        let mut tile_sizes = [0u16; MAX_TILE_SIZE_ENTRIES];

        Self::fill_tile_size_buffer(&mut tile_sizes, &sps, &pps);

        let mut luma_base = [0; 17];
        let mut chroma_base = [0; 17];

        self.set_reference_frames(
            &mut nvh265,
            &frame_info,
            &std_pic_info,
            (&mut luma_base, &mut chroma_base),
        );

        let setup_ref_slot = unsafe { *frame_info.pSetupReferenceSlot };

        let cur_pic_idx = self.get_pic_idx(setup_ref_slot.slotIndex);
        nvh265.curr_pic_idx = cur_pic_idx as u8;

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

        let scaling_lists = (sps.flags.scaling_list_enabled_flag() != 0)
            .then(|| ScalingList::from_vulkan_params(&sps, &pps));

        let gpu_addrs: Uploads = Uploads::upload_to_the_gpu(
            nvk_cmd,
            nvh265,
            tile_sizes,
            scaling_lists,
        );

        let src_buffer = unsafe {
            nvk_video_bindings::nvk_buffer_from_handle(frame_info.srcBuffer)
        };
        let src_address = unsafe {
            nvk_video_bindings::nvk_buffer_address(
                src_buffer,
                frame_info.srcBufferOffset,
            )
        };

        let push = self.build_push(
            &gpu_addrs,
            &gpu_buffers,
            src_address,
            (&mut luma_base, &mut chroma_base),
        );

        append_rust_push(push, nvk_cmd);
        self.frame_num += 1;
    }
}
