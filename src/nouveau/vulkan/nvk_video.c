#include "nvk_video.h"

#include "vk_alloc.h"
#include "nvk_buffer.h"
#include "nvk_device.h"
#include "nvk_cmd_buffer.h"
#include "nvk_image.h"
#include "nvk_image_view.h"
#include "nvk_physical_device.h"
#include "nvk_entrypoints.h"

#include "nv_push_cl906f.h"
#include "nv_push_clc5b0.h"

#include "nvidia/nvdec_drv.h"

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateVideoSessionKHR(VkDevice _device, const VkVideoSessionCreateInfoKHR *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator, VkVideoSessionKHR *pVideoSession)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);

   struct nvk_video_session *vid =
      vk_alloc2(&dev->vk.alloc, pAllocator, sizeof(*vid), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!vid)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(vid, 0, sizeof(struct nvk_video_session));

   VkResult result = vk_video_session_init(&dev->vk, &vid->vk, pCreateInfo);
   if (result != VK_SUCCESS) {
      vk_free2(&dev->vk.alloc, pAllocator, vid);
      return result;
   }
   *pVideoSession = nvk_video_session_to_handle(vid);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyVideoSessionKHR(VkDevice _device, VkVideoSessionKHR _session, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   VK_FROM_HANDLE(nvk_video_session, vid, _session);

   if (!_session)
      return;

   vk_object_base_finish(&vid->vk.base);
   vk_free2(&dev->vk.alloc, pAllocator, vid);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateVideoSessionParametersKHR(VkDevice _device, const VkVideoSessionParametersCreateInfoKHR *pCreateInfo,
                                     const VkAllocationCallbacks *pAllocator,
                                     VkVideoSessionParametersKHR *pVideoSessionParameters)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   VK_FROM_HANDLE(nvk_video_session, vid, pCreateInfo->videoSession);
   VK_FROM_HANDLE(nvk_video_session_params, templ, pCreateInfo->videoSessionParametersTemplate);

   struct nvk_video_session_params *params =
      vk_alloc2(&dev->vk.alloc, pAllocator, sizeof(*params), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!params)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result =
      vk_video_session_parameters_init(&dev->vk, &params->vk, &vid->vk, templ ? &templ->vk : NULL, pCreateInfo);
   if (result != VK_SUCCESS) {
      vk_free2(&dev->vk.alloc, pAllocator, params);
      return result;
   }

   *pVideoSessionParameters = nvk_video_session_params_to_handle(params);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyVideoSessionParametersKHR(VkDevice _device, VkVideoSessionParametersKHR _params,
                                      const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   VK_FROM_HANDLE(nvk_video_session_params, params, _params);
   vk_video_session_parameters_finish(&dev->vk, &params->vk);
   vk_free2(&dev->vk.alloc, pAllocator, params);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetPhysicalDeviceVideoCapabilitiesKHR(VkPhysicalDevice physicalDevice, const VkVideoProfileInfoKHR *pVideoProfile,
                                           VkVideoCapabilitiesKHR *pCapabilities)
{
   switch (pVideoProfile->videoCodecOperation) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
      break;
   default:
      unreachable("unsupported operation");
   }

   pCapabilities->flags = 0;
   pCapabilities->minBitstreamBufferOffsetAlignment = 256;
   pCapabilities->minBitstreamBufferSizeAlignment = 256;
   pCapabilities->pictureAccessGranularity.width = VK_VIDEO_H264_MACROBLOCK_WIDTH;
   pCapabilities->pictureAccessGranularity.height = VK_VIDEO_H264_MACROBLOCK_HEIGHT;
   pCapabilities->minCodedExtent.width = 48;
   pCapabilities->minCodedExtent.height = VK_VIDEO_H264_MACROBLOCK_HEIGHT;
   pCapabilities->maxCodedExtent.width = 4096;
   pCapabilities->maxCodedExtent.height = 4096;

   struct VkVideoDecodeCapabilitiesKHR *dec_caps =
      (struct VkVideoDecodeCapabilitiesKHR *)vk_find_struct(pCapabilities->pNext, VIDEO_DECODE_CAPABILITIES_KHR);
   if (dec_caps)
      dec_caps->flags = VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;
   /* H264 allows different luma and chroma bit depths */
   if (pVideoProfile->lumaBitDepth != pVideoProfile->chromaBitDepth)
      return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;

   if (pVideoProfile->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)
      return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;

   switch (pVideoProfile->videoCodecOperation) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: {

      struct VkVideoDecodeH264CapabilitiesKHR *ext = (struct VkVideoDecodeH264CapabilitiesKHR *)vk_find_struct(
         pCapabilities->pNext, VIDEO_DECODE_H264_CAPABILITIES_KHR);
      const struct VkVideoDecodeH264ProfileInfoKHR *h264_profile =
         vk_find_struct_const(pVideoProfile->pNext, VIDEO_DECODE_H264_PROFILE_INFO_KHR);
      if (h264_profile->stdProfileIdc != STD_VIDEO_H264_PROFILE_IDC_BASELINE &&
          h264_profile->stdProfileIdc != STD_VIDEO_H264_PROFILE_IDC_MAIN &&
          h264_profile->stdProfileIdc != STD_VIDEO_H264_PROFILE_IDC_HIGH)
         return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;

      if (pVideoProfile->lumaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR)
         return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;

      pCapabilities->flags = VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR;
      pCapabilities->maxDpbSlots = 17;
      pCapabilities->maxActiveReferencePictures = 16;
      ext->fieldOffsetGranularity.x = 0;
      ext->fieldOffsetGranularity.y = 0;
      ext->maxLevelIdc = STD_VIDEO_H265_LEVEL_IDC_5_2;
      strcpy(pCapabilities->stdHeaderVersion.extensionName, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME);
      pCapabilities->stdHeaderVersion.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION;
      break;
   }
   default:
      break;
   }
   return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetPhysicalDeviceVideoFormatPropertiesKHR(VkPhysicalDevice physicalDevice,
                                               const VkPhysicalDeviceVideoFormatInfoKHR *pVideoFormatInfo,
                                               uint32_t *pVideoFormatPropertyCount,
                                               VkVideoFormatPropertiesKHR *pVideoFormatProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkVideoFormatPropertiesKHR, out, pVideoFormatProperties, pVideoFormatPropertyCount);

   vk_outarray_append_typed(VkVideoFormatPropertiesKHR, &out, p)
   {
      p->format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
      p->imageType = VK_IMAGE_TYPE_2D;
      p->imageTiling = VK_IMAGE_TILING_OPTIMAL;
      p->imageUsageFlags = pVideoFormatInfo->imageUsage;
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetVideoSessionMemoryRequirementsKHR(VkDevice _device, VkVideoSessionKHR videoSession,
                                          uint32_t *pMemoryRequirementsCount,
                                          VkVideoSessionMemoryRequirementsKHR *pMemoryRequirements)
{
//   VK_FROM_HANDLE(nvk_device, dev, _device);
//   VK_FROM_HANDLE(nvk_video_session, vid, videoSession);
   uint32_t memory_type_bits = (1u << 2) - 1;
   VK_OUTARRAY_MAKE_TYPED(VkVideoSessionMemoryRequirementsKHR, out, pMemoryRequirements, pMemoryRequirementsCount);

   vk_outarray_append_typed(VkVideoSessionMemoryRequirementsKHR, &out, m) {
      m->memoryBindIndex = 0;
      m->memoryRequirements.size = 16384;
      m->memoryRequirements.alignment = 4096;
      m->memoryRequirements.memoryTypeBits = memory_type_bits;
   }
   vk_outarray_append_typed(VkVideoSessionMemoryRequirementsKHR, &out, m) {
      m->memoryBindIndex = 1;
      m->memoryRequirements.size = 122880;
      m->memoryRequirements.alignment = 4096;
      m->memoryRequirements.memoryTypeBits = memory_type_bits;
   }
   vk_outarray_append_typed(VkVideoSessionMemoryRequirementsKHR, &out, m) {
      m->memoryBindIndex = 2;
      m->memoryRequirements.size = 12288;
      m->memoryRequirements.alignment = 4096;
      m->memoryRequirements.memoryTypeBits = memory_type_bits;
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_UpdateVideoSessionParametersKHR(VkDevice _device, VkVideoSessionParametersKHR videoSessionParameters,
                                     const VkVideoSessionParametersUpdateInfoKHR *pUpdateInfo)
{
   VK_FROM_HANDLE(nvk_video_session_params, params, videoSessionParameters);
   return vk_video_session_parameters_update(&params->vk, pUpdateInfo);
}

static void
copy_bind(struct nvk_vid_mem *dst, const VkBindVideoSessionMemoryInfoKHR *src)
{
   dst->mem = nvk_device_memory_from_handle(src->memory);
   dst->offset = src->memoryOffset;
   dst->size = src->memorySize;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BindVideoSessionMemoryKHR(VkDevice _device, VkVideoSessionKHR videoSession, uint32_t videoSessionBindMemoryCount,
                               const VkBindVideoSessionMemoryInfoKHR *pBindSessionMemoryInfos)
{
   VK_FROM_HANDLE(nvk_video_session, vid, videoSession);

   for (unsigned i = 0; i < videoSessionBindMemoryCount; i++) {
      copy_bind(&vid->mems[pBindSessionMemoryInfos[i].memoryBindIndex], &pBindSessionMemoryInfos[i]);
   }
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBeginVideoCodingKHR(VkCommandBuffer commandBuffer, const VkVideoBeginCodingInfoKHR *pBeginInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   VK_FROM_HANDLE(nvk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(nvk_video_session, vid, pBeginInfo->videoSession);
   VK_FROM_HANDLE(nvk_video_session_params, params, pBeginInfo->videoSessionParameters);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);

   cmd_buffer->video.vid = vid;
   cmd_buffer->video.params = params;

   struct nv_push *p = nvk_cmd_buffer_push(cmd, 6);

   /* assign nvdec to sub channel 4 */
   __push_mthd(p, SUBC_NVC5B0, NV906F_SET_OBJECT);
   P_NV906F_SET_OBJECT(p, { .nvclass = dev->nvkmd->pdev->dev_info.cls_video,
                            .engine = 0 });


}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdControlVideoCodingKHR(VkCommandBuffer commandBuffer, const VkVideoCodingControlInfoKHR *pCodingControlInfo)
{

}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdEndVideoCodingKHR(VkCommandBuffer commandBuffer, const VkVideoEndCodingInfoKHR *pEndCodingInfo)
{

}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdDecodeVideoKHR(VkCommandBuffer commandBuffer, const VkVideoDecodeInfoKHR *frame_info)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_buffer, src_buffer, frame_info->srcBuffer);
   struct nvk_video_session *vid = cmd->video.vid;
   struct nvk_video_session_params *params = cmd->video.params;
   struct nvk_image_view *dst_iv = nvk_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);
   struct nvk_image *dst_img = (struct nvk_image *)dst_iv->vk.image;
   nvdec_h264_pic_s *nvh264;
   uint64_t *slice_offsets;
   uint64_t *mbstatus;
   VkResult status;
   uint64_t pic_gpu_addr, slice_offsets_address, mbstatus_address;
   const struct VkVideoDecodeH264PictureInfoKHR *h264_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_H264_PICTURE_INFO_KHR);

   const StdVideoH264SequenceParameterSet *sps =
      vk_video_find_h264_dec_std_sps(&params->vk, h264_pic_info->pStdPictureInfo->seq_parameter_set_id);
   const StdVideoH264PictureParameterSet *pps =
      vk_video_find_h264_dec_std_pps(&params->vk, h264_pic_info->pStdPictureInfo->pic_parameter_set_id);
   status = nvk_cmd_buffer_upload_alloc(cmd, sizeof(nvdec_h264_pic_s), 256,
                                        &pic_gpu_addr, (void **)&nvh264);
   assert(status == VK_SUCCESS);

   status = nvk_cmd_buffer_upload_alloc(cmd, 32, 256,
                                        &slice_offsets_address, (void **)&slice_offsets);
   assert(status == VK_SUCCESS);

   status = nvk_cmd_buffer_upload_alloc(cmd, 4096, 4096,
                                        &mbstatus_address, (void **)&mbstatus);

   memset(nvh264, 0, sizeof(nvdec_h264_pic_s));

   nvh264->slice_count = 1;
   nvh264->stream_len = frame_info->srcBufferRange;
   nvh264->mbhist_buffer_size = 4096;
   nvh264->log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;
   nvh264->delta_pic_order_always_zero_flag = sps->flags.delta_pic_order_always_zero_flag;
   nvh264->frame_mbs_only_flag = sps->flags.frame_mbs_only_flag;
   nvh264->PicWidthInMbs = sps->pic_width_in_mbs_minus1 + 1;
   nvh264->FrameHeightInMbs = sps->pic_height_in_map_units_minus1 + 1;

   nvh264->tileFormat = 0;
   nvh264->gob_height = 3;

   nvh264->entropy_coding_mode_flag = pps->flags.entropy_coding_mode_flag;
   nvh264->pic_order_present_flag = pps->flags.bottom_field_pic_order_in_frame_present_flag;
   nvh264->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
   nvh264->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;
   nvh264->deblocking_filter_control_present_flag = pps->flags.deblocking_filter_control_present_flag;
   nvh264->redundant_pic_cnt_present_flag = pps->flags.redundant_pic_cnt_present_flag;
   nvh264->transform_8x8_mode_flag = pps->flags.transform_8x8_mode_flag;
   nvh264->pitch_luma = dst_img->planes[0].nil.levels[0].row_stride_B;
   nvh264->pitch_chroma = dst_img->planes[1].nil.levels[0].row_stride_B;
   nvh264->luma_top_offset = 0;
   nvh264->luma_bot_offset = 0;
   nvh264->luma_frame_offset = 0;
   nvh264->chroma_top_offset = 0;
   nvh264->chroma_bot_offset = 0;
   nvh264->chroma_frame_offset = 0;

   nvh264->HistBufferSize = 0;

   nvh264->MbaffFrameFlag = sps->flags.mb_adaptive_frame_field_flag;
   nvh264->direct_8x8_inference_flag = sps->flags.direct_8x8_inference_flag;
   nvh264->weighted_pred_flag = pps->flags.weighted_pred_flag;
   nvh264->constrained_intra_pred_flag = pps->flags.constrained_intra_pred_flag;
   nvh264->ref_pic_flag = h264_pic_info->pStdPictureInfo->flags.is_reference;
   nvh264->field_pic_flag = h264_pic_info->pStdPictureInfo->flags.field_pic_flag;
   nvh264->bottom_field_flag = h264_pic_info->pStdPictureInfo->flags.bottom_field_flag;
   nvh264->second_field = h264_pic_info->pStdPictureInfo->flags.complementary_field_pair;
   nvh264->log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4;
   nvh264->chroma_format_idc = sps->chroma_format_idc;
   nvh264->pic_order_cnt_type = sps->pic_order_cnt_type;
   nvh264->pic_init_qp_minus26 = pps->pic_init_qp_minus26;
   nvh264->chroma_qp_index_offset = pps->chroma_qp_index_offset;
   nvh264->second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;

   nvh264->weighted_bipred_idc = pps->weighted_bipred_idc;
   nvh264->CurrPicIdx = frame_info->pSetupReferenceSlot->slotIndex;
   nvh264->CurrColIdx = frame_info->pSetupReferenceSlot->slotIndex;
   nvh264->frame_num = h264_pic_info->pStdPictureInfo->frame_num;
   nvh264->frame_surfaces = 0;
   nvh264->output_memory_layout = 0;

   nvh264->CurrFieldOrderCnt[0] = h264_pic_info->pStdPictureInfo->PicOrderCnt[0];
   nvh264->CurrFieldOrderCnt[1] = h264_pic_info->pStdPictureInfo->PicOrderCnt[1];

   memset(nvh264->WeightScale, 0x10, 6*4*4);
   memset(nvh264->WeightScale8x8, 0x10, 2*8*8);
   memset(slice_offsets, 0, 64);

   uint64_t luma_base[17] = { 0 };
   uint64_t chroma_base[17] = { 0 };

   /* DPB */
   for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
      int idx = frame_info->pReferenceSlots[i].slotIndex;
      const struct VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot =
         vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
      struct nvk_image_view *f_dpb_iv =
         nvk_image_view_from_handle(frame_info->pReferenceSlots[i].pPictureResource->imageViewBinding);
      struct nvk_image *dpb_img = (struct nvk_image *)f_dpb_iv->vk.image;

      nvh264->dpb[i].index = idx;
      nvh264->dpb[i].col_idx = idx;
      nvh264->dpb[i].FieldOrderCnt[0] = dpb_slot->pStdReferenceInfo->PicOrderCnt[0];
      nvh264->dpb[i].FieldOrderCnt[1] = dpb_slot->pStdReferenceInfo->PicOrderCnt[1];
      nvh264->dpb[i].FrameIdx = dpb_slot->pStdReferenceInfo->FrameNum;

      nvh264->dpb[i].is_long_term = dpb_slot->pStdReferenceInfo->flags.used_for_long_term_reference;
      nvh264->dpb[i].not_existing = dpb_slot->pStdReferenceInfo->flags.is_non_existing;
      luma_base[idx] = nvk_image_base_address(dpb_img, 0) >> 8;
      chroma_base[idx] = nvk_image_base_address(dpb_img, 1) >> 8;
   }

   /* weights scale, scale 8x8 - raster scan */

   nvh264->lossless_ipred8x8_filter_enable = 0;
   nvh264->qpprime_y_zero_transform_bypass_flag = 0;

   int slot_idx = frame_info->pSetupReferenceSlot->slotIndex;
   luma_base[slot_idx] = nvk_image_base_address(dst_img, 0) >> 8;
   chroma_base[slot_idx] = nvk_image_base_address(dst_img, 1) >> 8;
   uint64_t mem0_addr = (vid->mems[0].mem->mem->va->addr + vid->mems[0].offset) >> 8;
   uint64_t mem1_addr = (vid->mems[1].mem->mem->va->addr + vid->mems[1].offset) >> 8;
   uint64_t mem2_addr = (vid->mems[2].mem->mem->va->addr + vid->mems[2].offset) >> 8;
   uint64_t src_address = nvk_buffer_address(src_buffer, frame_info->srcBufferOffset);
   struct nv_push *p = nvk_cmd_buffer_push(cmd, 64);
   /* display param */
   /* SET DRV PIC SETUP OFFSET */
   P_MTHD(p, NVC5B0, SET_APPLICATION_ID);
   P_NVC5B0_SET_APPLICATION_ID(p, ID_H264);

   P_MTHD(p, NVC5B0, SET_CONTROL_PARAMS);
   P_NVC5B0_SET_CONTROL_PARAMS(p, { .codec_type = CODEC_TYPE_H264,
                                    .gptimer_on = 1,
                                    .err_conceal_on = 1,
                                    .mbtimer_on = 1
      });

   P_MTHD(p, NVC5B0, SET_DRV_PIC_SETUP_OFFSET);
   P_NVC5B0_SET_DRV_PIC_SETUP_OFFSET(p, pic_gpu_addr >> 8);
   P_NVC5B0_SET_IN_BUF_BASE_OFFSET(p, src_address >> 8);
   P_NVC5B0_SET_PICTURE_INDEX(p, slot_idx);
   P_NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET(p, slice_offsets_address >> 8);
   P_NVC5B0_SET_COLOC_DATA_OFFSET(p, mem0_addr);
   P_NVC5B0_SET_HISTORY_OFFSET(p, mem2_addr);
//   P_NVC5B0_SET_DISPLAY_BUF_SIZE(p, 0);
//   P_NVC5B0_SET_HISTOGRAM_OFFSET(p, 0);

   P_MTHD(p, NVC5B0, SET_NVDEC_STATUS_OFFSET);
   P_NVC5B0_SET_NVDEC_STATUS_OFFSET(p, mbstatus_address >> 8);

//   P_NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET(p, 0);//luma_addr >> 8);
//   P_NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET(p, 0);//chroma_addr >> 8);
   P_MTHD(p, NVC5B0, SET_PICTURE_LUMA_OFFSET0);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET0(p, luma_base[0]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET1(p, luma_base[1]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET2(p, luma_base[2]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET3(p, luma_base[3]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET4(p, luma_base[4]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET5(p, luma_base[5]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET6(p, luma_base[6]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET7(p, luma_base[7]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET8(p, luma_base[8]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET9(p, luma_base[9]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET10(p, luma_base[10]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET11(p, luma_base[11]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET12(p, luma_base[12]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET13(p, luma_base[13]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET14(p, luma_base[14]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET15(p, luma_base[15]);
   P_NVC5B0_SET_PICTURE_LUMA_OFFSET16(p, luma_base[16]);

   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET0(p, chroma_base[0]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET1(p, chroma_base[1]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET2(p, chroma_base[2]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET3(p, chroma_base[3]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET4(p, chroma_base[4]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET5(p, chroma_base[5]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET6(p, chroma_base[6]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET7(p, chroma_base[7]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET8(p, chroma_base[8]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET9(p, chroma_base[9]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET10(p, chroma_base[10]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET11(p, chroma_base[11]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET12(p, chroma_base[12]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET13(p, chroma_base[13]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET14(p, chroma_base[14]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET15(p, chroma_base[15]);
   P_NVC5B0_SET_PICTURE_CHROMA_OFFSET16(p, chroma_base[16]);

   P_MTHD(p, NVC5B0, H264_SET_MBHIST_BUF_OFFSET);
   P_NVC5B0_H264_SET_MBHIST_BUF_OFFSET(p, mem1_addr);

   P_MTHD(p, NVC5B0, EXECUTE);
   P_NVC5B0_EXECUTE(p, { .notify = NOTIFY_DISABLE,
                         .notify_on = NOTIFY_ON_END,
                         .awaken = AWAKEN_DISABLE });
   /* EXECUTE */
}
