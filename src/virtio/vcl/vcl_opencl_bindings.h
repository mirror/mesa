/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#include <CL/cl_icd.h>
#include <stddef.h>

#define DECL_CL_STRUCT(name) struct name { const cl_icd_dispatch *dispatch; }
DECL_CL_STRUCT(_cl_command_queue);
DECL_CL_STRUCT(_cl_context);
DECL_CL_STRUCT(_cl_device_id);
DECL_CL_STRUCT(_cl_event);
DECL_CL_STRUCT(_cl_kernel);
DECL_CL_STRUCT(_cl_mem);
DECL_CL_STRUCT(_cl_platform_id);
DECL_CL_STRUCT(_cl_program);
DECL_CL_STRUCT(_cl_sampler);
#undef DECL_CL_STRUCT

#define CL_DRM_DEVICE_FAILED_MESA -10000
#define CL_VIRTGPU_IOCTL_FAILED_MESA -10001
#define CL_VIRTGPU_PARAM_FAILED_MESA -10002
#define CL_VIRTGPU_MAP_FAILED_MESA -10003
#define CL_VIRTGPU_NOT_FOUND_MESA -10004

typedef struct cl_image_desc_MESA
{
   cl_mem_object_type image_type;
   size_t image_width;
   size_t image_height;
   size_t image_depth;
   size_t image_array_size;
   size_t image_row_pitch;
   size_t image_slice_pitch;
   cl_uint num_mip_levels;
   cl_uint num_samples;
   cl_mem mem_object;
} cl_image_desc_MESA;
