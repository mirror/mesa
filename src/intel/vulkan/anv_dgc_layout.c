/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdbool.h>

#include "genxml/genX_bits.h"
#include "shaders/libintel_shaders.h"

#include "anv_private.h"

#define sizeof_field(type, field) sizeof(((type *)0)->field)

static gl_shader_stage
vk_stage_to_mesa_stage(VkShaderStageFlags stage)
{
   switch (stage) {
   case VK_SHADER_STAGE_VERTEX_BIT:
      return MESA_SHADER_VERTEX;
   case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
      return MESA_SHADER_TESS_CTRL;
   case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
      return MESA_SHADER_TESS_EVAL;
   case VK_SHADER_STAGE_GEOMETRY_BIT:
      return MESA_SHADER_GEOMETRY;
   case VK_SHADER_STAGE_FRAGMENT_BIT:
      return MESA_SHADER_FRAGMENT;
   case VK_SHADER_STAGE_COMPUTE_BIT:
      return MESA_SHADER_COMPUTE;
   case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
      return MESA_SHADER_RAYGEN;
   case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
      return MESA_SHADER_ANY_HIT;
   case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
      return MESA_SHADER_CLOSEST_HIT;
   case VK_SHADER_STAGE_MISS_BIT_KHR:
      return MESA_SHADER_MISS;
   case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
      return MESA_SHADER_INTERSECTION;
   case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
      return MESA_SHADER_CALLABLE;
   case VK_SHADER_STAGE_TASK_BIT_EXT:
      return MESA_SHADER_TASK;
   case VK_SHADER_STAGE_MESH_BIT_EXT:
      return MESA_SHADER_MESH;
   default:
      unreachable("unsupported stage");
   }
}

static uint32_t
indirect_legacy_gfx_final_cmd_size(const struct intel_device_info *devinfo)
{
   /* URB is dynamic if Wa_16014912113 is enabled */
   const uint32_t urb_stage_dwords = devinfo->ver >= 12 ?
      _3DSTATE_URB_ALLOC_VS_length(devinfo):
      _3DSTATE_URB_VS_length(devinfo);
   const uint32_t urb_dwords =
      intel_needs_workaround(devinfo, 16014912113) ? 0 :
      (4 /* VS,HS,DS,GS */ * urb_stage_dwords +
       _3DSTATE_URB_ALLOC_TASK_length(devinfo) +
       _3DSTATE_URB_ALLOC_MESH_length(devinfo));
   const uint32_t push_dwords =
      5 /* VS,HS,DS,GS,PS */ * _3DSTATE_PUSH_CONSTANT_ALLOC_VS_length(devinfo) +
      ((intel_needs_workaround(devinfo, 18022330953) ||
        intel_needs_workaround(devinfo, 22011440098)) ?
       _3DSTATE_CONSTANT_ALL_length(devinfo) : 0);
   const uint32_t legacy_dwords =
      _3DSTATE_VF_SGVS_length(devinfo) +
      _3DSTATE_VF_SGVS_2_length(devinfo) +
      _3DSTATE_VS_length(devinfo) +
      _3DSTATE_HS_length(devinfo) +
      _3DSTATE_DS_length(devinfo) +
      (3 + 4 * 2) /* 3DSTATE_SO_DECL_LIST */;
   const uint32_t common_dwords =
      _3DSTATE_PRIMITIVE_REPLICATION_length(devinfo) +
      _3DSTATE_SBE_length(devinfo) +
      _3DSTATE_SBE_SWIZ_length(devinfo);

   return 4 * (urb_dwords + push_dwords + common_dwords + legacy_dwords);
}

static uint32_t
indirect_mesh_gfx_final_cmd_size(const struct intel_device_info *devinfo)
{
   const uint32_t urb_stage_dwords = devinfo->ver >= 12 ?
      _3DSTATE_URB_ALLOC_VS_length(devinfo):
      _3DSTATE_URB_VS_length(devinfo);
   const uint32_t urb_dwords =
      intel_needs_workaround(devinfo, 16014912113) ? 0 :
      (4 /* VS,HS,DS,GS */ * urb_stage_dwords +
       _3DSTATE_URB_ALLOC_TASK_length(devinfo) +
       _3DSTATE_URB_ALLOC_MESH_length(devinfo));
   const uint32_t push_dwords =
      4 * _3DSTATE_PUSH_CONSTANT_ALLOC_VS_length(devinfo) +
      ((intel_needs_workaround(devinfo, 18022330953) ||
        intel_needs_workaround(devinfo, 22011440098)) ?
       _3DSTATE_CONSTANT_ALL_length(devinfo) : 0);
   const uint32_t mesh_dwords =
      _3DSTATE_PRIMITIVE_REPLICATION_length(devinfo) +
      _3DSTATE_TASK_CONTROL_length(devinfo) +
      _3DSTATE_TASK_SHADER_length(devinfo) +
      _3DSTATE_TASK_REDISTRIB_length(devinfo) +
      _3DSTATE_MESH_CONTROL_length(devinfo) +
      _3DSTATE_MESH_SHADER_length(devinfo) +
      _3DSTATE_MESH_DISTRIB_length(devinfo) +
      _3DSTATE_CLIP_MESH_length(devinfo) +
      _3DSTATE_SBE_length(devinfo) +
      _3DSTATE_SBE_SWIZ_length(devinfo) +
      _3DSTATE_SBE_MESH_length(devinfo);

   return 4 * (urb_dwords + push_dwords + mesh_dwords);
}

static uint32_t
indirect_partial_gfx_cmd_size(const struct intel_device_info *devinfo,
                              bool mesh)
{
    const uint32_t urb_stage_dwords = devinfo->ver >= 12 ?
      _3DSTATE_URB_ALLOC_VS_length(devinfo):
      _3DSTATE_URB_VS_length(devinfo);
  const uint32_t wa_16014912113_dwords =
      intel_needs_workaround(devinfo, 16014912113) ?
      (4 /* VS,HS,DS,GS */ * urb_stage_dwords * 2 +
       PIPE_CONTROL_length(devinfo)) : 0;
   const uint32_t partial_cmds_dwords =
      _3DSTATE_VFG_length(devinfo) +
      (mesh ? 0 :
       (_3DSTATE_VF_TOPOLOGY_length(devinfo) +
        _3DSTATE_TE_length(devinfo) +
        _3DSTATE_GS_length(devinfo) +
        _3DSTATE_STREAMOUT_length(devinfo))) +
      _3DSTATE_CLIP_length(devinfo) +
      _3DSTATE_SF_length(devinfo) +
      _3DSTATE_RASTER_length(devinfo) +
      _3DSTATE_WM_length(devinfo) +
      _3DSTATE_PS_length(devinfo) +
      _3DSTATE_PS_EXTRA_length(devinfo) +
      _3DSTATE_PS_BLEND_length(devinfo) +
      (intel_needs_workaround(devinfo, 14018283232) ?
       RESOURCE_BARRIER_length(devinfo) : 0);

   return 4 * (wa_16014912113_dwords + partial_cmds_dwords);
}

static uint32_t
draw_cmd_size(const struct intel_device_info *devinfo,
              const struct vk_indirect_command_layout *vk_layout)
{
   return 4 *
      ((vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH)) ?
       _3DMESH_3D_length(devinfo) :
       devinfo->ver >= 11 ? _3DPRIMITIVE_EXTENDED_length(devinfo) :
       _3DPRIMITIVE_length(devinfo));

}

static void
layout_add_command(struct anv_indirect_command_layout *layout, uint32_t size,
                   const char *name)
{
   layout->cmd_size = align(layout->cmd_size, 4);
   layout->cmd_size += size;

   layout->items[layout->n_items++] = (struct anv_indirect_command_layout_item) {
      .name = name,
      .size = size,
   };
}

static void
layout_add_data(struct anv_indirect_command_layout *layout,
                uint32_t size, uint32_t alignment,
                uint16_t *out_data_offset)
{
   layout->data_size = align(layout->data_size, alignment);
   if (out_data_offset)
      *out_data_offset = layout->data_size;
   layout->data_size += size;
}

static void
push_layout_add_range(struct anv_gen_push_layout *pc_layout,
                      const struct vk_indirect_command_push_constant_layout *vk_pc_layout)
{
   pc_layout->entries[pc_layout->num_entries++] = (struct anv_gen_push_entry) {
      .seq_offset  = vk_pc_layout->src_offset_B,
      .push_offset = vk_pc_layout->dst_offset_B,
      .size        = vk_pc_layout->size_B,
   };
}

static uint32_t
push_constant_command_size(const struct intel_device_info *devinfo,
                           VkShaderStageFlags stages,
                           uint32_t n_slots)
{
   uint32_t dwords = 0;
   anv_foreach_vk_stage(stage, stages) {
      switch (stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:
      case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
      case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
      case VK_SHADER_STAGE_GEOMETRY_BIT:
      case VK_SHADER_STAGE_FRAGMENT_BIT:
         if (devinfo->ver >= 12) {
            dwords += (_3DSTATE_CONSTANT_ALL_length(devinfo) +
                      n_slots * _3DSTATE_CONSTANT_ALL_DATA_length(devinfo));
         } else {
            dwords += _3DSTATE_CONSTANT_VS_length(devinfo);
         }
         break;
      case VK_SHADER_STAGE_MESH_BIT_EXT:
         dwords += _3DSTATE_MESH_SHADER_DATA_length(devinfo);
         break;
      case VK_SHADER_STAGE_TASK_BIT_EXT:
         dwords += _3DSTATE_TASK_SHADER_DATA_length(devinfo);
         break;
      default:
         unreachable("Invalid stage");
      }
   }
   return 4 * dwords;
}

VkResult anv_CreateIndirectCommandsLayoutEXT(
    VkDevice                                    _device,
    const VkIndirectCommandsLayoutCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkIndirectCommandsLayoutEXT*                pIndirectCommandsLayout)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   const struct intel_device_info *devinfo = device->info;
   struct anv_indirect_command_layout *layout_obj;

   layout_obj = vk_indirect_command_layout_create(
      &device->vk, pCreateInfo, pAllocator,
      sizeof(struct anv_indirect_command_layout));
   if (!layout_obj)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_indirect_command_layout *vk_layout = &layout_obj->vk;

   const bool is_gfx =
      (vk_layout->dgc_info &
       (BITFIELD_BIT(MESA_VK_DGC_DRAW) |
        BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED) |
        BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH))) != 0;

   struct anv_gen_gfx_layout *gfx_layout = &layout_obj->gfx_layout;
   struct anv_gen_cs_layout *cs_layout = &layout_obj->cs_layout;
   struct anv_gen_push_layout *pc_layout =
      is_gfx ? &gfx_layout->push_constants : &cs_layout->push_constants;

   STATIC_ASSERT(ANV_GENERATED_COMMAND_RT_GLOBAL_DISPATCH_SIZE ==
                 BRW_RT_PUSH_CONST_OFFSET);
   STATIC_ASSERT(ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE ==
                 MAX_PUSH_CONSTANTS_SIZE);
   STATIC_ASSERT(ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_WS_SIZE_OFFSET ==
                 offsetof(struct anv_push_constants, cs.num_work_groups));
   STATIC_ASSERT(ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_FS_MSAA_FLAGS_OFFSET ==
                 offsetof(struct anv_push_constants, gfx.fs_msaa_flags));
   STATIC_ASSERT(ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_PCP_OFFSET ==
                 offsetof(struct anv_push_constants, gfx.tcs_input_vertices));

   /* Keep this in sync with generate_commands.cl:write_prolog_epilog() */
   layout_obj->cmd_prolog_size = 4 *
      (MI_STORE_DATA_IMM_length(devinfo) + 1 +
       MI_BATCH_BUFFER_START_length(devinfo) +
       (devinfo->ver >= 12 ? MI_ARB_CHECK_length(devinfo) : 0));
   layout_obj->cmd_epilog_size = 4 * MI_BATCH_BUFFER_START_length(devinfo);

   /* On <= Gfx12.0 the gl_NumWorkGroups is located in the push constants so
    * we need push constant data per sequence.
    */
   const bool has_per_sequence_constants =
      /* Ray tracing dispatch have some per dispatch data in the push
       * constants (like tracing size)
       */
      (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_RT)) ||
      /* <= Gfx12.0 has per_thread data for local workgroup index computation
       */
      (devinfo->verx10 <= 120 &&
       (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DISPATCH))) ||
      /* Finally application updates of push constants or sequence index */
      (vk_layout->dgc_info & (BITFIELD_BIT(MESA_VK_DGC_PC) |
                              BITFIELD_BIT(MESA_VK_DGC_SI))) ||
      /* Gfx IES will potentially need per sequence driver push constants
       * (fs_msaa_flags, patch_control_points)
       */
      ((vk_layout->dgc_info & (BITFIELD_BIT(MESA_VK_DGC_DRAW) |
                               BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED) |
                               BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH))) &&
       (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)));

   if (has_per_sequence_constants) {
      /* RT & compute need a combined push constants and also Mesh. */
      uint32_t pc_size = sizeof(struct anv_push_constants);
      /* Prior to Gfx12.5+, there is no HW mechanism in the HW thread
       * generation to provide a workgroup local id. The way the workgroup
       * local id is provided is through a per-thread push constant mechanism
       * that read a per thread 32B (one GRF) piece of data in which the
       * driver writes the thread id.
       *
       * The maximum workgroup size is 1024. With a worse case dispatch size
       * of SIMD8, that means at max 128 HW threads, each needing a 32B for
       * its subgroup_id value within the workgroup. 32B * 128 = 4096B.
       */
      if (devinfo->verx10 < 125 &&
          (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DISPATCH)))
         pc_size += 4096;

      layout_add_data(layout_obj, pc_size, ANV_UBO_ALIGNMENT,
                      &pc_layout->data_offset);

      for (uint32_t i = 0; i < vk_layout->n_pc_layouts; i++) {
         const struct vk_indirect_command_push_constant_layout *vk_pc_layout =
            &vk_layout->pc_layouts[i];
         push_layout_add_range(pc_layout, vk_pc_layout);
      }

      if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_SI)) {
         pc_layout->seq_id_active = true;
         pc_layout->seq_id_offset = vk_layout->si_layout.dst_offset_B;
      }

      pc_layout->mocs =
         isl_mocs(&device->isl_dev, ISL_SURF_USAGE_CONSTANT_BUFFER_BIT, false);
   }

   /* Graphics */
   if (vk_layout->dgc_info & (BITFIELD_BIT(MESA_VK_DGC_DRAW) |
                              BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED) |
                              BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH))) {
      layout_obj->bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;

      /* 3DSTATE_INDEX_BUFFER */
      if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IB)) {
         layout_add_command(layout_obj,
                            _3DSTATE_INDEX_BUFFER_length(devinfo) * 4,
                            "index");
      }

      /* 3DSTATE_VERTEX_BUFFERS */
      if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_VB)) {
         layout_add_command(layout_obj,
                            (1 /* TODO: _3DSTATE_VERTEX_BUFFERS_length(devinfo) */ +
                             util_bitcount(vk_layout->vertex_bindings) *
                             VERTEX_BUFFER_STATE_length(devinfo)) * 4,
                            "vertex");
      }

      if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) {
         gfx_layout->indirect_set.active = true;
         if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH)) {
            layout_add_command(layout_obj,
                               indirect_mesh_gfx_final_cmd_size(devinfo) +
                               indirect_partial_gfx_cmd_size(devinfo, true),
                               "ies-mesh");
         } else {
            layout_add_command(layout_obj,
                               indirect_legacy_gfx_final_cmd_size(devinfo) +
                               indirect_partial_gfx_cmd_size(devinfo, false),
                               "ies-primitive");
         }
      } else {
         if ((vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH)) == 0) {
            if (intel_needs_workaround(device->info, 16011107343))
               layout_add_command(layout_obj, _3DSTATE_HS_length(devinfo) * 4, "hs");
            if (intel_needs_workaround(device->info, 22018402687))
               layout_add_command(layout_obj, _3DSTATE_DS_length(devinfo) * 4, "ds");
         }
      }

      const VkShaderStageFlags draw_stages =
         (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH)) ?
         (VK_SHADER_STAGE_TASK_BIT_EXT |
          VK_SHADER_STAGE_MESH_BIT_EXT |
          VK_SHADER_STAGE_FRAGMENT_BIT) :
         (VK_SHADER_STAGE_VERTEX_BIT |
          VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
          VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
          VK_SHADER_STAGE_GEOMETRY_BIT |
          VK_SHADER_STAGE_FRAGMENT_BIT);
      const bool need_push_constants =
         (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) != 0 ||
         (vk_layout->dgc_info & (BITFIELD_BIT(MESA_VK_DGC_PC) |
                                 BITFIELD_BIT(MESA_VK_DGC_SI))) != 0;

      /* 3DSTATE_CONSTANT_* */
      if (need_push_constants) {
         uint32_t cmd_size = push_constant_command_size(
            devinfo, draw_stages, 4);
         layout_add_command(layout_obj, cmd_size, "push-constants");
      }

      /* 3DPRIMITIVE / 3DMESH_3D */
      layout_add_command(layout_obj, draw_cmd_size(devinfo, vk_layout), "draw");
      gfx_layout->draw.seq_offset = vk_layout->draw_src_offset_B;
   }

   /* Compute */
   if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DISPATCH)) {
      layout_obj->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;

      if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) {
         cs_layout->indirect_set.active = true;
         cs_layout->indirect_set.seq_offset = vk_layout->ies_src_offset_B;
      }

      cs_layout->dispatch.seq_offset = vk_layout->dispatch_src_offset_B;
      if (devinfo->verx10 >= 125) {
         /* On Gfx12.5+ everything is in a single instruction */
         uint32_t cmd_size = COMPUTE_WALKER_length(devinfo) * 4;
         layout_add_command(layout_obj, cmd_size, "compute-walker");
      } else {
         /* Prior generations  */
         uint32_t cmd_size = 4 * (MEDIA_CURBE_LOAD_length(devinfo) +
                                  GPGPU_WALKER_length(devinfo) +
                                  MEDIA_STATE_FLUSH_length(devinfo));

         if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) {
            cmd_size += 4 * (MEDIA_VFE_STATE_length(devinfo) +
                             MEDIA_INTERFACE_DESCRIPTOR_LOAD_length(devinfo));
            layout_add_data(layout_obj,
                            INTERFACE_DESCRIPTOR_DATA_length(devinfo), 64,
                            &cs_layout->indirect_set.data_offset);
         }

         layout_add_command(layout_obj, cmd_size,
                            "media-curbe,gpgpu-walker,media-state");
      }
   }

   /* Ray-tracing */
   if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_RT)) {
      layout_obj->bind_point = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;

      uint32_t cmd_size = COMPUTE_WALKER_length(devinfo) * 4;
      layout_add_command(layout_obj, cmd_size, "compute-walker");

      cs_layout->dispatch.seq_offset = vk_layout->dispatch_src_offset_B;
   }

   layout_obj->data_prolog_size = align(layout_obj->data_prolog_size, 64);
   layout_obj->data_size = align(layout_obj->data_size, ANV_UBO_ALIGNMENT);

   layout_obj->emits_push_constants =
      (vk_layout->dgc_info & (BITFIELD_BIT(MESA_VK_DGC_PC) |
                              BITFIELD_BIT(MESA_VK_DGC_SI))) ||
      ((vk_layout->dgc_info & (BITFIELD_BIT(MESA_VK_DGC_DRAW) |
                               BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED) |
                               BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH))) &&
       (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)));

   *pIndirectCommandsLayout = anv_indirect_command_layout_to_handle(layout_obj);

   return VK_SUCCESS;
}

void
anv_DestroyIndirectCommandsLayoutEXT(
    VkDevice                                    _device,
    VkIndirectCommandsLayoutEXT                 indirectCommandsLayout,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_indirect_command_layout, layout, indirectCommandsLayout);

   if (!layout)
      return;

   vk_indirect_command_layout_destroy(&device->vk, pAllocator, &layout->vk);
}

void anv_GetGeneratedCommandsMemoryRequirementsEXT(
    VkDevice                                    _device,
    const VkGeneratedCommandsMemoryRequirementsInfoEXT* pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_indirect_command_layout, layout_obj,
                   pInfo->indirectCommandsLayout);

   pMemoryRequirements->memoryRequirements.alignment = 64;
   pMemoryRequirements->memoryRequirements.size =
      align(layout_obj->cmd_prolog_size + layout_obj->cmd_epilog_size +
            pInfo->maxSequenceCount * layout_obj->cmd_size, 64) +
      align(pInfo->maxSequenceCount * layout_obj->data_size, 64) +
      align(layout_obj->data_prolog_size, 64);
   pMemoryRequirements->memoryRequirements.memoryTypeBits =
      device->info->verx10 <= 120 ?
      device->physical->memory.dynamic_visible_mem_types :
      device->physical->memory.default_buffer_mem_types;
}

void
anv_generated_gfx_fill_state(struct anv_gen_gfx_state *state,
                             struct anv_cmd_buffer *cmd_buffer,
                             const struct anv_indirect_command_layout *layout,
                             const struct anv_graphics_pipeline *pipeline,
                             const struct anv_indirect_execution_set *indirect_set)
{
   const struct vk_indirect_command_layout *vk_layout = &layout->vk;

   if (vk_layout->dgc_info & (BITFIELD_BIT(MESA_VK_DGC_PC) |
                              BITFIELD_BIT(MESA_VK_DGC_SI))) {
      struct anv_cmd_graphics_state *gfx_state = &cmd_buffer->state.gfx;

      if (pipeline) {
         anv_foreach_vk_stage(stage, ANV_GRAPHICS_STAGE_BITS) {
            uint32_t n_slots = 0;
            if ((pipeline->base.base.active_stages & stage) == 0)
               continue;

            const struct anv_pipeline_bind_map *bind_map = &pipeline->base.shaders[
               vk_stage_to_mesa_stage(stage)]->bind_map;
            for (uint32_t i = 0; i < ARRAY_SIZE(bind_map->push_ranges); i++) {
               const struct anv_push_range *range = &bind_map->push_ranges[i];
               if (range->length == 0)
                  break;

               switch (range->set) {
               case ANV_DESCRIPTOR_SET_DESCRIPTORS: {
                  struct anv_descriptor_set *set =
                     gfx_state->base.descriptors[range->index];
                  state->push_constants.addresses[i] = anv_address_physical(
                     anv_descriptor_set_address(set));
                  break;
               }

               case ANV_DESCRIPTOR_SET_DESCRIPTORS_BUFFER:
                  state->push_constants.addresses[i] =
                     anv_cmd_buffer_descriptor_buffer_address(
                        cmd_buffer,
                        gfx_state->base.descriptor_buffers[range->index].buffer_index) +
                     gfx_state->base.descriptor_buffers[range->index].buffer_offset;
                  break;

               case ANV_DESCRIPTOR_SET_PUSH_CONSTANTS:
                  break;

               case ANV_DESCRIPTOR_SET_NULL:
                  state->push_constants.addresses[i] =
                     anv_address_physical(cmd_buffer->device->workaround_address);
                  break;

               default: {
                  struct anv_descriptor_set *set =
                     gfx_state->base.descriptors[range->set];
                  const struct anv_descriptor *desc =
                     &set->descriptors[range->index];

                  if (desc->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                     if (desc->buffer) {
                        state->push_constants.addresses[i] = anv_address_physical(
                           anv_address_add(desc->buffer->address,
                                           desc->offset));
                     }
                  } else {
                     assert(desc->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
                     if (desc->buffer) {
                        const struct anv_cmd_pipeline_state *pipe_state = &gfx_state->base;
                        uint32_t dynamic_offset =
                           pipe_state->dynamic_offsets[
                              range->set].offsets[range->dynamic_offset_index];
                        state->push_constants.addresses[i] = anv_address_physical(
                           anv_address_add(desc->buffer->address,
                                           desc->offset + dynamic_offset));
                     }
                  }

                  if (state->push_constants.addresses[i] == 0) {
                     /* For NULL UBOs, we just return an address in the
                      * workaround BO. We do writes to it for workarounds but
                      * always at the bottom. The higher bytes should be all
                      * zeros.
                      */
                     assert(range->length * 32 <= 2048);
                     state->push_constants.addresses[i] =
                        anv_address_physical((struct anv_address) {
                              .bo = cmd_buffer->device->workaround_bo,
                              .offset = 1024,
                           });
                  }
               }
               }

               n_slots++;
            }
         }
      }
   }
}

/* This function determines the final layout of GFX generated commands. A lot
 * of things make the amount of space vary (number of stages, number of push
 * constant slots, etc...) such that we can only determine this just before
 * executing the generation.
 */
uint32_t
anv_generated_gfx_fill_layout(struct anv_gen_gfx_layout *layout,
                              const struct anv_device *device,
                              const struct anv_indirect_command_layout *layout_obj,
                              const struct anv_graphics_pipeline *pipeline,
                              const struct anv_indirect_execution_set *indirect_set)
{
   const struct vk_indirect_command_layout *vk_layout = &layout_obj->vk;
   const struct intel_device_info *devinfo = device->info;

   uint32_t cmd_offset = 0;

   layout->draw.draw_type =
      (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH)) ? ANV_GEN_GFX_DRAW_MESH :
      (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED)) ? ANV_GEN_GFX_DRAW_INDEXED :
      ANV_GEN_GFX_DRAW;

   layout->index_buffer.cmd_offset = cmd_offset;
   if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IB)) {
      layout->index_buffer.cmd_size = _3DSTATE_INDEX_BUFFER_length(devinfo) * 4;
      layout->index_buffer.seq_offset = vk_layout->index_src_offset_B;
      layout->index_buffer.mocs =
         isl_mocs(&device->isl_dev, ISL_SURF_USAGE_INDEX_BUFFER_BIT, false);
      if (vk_layout->index_mode_is_dx) {
         /* DXGI_FORMAT values */
         layout->index_buffer.u32_value = 42;
         layout->index_buffer.u16_value = 57;
         layout->index_buffer.u8_value  = 62;
      } else {
         layout->index_buffer.u32_value = VK_INDEX_TYPE_UINT32;
         layout->index_buffer.u16_value = VK_INDEX_TYPE_UINT16;
         layout->index_buffer.u8_value  = VK_INDEX_TYPE_UINT8_EXT;
      }

      cmd_offset += layout->index_buffer.cmd_size;
   }

   layout->vertex_buffers.cmd_offset = cmd_offset;
   if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_VB)) {
      layout->vertex_buffers.cmd_size =
         (1 /* TODO: _3DSTATE_VERTEX_BUFFERS_length(devinfo) */ +
          util_bitcount(vk_layout->vertex_bindings) *
          VERTEX_BUFFER_STATE_length(devinfo)) * 4;
      layout->vertex_buffers.mocs =
         isl_mocs(&device->isl_dev, ISL_SURF_USAGE_VERTEX_BUFFER_BIT, false);
      layout->vertex_buffers.n_buffers = vk_layout->n_vb_layouts;
      for (uint32_t i = 0; i < vk_layout->n_vb_layouts; i++) {
         layout->vertex_buffers.buffers[i].seq_offset =
            vk_layout->vb_layouts[i].src_offset_B;
         layout->vertex_buffers.buffers[i].binding =
            vk_layout->vb_layouts[i].binding;
      }

      cmd_offset += layout->vertex_buffers.cmd_size;
   }

   layout->indirect_set.final_cmds_offset = cmd_offset;
   layout->indirect_set.partial_cmds_offset = cmd_offset;
   if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) {
      assert(indirect_set != NULL);
      layout->indirect_set.active = true;

      layout->indirect_set.final_cmds_size =
         (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH))  ?
         indirect_mesh_gfx_final_cmd_size(devinfo) :
         indirect_legacy_gfx_final_cmd_size(devinfo);

      assert(indirect_set->max_final_commands_size <= layout->indirect_set.final_cmds_size);

      assert(layout->indirect_set.final_cmds_size <=
             sizeof_field(struct anv_gen_gfx_indirect_descriptor,
                          final_commands));

      cmd_offset += layout->indirect_set.final_cmds_size;

      layout->indirect_set.partial_cmds_offset = cmd_offset;
      layout->indirect_set.partial_cmds_size =
         indirect_partial_gfx_cmd_size(
            devinfo,
            layout->draw.draw_type == ANV_GEN_GFX_DRAW_MESH);

      cmd_offset += layout->indirect_set.partial_cmds_size;
   } else {
      assert(pipeline != NULL);
      if (intel_needs_workaround(devinfo, 16011107343) &&
          anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_CTRL)) {
         layout->indirect_set.final_cmds_size +=
            _3DSTATE_HS_length(devinfo) * 4;
      }
      if (intel_needs_workaround(devinfo, 22018402687) &&
          anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL)) {
         layout->indirect_set.final_cmds_size +=
            _3DSTATE_DS_length(devinfo) * 4;
      }

      cmd_offset += layout->indirect_set.final_cmds_size;
   }

   layout->push_constants.cmd_offset = cmd_offset;
   if (vk_layout->dgc_info & (BITFIELD_BIT(MESA_VK_DGC_PC) |
                              BITFIELD_BIT(MESA_VK_DGC_SI))) {
      struct anv_gen_push_layout *pc_layout = &layout->push_constants;

      layout->push_constants.flags = (ANV_GEN_PUSH_CONSTANTS_CMD_ACTIVE |
                                      ANV_GEN_PUSH_CONSTANTS_DATA_ACTIVE);
      for (uint32_t i = 0; i < vk_layout->n_pc_layouts; i++) {
         const struct vk_indirect_command_push_constant_layout *vk_pc_layout =
            &vk_layout->pc_layouts[i];
         push_layout_add_range(&layout->push_constants, vk_pc_layout);
      }
      if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_SI)) {
         pc_layout->seq_id_active = true;
         pc_layout->seq_id_offset = vk_layout->si_layout.dst_offset_B;
      }
      pc_layout->mocs =
         isl_mocs(&device->isl_dev, ISL_SURF_USAGE_CONSTANT_BUFFER_BIT, false);

      const VkShaderStageFlags stages =
         (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH)) ?
         (VK_SHADER_STAGE_MESH_BIT_EXT |
          VK_SHADER_STAGE_TASK_BIT_EXT |
          VK_SHADER_STAGE_FRAGMENT_BIT) :
         VK_SHADER_STAGE_ALL_GRAPHICS;
      anv_foreach_vk_stage(stage, stages) {
         uint32_t n_slots = 0;
         if (pipeline) {
            if ((pipeline->base.base.active_stages & stage) == 0)
               continue;

            const struct anv_pipeline_bind_map *bind_map =
               &pipeline->base.shaders[
                  vk_stage_to_mesa_stage(stage)]->bind_map;
            for (uint32_t i = 0; i < ARRAY_SIZE(bind_map->push_ranges); i++) {
               const struct anv_push_range *range = &bind_map->push_ranges[i];
               if (range->length == 0)
                  break;
               n_slots++;
            }
         } else {
            assert(indirect_set);
            n_slots = 1;
         }

         layout->push_constants.cmd_size +=
            push_constant_command_size(devinfo, stage, n_slots);
      }

      cmd_offset += layout->push_constants.cmd_size;
   } else if ((vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) &&
              (vk_layout->dgc_info & (BITFIELD_BIT(MESA_VK_DGC_DRAW) |
                                      BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED) |
                                      BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH)))) {
      const VkShaderStageFlags stages =
         (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH)) ?
         (VK_SHADER_STAGE_MESH_BIT_EXT |
          VK_SHADER_STAGE_TASK_BIT_EXT |
          VK_SHADER_STAGE_FRAGMENT_BIT) :
         VK_SHADER_STAGE_ALL_GRAPHICS;
      layout->push_constants.flags = (ANV_GEN_PUSH_CONSTANTS_CMD_ACTIVE |
                                      ANV_GEN_PUSH_CONSTANTS_DATA_ACTIVE);
      layout->push_constants.cmd_size +=
         push_constant_command_size(devinfo, stages, 1);
      cmd_offset += layout->push_constants.cmd_size;
   }

   layout->draw.cmd_offset = cmd_offset;
   layout->draw.cmd_size = draw_cmd_size(devinfo, vk_layout);
   layout->draw.seq_offset = vk_layout->draw_src_offset_B;

   cmd_offset += layout->draw.cmd_size;

   assert(cmd_offset <= layout_obj->cmd_size);

   return cmd_offset;
}

void
anv_generated_commands_gfx_print_state(const struct anv_gen_gfx_layout *layout,
                                       const struct anv_indirect_command_layout *layout_obj)
{
   fprintf(stderr, "Generated Gfx state:\n");
#define PRINT(state_bits, cond2, ...) do {            \
      if ((state_bits) == 0 ||                        \
          (layout_obj->vk.dgc_info & (state_bits)) || \
          (cond2))                                    \
         fprintf(stderr, __VA_ARGS__);                \
   } while (0)
   PRINT(BITFIELD_BIT(MESA_VK_DGC_IB), false,
         "  ib:      cmd_offset=0x%04x-0x%04x (%u)\n",
         layout->index_buffer.cmd_offset,
         layout->index_buffer.cmd_offset +
         layout->index_buffer.cmd_size,
         layout->index_buffer.cmd_size);
   PRINT(BITFIELD_BIT(MESA_VK_DGC_VB), false,
         "  vb:      cmd_offset=0x%04x-0x%04x (%u)\n",
         layout->vertex_buffers.cmd_offset,
         layout->vertex_buffers.cmd_offset +
         layout->vertex_buffers.cmd_size,
         layout->vertex_buffers.cmd_size);
   PRINT(0, false,
         "  final:   cmd_offset=0x%04x-0x%04x (%u)\n",
         layout->indirect_set.final_cmds_offset,
         layout->indirect_set.final_cmds_offset +
         layout->indirect_set.final_cmds_size,
         layout->indirect_set.final_cmds_size);
   PRINT(BITFIELD_BIT(MESA_VK_DGC_IES), false,
         "  partial: cmd_offset=0x%04x-0x%04x (%u)\n",
         layout->indirect_set.partial_cmds_offset,
         layout->indirect_set.partial_cmds_offset +
         layout->indirect_set.partial_cmds_size,
         layout->indirect_set.partial_cmds_size);
   PRINT(BITFIELD_BIT(MESA_VK_DGC_PC) |
         BITFIELD_BIT(MESA_VK_DGC_SI),
         layout->push_constants.cmd_size != 0,
         "  push:    cmd_offset=0x%04x-0x%04x (%u)\n",
         layout->push_constants.cmd_offset,
         layout->push_constants.cmd_offset +
         layout->push_constants.cmd_size,
         layout->push_constants.cmd_size);
   PRINT(0, false,
         "  draw:    cmd_offset=0x%04x-0x%04x (%u)\n",
         layout->draw.cmd_offset,
         layout->draw.cmd_offset +
         layout->draw.cmd_size,
         layout->draw.cmd_size);
#undef PRINT
}

void
anv_generated_commands_print_layout(const struct anv_indirect_command_layout *layout)
{
   fprintf(stderr, "Generated %s layout:\n",
           layout->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS ? "Gfx" :
           layout->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE  ? "CS" :
           layout->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE  ? "RT" : "unknown");
#define DGC_BIT(name) ((layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_##name)) ? #name"," : "")
   fprintf(stderr, "  bits: %s%s%s%s%s%s%s%s%s%s\n",
           DGC_BIT(IES),
           DGC_BIT(PC),
           DGC_BIT(IB),
           DGC_BIT(VB),
           DGC_BIT(SI),
           DGC_BIT(DRAW),
           DGC_BIT(DRAW_INDEXED),
           DGC_BIT(DRAW_MESH),
           DGC_BIT(DISPATCH),
           DGC_BIT(RT));
#undef DGC_BIT
   fprintf(stderr, "  seq_stride:    %lu\n", layout->vk.stride);
   fprintf(stderr, "  cmd_prolog:    %u\n", layout->cmd_prolog_size);
   fprintf(stderr, "  cmd_stride:    %u\n", layout->cmd_size);
   fprintf(stderr, "  cmd_epilog:    %u\n", layout->cmd_epilog_size);
   fprintf(stderr, "  data_prolog:   %u\n", layout->data_prolog_size);
   fprintf(stderr, "  data_stride:   %u\n", layout->data_size);

   fprintf(stderr, "  sequences:\n");
   const struct vk_indirect_command_layout *vk_layout = &layout->vk;
   const struct anv_gen_push_layout *pc_layout =
      layout->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS ?
      &layout->gfx_layout.push_constants : &layout->cs_layout.push_constants;
   if (pc_layout->num_entries > 0 || pc_layout->seq_id_active) {
      fprintf(stderr, "    push_constants:\n");
      for (uint32_t i = 0; i < pc_layout->num_entries; i++) {
         fprintf(stderr,
                 "      pc_entry%02u seq_offset: 0x%04x (offset=%hu, size=%hu)\n",
                 i,
                 pc_layout->entries[i].seq_offset,
                 pc_layout->entries[i].push_offset,
                 pc_layout->entries[i].size);
      }
      if (pc_layout->seq_id_active) {
         fprintf(stderr, "      seq_id_offset: 0x%04hx\n",
                 pc_layout->seq_id_offset);
      }
   }
   switch (layout->bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS: {
      if (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IB)) {
         fprintf(stderr, "    index_buffer:\n");
         fprintf(stderr, "      seq_offset: 0x%04x\n", vk_layout->index_src_offset_B);
      }
      if (vk_layout->n_vb_layouts) {
         fprintf(stderr, "    vertex_buffers:\n");
         for (uint32_t i = 0; i < vk_layout->n_vb_layouts; i++) {
            fprintf(stderr, "      seq_offset: 0x%04x (vb%u)\n",
                    vk_layout->vb_layouts[i].src_offset_B,
                    vk_layout->vb_layouts[i].binding);
         }
      }
      fprintf(stderr, "    %s:\n",
              (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH)) ? "mesh" :
              (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED)) ? "draw-indexed" :
              (vk_layout->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW)) ? "draw" :
              "unknown");
      fprintf(stderr, "      seq_offset: 0x%04x\n", vk_layout->draw_src_offset_B);
      break;
   }
   case VK_PIPELINE_BIND_POINT_COMPUTE:
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR: {
      const struct anv_gen_cs_layout *cs_layout = &layout->cs_layout;
      if (cs_layout->indirect_set.active) {
         fprintf(stderr, "    ies:\n");
         fprintf(stderr, "      seq_offset: 0x%04x\n", cs_layout->indirect_set.seq_offset);
      }
      fprintf(stderr, "    dispatch:\n");
      fprintf(stderr, "      seq_offset: 0x%04x\n", cs_layout->dispatch.seq_offset);
      break;
   }
   default:
      unreachable("Invalid bind point");
   }

   fprintf(stderr, "  commands:\n");
   for (uint32_t i = 0; i < layout->n_items; i++) {
      fprintf(stderr, "    %s: %u\n",
              layout->items[i].name, layout->items[i].size);
   }
}
