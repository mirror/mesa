/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2024 Valve Corporation
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TU_DEVICE_GENERATED_COMMANDS_H
#define TU_DEVICE_GENERATED_COMMANDS_H

#include "vk_object.h"

#include "tu_common.h"
#include "tu_cs.h"

#include "dgc/tu_dgc.h"

#define TU_DGC_MAX_PIPELINES (MAX_STORAGE_BUFFER_RANGE / TU_DGC_PIPELINE_SIZE)

struct tu_dgc_cs {
   struct tu_cs cs;
   struct tu_cs patchpoint_cs;
   unsigned patchpoint_count;
   unsigned idx;
};

struct tu_indirect_command_layout {
   struct vk_object_base base;

   VkIndirectCommandsLayoutUsageFlagsEXT flags;
   VkPipelineBindPoint pipeline_bind_point;

   uint32_t input_stride;

   uint32_t pipeline_offset;

   bool dispatch;
   bool draw_indexed;
   bool draw_indirect_count;

   bool tess;

   bool bind_pipeline;

   bool bind_index_buffer;

   bool emit_push_constants;

   uint32_t bind_vbo_mask;

   uint32_t push_constant_size;

   int main_cs_idx;
   int user_consts_cs_idx;
   int vertex_buffer_idx;
   int vertex_buffer_stride_idx;

   struct tu_cs cs;
   struct tu_cs patchpoint_cs;

   struct tu_draw_state buffers[TU_DGC_MAX_BUFFERS];
   struct tu_draw_state patchpoints[TU_DGC_MAX_BUFFERS];
   unsigned buffer_count;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(tu_indirect_command_layout, base, VkIndirectCommandsLayoutEXT,
                               VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT)

struct tu_indirect_execution_set {
   struct vk_object_base base;

   uint32_t pipeline_count;

   struct tu_pipeline *pipelines[];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(tu_indirect_execution_set, base, VkIndirectExecutionSetEXT,
                               VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT)

void
tu_dgc_begin(struct tu_cmd_buffer *cmd,
             const VkGeneratedCommandsInfoEXT *info);

void
tu_dgc_end(struct tu_cmd_buffer *cmd,
           const VkGeneratedCommandsInfoEXT *info);

template<chip CHIP>
void
tu_preprocess(struct tu_cmd_buffer *cmd,
              struct tu_cmd_buffer *state_cmd,
              const VkGeneratedCommandsInfoEXT *info);

#endif
