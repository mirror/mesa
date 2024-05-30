/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdbool.h>

#include "genxml/genX_bits.h"

#include "anv_private.h"

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

enum anv_gen_command_stage
anv_vk_stage_to_generated_stage(VkShaderStageFlags vk_stage)
{
   switch (vk_stage) {
   case VK_SHADER_STAGE_VERTEX_BIT:
      return ANV_GENERATED_COMMAND_STAGE_VERTEX;
   case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
      return ANV_GENERATED_COMMAND_STAGE_TESS_CTRL;
   case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
      return ANV_GENERATED_COMMAND_STAGE_TESS_EVAL;
   case VK_SHADER_STAGE_GEOMETRY_BIT:
      return ANV_GENERATED_COMMAND_STAGE_GEOMETRY;
   case VK_SHADER_STAGE_FRAGMENT_BIT:
      return ANV_GENERATED_COMMAND_STAGE_FRAGMENT;
   case VK_SHADER_STAGE_TASK_BIT_EXT:
      return ANV_GENERATED_COMMAND_STAGE_TASK;
   case VK_SHADER_STAGE_MESH_BIT_EXT:
      return ANV_GENERATED_COMMAND_STAGE_MESH;
   case VK_SHADER_STAGE_COMPUTE_BIT:
      return ANV_GENERATED_COMMAND_STAGE_COMPUTE;
   default:
      unreachable("Unhandled stage");
   }
}

uint32_t
anv_vk_stages_to_generated_stages(VkShaderStageFlags vk_stages)
{
   uint32_t gen_stages = 0;
   anv_foreach_vk_stage(stage, vk_stages)
      gen_stages |= BITFIELD_BIT(anv_vk_stage_to_generated_stage(stage));
   return gen_stages;
}

void
anv_indirect_descriptor_push_constants_write(struct anv_gen_gfx_indirect_descriptor *descriptor,
                                             struct anv_graphics_pipeline *pipeline)
{
   anv_foreach_vk_stage(stage, ANV_GRAPHICS_STAGE_BITS) {
      enum anv_gen_command_stage gen_stage =
         anv_vk_stage_to_generated_stage(stage);
      struct anv_gen_push_stage_state empty_push = {};

      if ((pipeline->base.base.active_stages & stage) == 0) {
         descriptor->push_constants.stages[gen_stage] = empty_push;
         continue;
      }

      const struct anv_pipeline_bind_map *bind_map = &pipeline->base.shaders[
         vk_stage_to_mesa_stage(stage)]->bind_map;
      for (uint32_t i = 0; i < ARRAY_SIZE(bind_map->push_ranges); i++) {
         const struct anv_push_range *range = &bind_map->push_ranges[i];
         if (range->length == 0)
            break;

         /* We should have compiler all the indirectly bindable shaders in
          * such a way that it's the only types of push constants we should
          * see.
          */
         assert(range->set == ANV_DESCRIPTOR_SET_PUSH_CONSTANTS ||
                range->set == ANV_DESCRIPTOR_SET_NULL);

         struct anv_gen_push_stage_slot *slot =
            &descriptor->push_constants.stages[gen_stage].slots[i];

         slot->push_data_size = 32 * range->length;

         slot->push_data_offset = 32 * range->start;
         slot->type = ANV_GEN_PUSH_SLOT_TYPE_PUSH_CONSTANTS;
         descriptor->push_constants.stages[gen_stage].n_slots++;
      }
      descriptor->push_constants.active_stages |=
         anv_vk_stages_to_generated_stages(stage);
   }
}

static void
write_gfx_set_entry(const struct intel_device_info *devinfo,
                    struct anv_indirect_execution_set *indirect_set,
                    uint32_t entry,
                    struct anv_graphics_pipeline *gfx_pipeline)
{
   struct anv_gen_gfx_indirect_descriptor descriptor = {};

   anv_genX(devinfo, write_gfx_indirect_descriptor)(&descriptor,
                                                    indirect_set,
                                                    gfx_pipeline);

   anv_indirect_descriptor_push_constants_write(&descriptor, gfx_pipeline);

   /* TODO: additional bits of information */
   descriptor.active_stages = anv_vk_stages_to_generated_stages(
      gfx_pipeline->base.base.active_stages);

   memcpy(indirect_set->bo->map + entry * indirect_set->stride,
          &descriptor, sizeof(descriptor));

   indirect_set->max_final_commands_size =
      MAX2(indirect_set->max_final_commands_size, descriptor.final_commands_size);

   indirect_set->uses_xfb |= gfx_pipeline->uses_xfb;

   indirect_set->max_scratch = MAX2(indirect_set->max_scratch,
                                    gfx_pipeline->base.base.scratch_size);
   indirect_set->max_ray_queries = MAX2(indirect_set->max_ray_queries,
                                        gfx_pipeline->base.base.ray_queries);
}

static void
write_cs_set_entry(const struct intel_device_info *devinfo,
                   struct anv_indirect_execution_set *indirect_set,
                   uint32_t entry,
                   struct anv_compute_pipeline *compute_pipeline)
{
   const struct anv_shader_bin *shader = compute_pipeline->cs;
   const struct anv_pipeline_bind_map *bind_map = &shader->bind_map;
   const struct anv_push_range *push_range = &bind_map->push_ranges[0];

   struct anv_gen_cs_indirect_descriptor descriptor = {
      .push_data_offset = 32 * (push_range->set == ANV_DESCRIPTOR_SET_PUSH_CONSTANTS ?
                                push_range->start : 0),
   };

   const struct brw_cs_prog_data *prog_data =
      get_cs_prog_data(compute_pipeline);

   if (devinfo->verx10 >= 125) {
      assert(sizeof(descriptor.gfx125.compute_walker) ==
             sizeof(compute_pipeline->gfx125.compute_walker));
      memcpy(descriptor.gfx125.compute_walker,
             compute_pipeline->gfx125.compute_walker,
             sizeof(descriptor.gfx125.compute_walker));
   } else {
      assert(sizeof(descriptor.gfx9.media_vfe_state) ==
             (compute_pipeline->base.batch.next -
              compute_pipeline->base.batch.start));
      assert(sizeof(descriptor.gfx9.interface_descriptor_data) ==
             sizeof(compute_pipeline->gfx9.interface_descriptor_data));
      assert(sizeof(descriptor.gfx9.gpgpu_walker) ==
             sizeof(compute_pipeline->gfx9.gpgpu_walker));

      memcpy(descriptor.gfx9.media_vfe_state,
             compute_pipeline->batch_data,
             sizeof(descriptor.gfx9.media_vfe_state));
      memcpy(descriptor.gfx9.interface_descriptor_data,
             compute_pipeline->gfx9.interface_descriptor_data,
             sizeof(descriptor.gfx9.interface_descriptor_data));
      memcpy(descriptor.gfx9.gpgpu_walker,
             compute_pipeline->gfx9.gpgpu_walker,
             sizeof(descriptor.gfx9.gpgpu_walker));

      const struct intel_cs_dispatch_info dispatch =
         brw_cs_get_dispatch_info(devinfo, prog_data, NULL);
      descriptor.gfx9.n_threads = dispatch.threads;
      descriptor.gfx9.cross_thread_push_size = prog_data->push.cross_thread.size;
      descriptor.gfx9.per_thread_push_size = prog_data->push.per_thread.size;
      descriptor.gfx9.subgroup_id_offset =
         offsetof(struct anv_push_constants, cs.subgroup_id) -
         (32 * push_range->start + prog_data->push.cross_thread.size);

      anv_reloc_list_append(&indirect_set->relocs,
                            &compute_pipeline->base.batch_relocs);
   }

   memcpy(indirect_set->bo->map + entry * indirect_set->stride,
          &descriptor, sizeof(descriptor));

   indirect_set->max_scratch = MAX2(indirect_set->max_scratch,
                                    prog_data->base.total_scratch);
   indirect_set->max_ray_queries = MAX2(indirect_set->max_ray_queries,
                                        compute_pipeline->base.ray_queries);
}

static void
write_rt_set_entry(const struct intel_device_info *devinfo,
                   struct anv_indirect_execution_set *indirect_set,
                   uint32_t entry,
                   struct anv_ray_tracing_pipeline *rt_pipeline)
{
   indirect_set->max_scratch = MAX2(indirect_set->max_scratch,
                                    rt_pipeline->base.scratch_size);
   indirect_set->max_ray_queries = MAX2(indirect_set->max_ray_queries,
                                        rt_pipeline->base.ray_queries);
}

VkResult anv_CreateIndirectExecutionSetEXT(
   VkDevice                                    _device,
   const VkIndirectExecutionSetCreateInfoEXT*  pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkIndirectExecutionSetEXT*                  pIndirectExecutionSet)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_pipeline, pipeline,
                   pCreateInfo->info.pPipelineInfo->initialPipeline);

   struct anv_indirect_execution_set *indirect_set =
      vk_object_zalloc(&device->vk, pAllocator,
                       sizeof(struct anv_indirect_execution_set),
                       VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT);
   if (indirect_set == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result =
      anv_reloc_list_init(&indirect_set->relocs,
                          pAllocator ? pAllocator : &device->vk.alloc,
                          device->physical->uses_relocs);
   if (result != VK_SUCCESS)
      goto fail_object;

   indirect_set->template_pipeline = pipeline;

   enum anv_bo_alloc_flags alloc_flags =
      ANV_BO_ALLOC_CAPTURE |
      ANV_BO_ALLOC_MAPPED |
      ANV_BO_ALLOC_HOST_CACHED_COHERENT;

   switch (pipeline->type) {
   case ANV_PIPELINE_GRAPHICS: {
      struct anv_graphics_pipeline *gfx_pipeline =
         anv_pipeline_to_graphics(pipeline);

      indirect_set->stride = sizeof(struct anv_gen_gfx_indirect_descriptor);

      uint32_t size = align(
         pCreateInfo->info.pPipelineInfo->maxPipelineCount *
         indirect_set->stride, 4096);

      result = anv_device_alloc_bo(device, "indirect-exec-set", size,
                                   alloc_flags, 0 /* explicit_address */,
                                   &indirect_set->bo);
      if (result != VK_SUCCESS)
         goto fail_relocs;

      indirect_set->layout_type = pipeline->layout.type;
      /* indirect_set->bind_map = anv_pipeline_bind_map_clone( */
      /*    device, pAllocator, &gfx_pipeline->cs->bind_map); */
      /* if (indirect_set->bind_map == NULL) { */
      /*    result = vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY, */
      /*                       "Fail to allocate bind map"); */
      /*    goto fail_bo; */
      /* } */

      write_gfx_set_entry(device->info, indirect_set, 0, gfx_pipeline);
      break;
   }

   case ANV_PIPELINE_COMPUTE: {
      struct anv_compute_pipeline *cs_pipeline =
         anv_pipeline_to_compute(pipeline);

      /* Alignment required for
       * MEDIA_INTERFACE_DESCRIPTOR_LOAD::InterfaceDescriptorDataStartAddress
       */
      STATIC_ASSERT(sizeof(struct anv_gen_cs_indirect_descriptor) % 64 == 0);

      indirect_set->stride = sizeof(struct anv_gen_cs_indirect_descriptor);

      uint32_t size = align(
         pCreateInfo->info.pPipelineInfo->maxPipelineCount *
         indirect_set->stride, 4096);

      /* Generations up to Gfx12.0 have a structures describing the compute
       * shader that needs to live in the dynamic state heap.
       */
      if (device->info->verx10 <= 120)
         alloc_flags |= ANV_BO_ALLOC_DYNAMIC_VISIBLE_POOL;

      result = anv_device_alloc_bo(device, "indirect-exec-set", size,
                                   alloc_flags, 0 /* explicit_address */,
                                   &indirect_set->bo);
      if (result != VK_SUCCESS)
         goto fail_relocs;

      indirect_set->layout_type = pipeline->layout.type;
      indirect_set->bind_map = anv_pipeline_bind_map_clone(
         device, pAllocator, &cs_pipeline->cs->bind_map);
      if (indirect_set->bind_map == NULL) {
         result = vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                            "Fail to allocate bind map");
         goto fail_bo;
      }

      write_cs_set_entry(device->info, indirect_set, 0, cs_pipeline);
      break;
   }

   case ANV_PIPELINE_RAY_TRACING: {
      struct anv_ray_tracing_pipeline *rt_pipeline =
         anv_pipeline_to_ray_tracing(pipeline);
      write_rt_set_entry(device->info, indirect_set, 0, rt_pipeline);
      break;
   }

   default:
      unreachable("Unsupported indirect pipeline type");
   }

   *pIndirectExecutionSet = anv_indirect_execution_set_to_handle(indirect_set);

   return VK_SUCCESS;

 fail_bo:
   anv_device_release_bo(device, indirect_set->bo);
 fail_relocs:
   anv_reloc_list_finish(&indirect_set->relocs);
 fail_object:
   vk_object_free(&device->vk, pAllocator, indirect_set);
   return result;
}

void anv_DestroyIndirectExecutionSetEXT(
   VkDevice                                    _device,
   VkIndirectExecutionSetEXT                   indirectExecutionSet,
   const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_indirect_execution_set, indirect_set, indirectExecutionSet);

   vk_free2(&device->vk.alloc, pAllocator, indirect_set->bind_map);
   anv_reloc_list_finish(&indirect_set->relocs);
   if (indirect_set->bo)
      anv_device_release_bo(device, indirect_set->bo);
   vk_object_free(&device->vk, pAllocator, indirect_set);
}

void anv_UpdateIndirectExecutionSetPipelineEXT(
   VkDevice                                    _device,
   VkIndirectExecutionSetEXT                   indirectExecutionSet,
   uint32_t                                    executionSetWriteCount,
   const VkWriteIndirectExecutionSetPipelineEXT* pExecutionSetWrites)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_indirect_execution_set, indirect_set, indirectExecutionSet);

   for (uint32_t i = 0; i < executionSetWriteCount; i++) {
      ANV_FROM_HANDLE(anv_pipeline, pipeline, pExecutionSetWrites[i].pipeline);

      switch (pipeline->type) {
      case ANV_PIPELINE_GRAPHICS:
         write_gfx_set_entry(device->info, indirect_set,
                             pExecutionSetWrites[i].index,
                             anv_pipeline_to_graphics(pipeline));
         break;

      case ANV_PIPELINE_COMPUTE:
         write_cs_set_entry(device->info, indirect_set,
                            pExecutionSetWrites[i].index,
                            anv_pipeline_to_compute(pipeline));
         break;

      case ANV_PIPELINE_RAY_TRACING:
         write_rt_set_entry(device->info, indirect_set,
                            pExecutionSetWrites[i].index,
                            anv_pipeline_to_ray_tracing(pipeline));
         break;

      default:
         unreachable("Unsupported indirect pipeline type");
      }
   }
}

void anv_UpdateIndirectExecutionSetShaderEXT(
   VkDevice                                    device,
   VkIndirectExecutionSetEXT                   indirectExecutionSet,
   uint32_t                                    executionSetWriteCount,
   const VkWriteIndirectExecutionSetShaderEXT* pExecutionSetWrites)
{
   /* Noop, we don't support VK_EXT_shader_object */
}
