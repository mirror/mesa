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

#include "nvidia/nvdec_drv.h"
#include "video/video.h"

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

   nvk_video_create_video_session(vid);

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

   nvk_video_destroy_video_session(vid);
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
   struct VkVideoDecodeCapabilitiesKHR *dec_caps =
      (struct VkVideoDecodeCapabilitiesKHR *)vk_find_struct(pCapabilities->pNext, VIDEO_DECODE_CAPABILITIES_KHR);

   switch (pVideoProfile->videoCodecOperation) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: {
      pCapabilities->flags = 0;
      pCapabilities->minBitstreamBufferOffsetAlignment = 256;
      pCapabilities->minBitstreamBufferSizeAlignment = 256;
      pCapabilities->pictureAccessGranularity.width = VK_VIDEO_H264_MACROBLOCK_WIDTH;
      pCapabilities->pictureAccessGranularity.height = VK_VIDEO_H264_MACROBLOCK_HEIGHT;
      pCapabilities->minCodedExtent.width = 48;
      pCapabilities->minCodedExtent.height = VK_VIDEO_H264_MACROBLOCK_HEIGHT;
      pCapabilities->maxCodedExtent.width = 4096;
      pCapabilities->maxCodedExtent.height = 4096;

      /* H264 allows different luma and chroma bit depths */
      if (pVideoProfile->lumaBitDepth != pVideoProfile->chromaBitDepth)
         return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;

      if (pVideoProfile->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)
         return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;

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
      ext->maxLevelIdc = STD_VIDEO_H264_LEVEL_IDC_5_2;
      strcpy(pCapabilities->stdHeaderVersion.extensionName, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME);
      pCapabilities->stdHeaderVersion.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION;
      break;
   }

   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR: {
      /*
      * Anything other than 8 bits is not supported by the driver for now.
      *
      * Basically, we'd have to advertise
      * VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR and
      * VK_FORMAT_FEATURE_VIDEO_DECODE_DPB_BIT_KHR for some 10bit YUV format.
      *
      * The GStreamer support is also not there currently.
      */
      if (pVideoProfile->lumaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR ||
         pVideoProfile->chromaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR)
         return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;

      pCapabilities->flags = 0;
      pCapabilities->minBitstreamBufferOffsetAlignment = 256;
      pCapabilities->minBitstreamBufferSizeAlignment = 256;
      pCapabilities->pictureAccessGranularity.width = VK_VIDEO_H265_CTU_MAX_WIDTH;
      pCapabilities->pictureAccessGranularity.height = VK_VIDEO_H265_CTU_MAX_HEIGHT;
      pCapabilities->minCodedExtent.width = 144;
      pCapabilities->minCodedExtent.height = 144;
      pCapabilities->maxCodedExtent.width = 8192;
      pCapabilities->maxCodedExtent.height = 8192;

      struct VkVideoDecodeH265CapabilitiesKHR *ext = (struct VkVideoDecodeH265CapabilitiesKHR *)vk_find_struct(
         pCapabilities->pNext, VIDEO_DECODE_H265_CAPABILITIES_KHR);
      const struct VkVideoDecodeH265ProfileInfoKHR *h265_profile =
         vk_find_struct_const(pVideoProfile->pNext, VIDEO_DECODE_H265_PROFILE_INFO_KHR);

      ext->maxLevelIdc = STD_VIDEO_H265_LEVEL_IDC_5_1;

      /*
       * XXX: the hw supports MAIN10 and MAIN12, but 10bit is not supported by
       * the driver now, and MAIN12 is not in the vulkan video spec
       */
      if (h265_profile->stdProfileIdc != STD_VIDEO_H265_PROFILE_IDC_MAIN)
         return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;

      /*
       * XXX: The hw does support both 420 and 444 (not 422, though).
       */
      if (pVideoProfile->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)
         return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;

      pCapabilities->flags = VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR;
      pCapabilities->maxDpbSlots = 17;
      pCapabilities->maxActiveReferencePictures = 16;

      strcpy(pCapabilities->stdHeaderVersion.extensionName, VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME);
      pCapabilities->stdHeaderVersion.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION;
      break;
   }

   default:
      return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
   }

   if (dec_caps)
      dec_caps->flags = VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;

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
   VK_FROM_HANDLE(nvk_video_session, vid, videoSession);
   uint32_t memory_type_bits = memory_type_bits = (1u << 2) - 1;
   VK_OUTARRAY_MAKE_TYPED(VkVideoSessionMemoryRequirementsKHR, out, pMemoryRequirements, pMemoryRequirementsCount);

   switch (vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: {
      size_t max_width_in_mb = vid->vk.max_coded.width / 16;
      size_t max_height_in_mb = vid->vk.max_coded.height / 16;
      size_t coloc_size   = align(align(max_height_in_mb, 2) * (max_width_in_mb * 64) - 63, 0x100);
      coloc_size  *= vid->vk.max_active_ref_pics + 1; /* Max number of references frames, plus current frame */
      size_t mbhist_size  = align(max_width_in_mb * 104, 0x100);
      size_t history_size = align(max_width_in_mb * 0x300, 0x200);

      vk_outarray_append_typed(VkVideoSessionMemoryRequirementsKHR, &out, m) {
         m->memoryBindIndex = 0;
         m->memoryRequirements.size = coloc_size;
         m->memoryRequirements.alignment = 256;
         m->memoryRequirements.memoryTypeBits = memory_type_bits;
      }
      vk_outarray_append_typed(VkVideoSessionMemoryRequirementsKHR, &out, m) {
         m->memoryBindIndex = 1;
         m->memoryRequirements.size = mbhist_size;
         m->memoryRequirements.alignment = 256;
         m->memoryRequirements.memoryTypeBits = memory_type_bits;
      }
      vk_outarray_append_typed(VkVideoSessionMemoryRequirementsKHR, &out, m) {
         m->memoryBindIndex = 2;
         m->memoryRequirements.size = history_size;
         m->memoryRequirements.alignment = 256;
         m->memoryRequirements.memoryTypeBits = memory_type_bits;
      }
      break;
   }

   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR: {
      const int CTU_SIZE = 64;
      const int MB_SIZE = 16;

      const int aligned_w = align(vid->vk.max_coded.width, CTU_SIZE);
      const int aligned_h = align(vid->vk.max_coded.height, CTU_SIZE);

      size_t coloc_size = (aligned_w * aligned_h) + (aligned_w * aligned_h / MB_SIZE);
      size_t filter_size = 624; /* 608 from the nvdec_drv.h macros, but apparently 624 is expected */
      size_t colmv_size = aligned_w * aligned_h / MB_SIZE;
      size_t sao_size = 4864; /* reverse engineered */
      size_t bsd_size = 60; /* taken from tegra */

      vk_outarray_append_typed(VkVideoSessionMemoryRequirementsKHR, &out, m) {
         m->memoryBindIndex = 0;
         m->memoryRequirements.size = coloc_size;
         m->memoryRequirements.alignment = 256;
         m->memoryRequirements.memoryTypeBits = memory_type_bits;
      }
      vk_outarray_append_typed(VkVideoSessionMemoryRequirementsKHR, &out, m) {
         m->memoryBindIndex = 1;
         m->memoryRequirements.size = (filter_size + sao_size + bsd_size) * aligned_h;
         m->memoryRequirements.alignment = 256;
         m->memoryRequirements.memoryTypeBits = memory_type_bits;
      }
      vk_outarray_append_typed(VkVideoSessionMemoryRequirementsKHR, &out, m) {
         m->memoryBindIndex = 2;
         m->memoryRequirements.size = colmv_size;
         m->memoryRequirements.alignment = 256;
         m->memoryRequirements.memoryTypeBits = memory_type_bits;
      }

      break;
   }

   default:
      return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
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

   cmd_buffer->video.vid = vid;
   cmd_buffer->video.params = params;


   nvk_video_cmd_begin_video_coding_khr(cmd, pBeginInfo);
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
   struct nvk_image_view *dst_iv = nvk_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);

   nvk_video_cmd_decode_video_khr(cmd, frame_info, src_buffer, dst_iv);
}
