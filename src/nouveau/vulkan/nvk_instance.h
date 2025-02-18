/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_INSTANCE_H
#define NVK_INSTANCE_H 1

#include "nvk_private.h"

#include "nvk_debug.h"
#include "vk_instance.h"
#include "util/xmlconfig.h"

struct nvk_instance {
   struct vk_instance vk;

   enum nvk_debug debug_flags;

   struct driOptionCache dri_options;
   struct driOptionCache available_dri_options;

   uint8_t driver_build_sha[20];
   uint32_t force_vk_vendor;
   bool ssbo_align_4b;
};

VK_DEFINE_HANDLE_CASTS(nvk_instance, vk.base, VkInstance, VK_OBJECT_TYPE_INSTANCE)

static inline uint32_t
nvk_min_ssbo_alignment(const struct nvk_instance *instance)
{
   return instance->ssbo_align_4b ? 4 : 16;
}

#endif
