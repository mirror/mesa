/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_VIDEO_H
#define NVK_VIDEO_H 1

#include "nvk_private.h"
#include "nvk_device_memory.h"

#include "vulkan/runtime/vk_video.h"

struct nvk_vid_mem {
   struct nvk_device_memory *mem;
   VkDeviceSize offset;
   VkDeviceSize size;
};

struct nvk_video_session {
   /** The parent object */
   struct vk_video_session vk;
   /** Opaque memory objects needed by the GPU.
    *
    * We must ensure they're allocated and that the size is correctly computed
    * from codec parameters.
    *
    * Not all memories are bound for all codecs.
    */
   struct nvk_vid_mem mems[5];
   /** Opaque pointer to data managed by the Rust side. */
   void *rust;
};

struct nvk_video_session_params {
   struct vk_video_session_parameters vk;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_video_session, vk.base, VkVideoSessionKHR, VK_OBJECT_TYPE_VIDEO_SESSION_KHR)
VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_video_session_params, vk.base, VkVideoSessionParametersKHR,
                               VK_OBJECT_TYPE_VIDEO_SESSION_PARAMETERS_KHR)
#endif
