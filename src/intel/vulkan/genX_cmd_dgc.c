/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdbool.h>

#include "anv_internal_kernels.h"
#include "anv_private.h"
#include "anv_measure.h"
#include "vk_render_pass.h"
#include "vk_util.h"

#include "common/intel_aux_map.h"
#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"
#include "genxml/genX_rt_pack.h"
#include "common/intel_genX_state_brw.h"

#include "ds/intel_tracepoints.h"

#include "genX_mi_builder.h"

static struct anv_state
emit_push_constants(struct anv_cmd_buffer *cmd_buffer,
                    const struct anv_cmd_pipeline_state *pipe_state)
{
   const uint8_t *data = (const uint8_t *) &pipe_state->push_constants;

   struct anv_state state =
      anv_cmd_buffer_alloc_temporary_state(cmd_buffer,
                                           sizeof(struct anv_push_constants),
                                           32 /* bottom 5 bits MBZ */);
   if (state.alloc_size == 0)
      return state;

   memcpy(state.map, data, pipe_state->push_constants_client_size);
   memcpy(state.map + MAX_PUSH_CONSTANTS_SIZE,
          data + MAX_PUSH_CONSTANTS_SIZE,
          sizeof(struct anv_push_constants) - MAX_PUSH_CONSTANTS_SIZE);

   return state;
}

static struct anv_generated_gfx_commands_params *
preprocess_gfx_sequences(struct anv_cmd_buffer *cmd_buffer,
                         struct anv_cmd_buffer *cmd_buffer_state,
                         struct anv_indirect_command_layout *layout,
                         struct anv_indirect_execution_set *indirect_set,
                         const VkGeneratedCommandsInfoEXT *info,
                         enum anv_internal_kernel_name kernel_name)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_graphics_pipeline *gfx_pipeline;
   if (indirect_set == NULL) {
      const VkGeneratedCommandsPipelineInfoEXT *pipeline_info =
         vk_find_struct_const(info->pNext, GENERATED_COMMANDS_PIPELINE_INFO_EXT);
      assert(pipeline_info);
      ANV_FROM_HANDLE(anv_pipeline, pipeline, pipeline_info->pipeline);
      gfx_pipeline = anv_pipeline_to_graphics(pipeline);
   } else {
      gfx_pipeline = NULL;
   }

   /* Allocate push constants with the cmd_buffer_state data. */
   struct anv_state push_constants_state =
      emit_push_constants(cmd_buffer, &cmd_buffer_state->state.gfx.base);
   if (push_constants_state.alloc_size == 0)
      return NULL;

   /**/
   struct anv_gen_gfx_state gfx_state = {};
   struct anv_state gfx_state_state =
      anv_cmd_buffer_alloc_temporary_state(cmd_buffer, sizeof(gfx_state), 8);
   if (gfx_state_state.map == NULL)
      return NULL;

   uint32_t cmd_stride =
      anv_generated_gfx_fill_layout(&gfx_state.layout, device, layout,
                                    gfx_pipeline, indirect_set);
   anv_generated_gfx_fill_state(&gfx_state, cmd_buffer_state,
                                layout, gfx_pipeline, indirect_set);
   genX(emit_indirect_dynamic_state)(&gfx_state, cmd_buffer_state, indirect_set);
   memcpy(gfx_state_state.map, &gfx_state, sizeof(gfx_state));

   /**/
   struct anv_shader_bin *generate_kernel;
   VkResult ret =
      anv_device_get_internal_shader(cmd_buffer->device,
                                     kernel_name, &generate_kernel);
   if (ret != VK_SUCCESS) {
      anv_batch_set_error(&cmd_buffer->batch, ret);
      return NULL;
   }

   struct anv_simple_shader simple_state = {
      .device               = cmd_buffer->device,
      .cmd_buffer           = cmd_buffer,
      .dynamic_state_stream = &cmd_buffer->dynamic_state_stream,
      .general_state_stream = &cmd_buffer->general_state_stream,
      .batch                = &cmd_buffer->batch,
      .kernel               = generate_kernel,
      .l3_config            = device->internal_kernels_l3_config,
      .urb_cfg              = &cmd_buffer->state.gfx.urb_cfg,
   };
   genX(emit_simple_shader_init)(&simple_state);

   struct anv_generated_gfx_commands_params *params;
   struct anv_state push_data_state =
      genX(simple_shader_alloc_push)(&simple_state, sizeof(*params));
   if (push_data_state.map == NULL)
      return NULL;
   params = push_data_state.map;

   const bool wa_16011107343 =
      INTEL_WA_16011107343_GFX_VER &&
      intel_needs_workaround(device->info, 16011107343) &&
      gfx_pipeline &&
      anv_pipeline_has_stage(gfx_pipeline, MESA_SHADER_TESS_CTRL);
   const bool wa_22018402687 =
      INTEL_WA_22018402687_GFX_VER &&
      intel_needs_workaround(device->info, 22018402687) &&
      gfx_pipeline &&
      anv_pipeline_has_stage(gfx_pipeline, MESA_SHADER_TESS_EVAL);

   /* Workaround instructions if needed */
   struct anv_state descriptor_state = ANV_STATE_NULL;
   if (indirect_set == NULL) {
      struct anv_gen_gfx_indirect_descriptor descriptor = {};
      anv_indirect_descriptor_push_constants_write(&descriptor, gfx_pipeline);

      uint32_t wa_insts_offset_dw = 0;
      if (wa_16011107343) {
         memcpy(&descriptor.final_commands[wa_insts_offset_dw],
                &gfx_pipeline->batch_data[
                   gfx_pipeline->final.hs.offset],
                GENX(3DSTATE_HS_length) * 4);
         wa_insts_offset_dw += GENX(3DSTATE_HS_length);
      }

      if (wa_22018402687) {
         memcpy(&descriptor.final_commands[wa_insts_offset_dw],
                &gfx_pipeline->batch_data[
                   gfx_pipeline->final.ds.offset],
                GENX(3DSTATE_DS_length) * 4);
         wa_insts_offset_dw += GENX(3DSTATE_DS_length);
      }

      descriptor_state =
         anv_cmd_buffer_alloc_temporary_state(
            cmd_buffer, sizeof(struct anv_gen_gfx_indirect_descriptor), 8);
      memcpy(descriptor_state.map, &descriptor, sizeof(descriptor));
   }

   *params = (struct anv_generated_gfx_commands_params) {
      .cmd_addr   = info->preprocessAddress,
      .cmd_stride = cmd_stride,

      .data_addr   = info->preprocessAddress +
                     align(layout->cmd_prolog_size +
                           info->maxSequenceCount * layout->cmd_size +
                           layout->cmd_epilog_size, 64),
      .data_stride = layout->data_size,

      .seq_addr   = info->indirectAddress,
      .seq_stride = layout->vk.stride,

      .seq_count_addr = info->sequenceCountAddress,
      .max_seq_count  = info->maxSequenceCount,

      .cmd_prolog_size = layout->cmd_prolog_size,
      .data_prolog_size = layout->data_prolog_size,

      .indirect_set_addr = indirect_set ?
                           anv_address_physical((struct anv_address) {
                                 .bo = indirect_set->bo, }) :
                           anv_address_physical(
                              anv_cmd_buffer_temporary_state_address(
                                 cmd_buffer, descriptor_state)),

      .state_addr = anv_address_physical(
         anv_cmd_buffer_temporary_state_address(cmd_buffer, gfx_state_state)),

      .const_addr = anv_address_physical(
         anv_cmd_buffer_temporary_state_address(
            cmd_buffer, push_constants_state)),
      .const_size =
         cmd_buffer_state->state.gfx.base.push_constants_client_size,

      .driver_const_addr = anv_address_physical(
         anv_address_add(
            anv_cmd_buffer_temporary_state_address(
               cmd_buffer, push_constants_state),
            MAX_PUSH_CONSTANTS_SIZE)),

      .flags = (cmd_buffer_state->state.conditional_render_enabled ?
                ANV_GENERATED_FLAG_PREDICATED : 0) |
               (wa_16011107343 ? ANV_GENERATED_FLAG_WA_16011107343 : 0) |
               (wa_22018402687 ? ANV_GENERATED_FLAG_WA_22018402687 : 0) |
               (intel_needs_workaround(device->info, 16014912113) ?
                ANV_GENERATED_FLAG_WA_16014912113 : 0) |
               (intel_needs_workaround(device->info, 18022330953) ||
                intel_needs_workaround(device->info, 22011440098) ?
                ANV_GENERATED_FLAG_WA_18022330953 : 0),
   };

   genX(emit_simple_shader_dispatch)(&simple_state, info->maxSequenceCount,
                                     push_data_state);

   return params;
}

#define merge_state(out, in)                            \
   do {                                                 \
      for (uint32_t i = 0; i < ARRAY_SIZE(out); i++)    \
         out[i] |= in[i];                               \
   } while (0)

static uint32_t
get_cs_pipeline_push_offset(const struct anv_cmd_pipeline_state *pipe_state,
                            const struct anv_indirect_command_layout *layout)
{
   /* With a device bound pipeline, we can't know this. */
   if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES))
      return 0;

   struct anv_compute_pipeline *pipeline =
      anv_pipeline_to_compute(pipe_state->pipeline);
   const struct anv_shader_bin *shader = pipeline->cs;
   const struct anv_pipeline_bind_map *bind_map = &shader->bind_map;
   const struct anv_push_range *push_range = &bind_map->push_ranges[0];

   return push_range->set == ANV_DESCRIPTOR_SET_PUSH_CONSTANTS ?
          (push_range->start * 32) : 0;
}

#if GFX_VERx10 >= 125
static void
write_driver_values(struct GENX(COMPUTE_WALKER) *walker,
                    struct anv_cmd_buffer *cmd_buffer)
{
   walker->PredicateEnable = cmd_buffer->state.conditional_render_enabled;
   walker->body.InterfaceDescriptor.SamplerStatePointer =
      cmd_buffer->state.samplers[MESA_SHADER_COMPUTE].offset;
   walker->body.InterfaceDescriptor.BindingTablePointer =
      cmd_buffer->state.binding_tables[MESA_SHADER_COMPUTE].offset;
}
#else
static void
write_driver_values(struct GENX(GPGPU_WALKER) *walker,
                    struct GENX(INTERFACE_DESCRIPTOR_DATA) *idd,
                    struct anv_cmd_buffer *cmd_buffer)
{
   walker->PredicateEnable = cmd_buffer->state.conditional_render_enabled;
   idd->BindingTablePointer =
      cmd_buffer->state.binding_tables[MESA_SHADER_COMPUTE].offset;
   idd->SamplerStatePointer =
      cmd_buffer->state.samplers[MESA_SHADER_COMPUTE].offset;
}
#endif /* GFX_VERx10 >= 125 */

static struct anv_generated_cs_commands_params *
preprocess_cs_sequences(struct anv_cmd_buffer *cmd_buffer,
                        struct anv_cmd_buffer *cmd_buffer_state,
                        struct anv_indirect_command_layout *layout,
                        struct anv_indirect_execution_set *indirect_set,
                        const VkGeneratedCommandsInfoEXT *info,
                        enum anv_internal_kernel_name kernel_name,
                        bool emit_driver_values)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_cmd_compute_state *comp_state = &cmd_buffer_state->state.compute;
   struct anv_cmd_pipeline_state *pipe_state = &comp_state->base;

   struct anv_state push_constants_state =
      emit_push_constants(cmd_buffer, pipe_state);
   if (push_constants_state.alloc_size == 0)
      return NULL;

   struct anv_state layout_state =
      anv_cmd_buffer_alloc_temporary_state(
         cmd_buffer, sizeof(layout->cs_layout), 8);
   if (layout_state.map == NULL)
      return NULL;
   memcpy(layout_state.map, &layout->cs_layout, sizeof(layout->cs_layout));

   /**/
   struct anv_gen_cs_indirect_descriptor cs_desc;

   cs_desc.push_data_offset = get_cs_pipeline_push_offset(pipe_state, layout);

#if GFX_VERx10 >= 125
   struct GENX(COMPUTE_WALKER) walker = {
      GENX(COMPUTE_WALKER_header),
      .body.PostSync.MOCS = anv_mocs(device, NULL, 0),
   };
   if (emit_driver_values)
      write_driver_values(&walker, cmd_buffer);

   GENX(COMPUTE_WALKER_pack)(NULL, cs_desc.gfx125.compute_walker, &walker);

   if ((layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) == 0) {
      struct anv_compute_pipeline *pipeline =
         anv_pipeline_to_compute(pipe_state->pipeline);
      merge_state(cs_desc.gfx125.compute_walker,
                  pipeline->gfx125.compute_walker);
   }
#else
   struct GENX(GPGPU_WALKER) walker = {
      GENX(GPGPU_WALKER_header),
   };
   struct GENX(INTERFACE_DESCRIPTOR_DATA) idd = {};
   if (emit_driver_values)
      write_driver_values(&walker, &idd, cmd_buffer);

   GENX(GPGPU_WALKER_pack)(NULL, cs_desc.gfx9.gpgpu_walker, &walker);
   GENX(INTERFACE_DESCRIPTOR_DATA_pack)(NULL,
                                        cs_desc.gfx9.interface_descriptor_data,
                                        &idd);

   if ((layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) == 0) {
      struct anv_compute_pipeline *pipeline =
         anv_pipeline_to_compute(pipe_state->pipeline);
      merge_state(cs_desc.gfx9.gpgpu_walker, pipeline->gfx9.gpgpu_walker);

      assert(sizeof(cs_desc.gfx9.media_vfe_state) ==
             (pipeline->base.batch.next - pipeline->base.batch.start));
      memcpy(cs_desc.gfx9.media_vfe_state, pipeline->batch_data,
             sizeof(cs_desc.gfx9.media_vfe_state));

      merge_state(cs_desc.gfx9.interface_descriptor_data,
                  pipeline->gfx9.interface_descriptor_data);

      const struct brw_cs_prog_data *prog_data =
         get_cs_prog_data(pipeline);
      const struct intel_cs_dispatch_info dispatch =
         brw_cs_get_dispatch_info(device->info, prog_data, NULL);
      cs_desc.gfx9.n_threads = dispatch.threads;
      cs_desc.gfx9.cross_thread_push_size = prog_data->push.cross_thread.size;
      cs_desc.gfx9.per_thread_push_size = prog_data->push.per_thread.size;
      cs_desc.gfx9.subgroup_id_offset =
         offsetof(struct anv_push_constants, cs.subgroup_id) -
         (cs_desc.push_data_offset + prog_data->push.cross_thread.size);
   }
#endif

   struct anv_state cs_desc_state =
      anv_cmd_buffer_alloc_temporary_state(cmd_buffer, sizeof(cs_desc),
                                           GFX_VERx10 >= 125 ? 8 : 64);
   if (cs_desc_state.map == NULL)
      return NULL;
   memcpy(cs_desc_state.map, &cs_desc, sizeof(cs_desc));

   /**/
   struct anv_shader_bin *generate_kernel;
   VkResult ret =
      anv_device_get_internal_shader(device, kernel_name, &generate_kernel);
   if (ret != VK_SUCCESS) {
      anv_batch_set_error(&cmd_buffer->batch, ret);
      return NULL;
   }

   struct anv_simple_shader state = {
      .device               = device,
      .cmd_buffer           = cmd_buffer,
      .dynamic_state_stream = &cmd_buffer->dynamic_state_stream,
      .general_state_stream = &cmd_buffer->general_state_stream,
      .batch                = &cmd_buffer->batch,
      .kernel               = generate_kernel,
      .l3_config            = device->internal_kernels_l3_config,
      .urb_cfg              = &cmd_buffer->state.gfx.urb_cfg,
   };
   genX(emit_simple_shader_init)(&state);

   struct anv_state push_data_state =
      genX(simple_shader_alloc_push)(&state,
                                     sizeof(struct anv_generated_cs_commands_params));
   if (push_data_state.map == NULL)
      return NULL;

   struct anv_generated_cs_commands_params *params = push_data_state.map;
   *params = (struct anv_generated_cs_commands_params) {
      .cmd_addr   = info->preprocessAddress,
      .cmd_stride = layout->cmd_size,

      .data_addr   = info->preprocessAddress +
                     align(layout->cmd_prolog_size +
                           info->maxSequenceCount * layout->cmd_size +
                           layout->cmd_epilog_size, 64),
      .data_stride = layout->data_size,

      .seq_addr   = info->indirectAddress,
      .seq_stride = layout->vk.stride,

      .seq_count_addr = info->sequenceCountAddress,
      .max_seq_count = info->maxSequenceCount,

      .cmd_prolog_size = layout->cmd_prolog_size,
      .data_prolog_size = layout->data_prolog_size,

      .layout_addr = anv_address_physical(
         anv_cmd_buffer_temporary_state_address(cmd_buffer, layout_state)),

      .indirect_set_addr = indirect_set ?
                           anv_address_physical((struct anv_address) {
                                 .bo = indirect_set->bo }) :
                           anv_address_physical(
                              anv_cmd_buffer_temporary_state_address(
                                 cmd_buffer, cs_desc_state)),

      .interface_descriptor_data_addr = anv_address_physical(
         anv_address_add(
            anv_cmd_buffer_temporary_state_address(cmd_buffer, cs_desc_state),
            offsetof(struct anv_gen_cs_indirect_descriptor,
                     gfx9.interface_descriptor_data))),

      .const_addr = anv_address_physical(
         anv_cmd_buffer_temporary_state_address(cmd_buffer,
                                                push_constants_state)),
      .const_size = pipe_state->push_constants_client_size,

      .driver_const_addr = anv_address_physical(
         anv_address_add(
            anv_cmd_buffer_temporary_state_address(cmd_buffer,
                                                   push_constants_state),
            MAX_PUSH_CONSTANTS_SIZE)),

      .flags = cmd_buffer_state->state.conditional_render_enabled ?
               ANV_GENERATED_FLAG_PREDICATED : 0,
   };

   genX(emit_simple_shader_dispatch)(&state,
                                     info->maxSequenceCount,
                                     push_data_state);

   return params;
}

static struct anv_generated_cs_commands_params *
postprocess_cs_sequences(struct anv_cmd_buffer *cmd_buffer,
                         struct anv_indirect_command_layout *layout,
                         struct anv_indirect_execution_set *indirect_set,
                         const VkGeneratedCommandsInfoEXT *info)
{
   struct anv_device *device = cmd_buffer->device;

   /**/
   struct anv_gen_cs_indirect_descriptor *cs_state;
   struct anv_state cs_state_state =
      anv_cmd_buffer_alloc_temporary_state(cmd_buffer, sizeof(*cs_state), 8);
   if (cs_state_state.map == NULL)
      return NULL;
   cs_state = cs_state_state.map;

#if GFX_VERx10 >= 125
   struct GENX(COMPUTE_WALKER) walker = {
      .body.PostSync.MOCS = anv_mocs(device, NULL, 0),
   };
   write_driver_values(&walker, cmd_buffer);

   GENX(COMPUTE_WALKER_pack)(NULL, cs_state->gfx125.compute_walker, &walker);
#else
   struct GENX(INTERFACE_DESCRIPTOR_DATA) idd = {
      .BindingTablePointer =
         cmd_buffer->state.binding_tables[MESA_SHADER_COMPUTE].offset,
      .SamplerStatePointer =
         cmd_buffer->state.samplers[MESA_SHADER_COMPUTE].offset,
   };

   GENX(INTERFACE_DESCRIPTOR_DATA_pack)(
      NULL, cs_state->gfx9.interface_descriptor_data, &idd);
#endif

   /**/
   struct anv_shader_bin *generate_kernel;
   VkResult ret =
      anv_device_get_internal_shader(
         device,
         ANV_INTERNAL_KERNEL_GENERATED_CS_COMMANDS_STEP2_COMPUTE,
         &generate_kernel);
   if (ret != VK_SUCCESS) {
      anv_batch_set_error(&cmd_buffer->batch, ret);
      return NULL;
   }

   struct anv_simple_shader state = {
      .device               = device,
      .cmd_buffer           = cmd_buffer,
      .dynamic_state_stream = &cmd_buffer->dynamic_state_stream,
      .general_state_stream = &cmd_buffer->general_state_stream,
      .batch                = &cmd_buffer->batch,
      .kernel               = generate_kernel,
      .l3_config            = device->internal_kernels_l3_config,
      .urb_cfg              = &cmd_buffer->state.gfx.urb_cfg,
   };
   genX(emit_simple_shader_init)(&state);

   struct anv_state push_data_state =
      genX(simple_shader_alloc_push)(&state,
                                     sizeof(struct anv_generated_cs_commands_params));
   if (push_data_state.map == NULL)
      return NULL;

   struct anv_generated_cs_commands_params *params = push_data_state.map;
   *params = (struct anv_generated_cs_commands_params) {
      .cmd_addr   = info->preprocessAddress,
      .cmd_stride = layout->cmd_size,

      .data_addr   = info->preprocessAddress +
                     align(layout->cmd_prolog_size +
                           info->maxSequenceCount * layout->cmd_size +
                           layout->cmd_epilog_size, 64),
      .data_stride = layout->data_size,

      .seq_count_addr = info->sequenceCountAddress,
      .max_seq_count = info->maxSequenceCount,

      .cmd_prolog_size = layout->cmd_prolog_size,
      .data_prolog_size = layout->data_prolog_size,

      .data_stride = layout->cs_layout.indirect_set.data_offset,

      .indirect_set_addr = anv_address_physical(
         anv_cmd_buffer_temporary_state_address(
            cmd_buffer, cs_state_state)),
   };

   genX(emit_simple_shader_dispatch)(&state,
                                     info->maxSequenceCount,
                                     push_data_state);

   return params;
}

#if GFX_VERx10 >= 125
static struct anv_generated_rt_commands_params *
preprocess_rt_sequences(struct anv_cmd_buffer *cmd_buffer,
                        struct anv_cmd_buffer *cmd_buffer_state,
                        struct anv_indirect_command_layout *layout,
                        struct anv_indirect_execution_set *indirect_set,
                        const VkGeneratedCommandsInfoEXT *info,
                        enum anv_internal_kernel_name kernel_name)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_cmd_ray_tracing_state *rt_state = &cmd_buffer_state->state.rt;
   struct anv_cmd_pipeline_state *pipe_state = &rt_state->base;

   struct anv_state push_constants_state =
      emit_push_constants(cmd_buffer, pipe_state);
   if (push_constants_state.alloc_size == 0)
      return NULL;

   struct anv_state layout_state =
      anv_cmd_buffer_alloc_temporary_state(
         cmd_buffer, sizeof(layout->cs_layout), 8);
   if (layout_state.map == NULL)
      return NULL;
   memcpy(layout_state.map, &layout->cs_layout, sizeof(layout->cs_layout));

   struct anv_state rtdg_state =
      anv_cmd_buffer_alloc_temporary_state(cmd_buffer,
                                           4 * GENX(RT_DISPATCH_GLOBALS_length),
                                           8);
   if (rtdg_state.alloc_size == 0)
      return NULL;

   struct GENX(RT_DISPATCH_GLOBALS) rtdg = {
      .MemBaseAddress     = (struct anv_address) {
         .bo = rt_state->scratch.bo,
         .offset = rt_state->scratch.layout.ray_stack_start,
      },
#if GFX_VERx10 == 300
      .CallStackHandler   = anv_shader_bin_get_handler(
         cmd_buffer->device->rt_trivial_return, 0),
#else
      .CallStackHandler   = anv_shader_bin_get_bsr(
         cmd_buffer->device->rt_trivial_return, 0),
#endif
      .AsyncRTStackSize   = rt_state->scratch.layout.ray_stack_stride / 64,
      .NumDSSRTStacks     = rt_state->scratch.layout.stack_ids_per_dss,
      .MaxBVHLevels       = BRW_RT_MAX_BVH_LEVELS,
      .Flags              = RT_DEPTH_TEST_LESS_EQUAL,
      .SWStackSize        = rt_state->scratch.layout.sw_stack_size / 64,
   };
   GENX(RT_DISPATCH_GLOBALS_pack)(NULL, rtdg_state.map, &rtdg);

   struct anv_state compute_walker_state =
      anv_cmd_buffer_alloc_temporary_state(cmd_buffer,
                                           4 * GENX(COMPUTE_WALKER_length),
                                           8);

   const struct brw_cs_prog_data *cs_prog_data =
      brw_cs_prog_data_const(device->rt_trampoline->prog_data);
   struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(device->info, cs_prog_data, NULL);
   struct GENX(COMPUTE_WALKER) cw = {
      GENX(COMPUTE_WALKER_header),
      .body = {
         .SIMDSize                       = dispatch.simd_size / 16,
         .MessageSIMD                    = dispatch.simd_size / 16,
         .ExecutionMask                  = 0xff,
         .EmitInlineParameter            = true,
         .PostSync.MOCS                  = anv_mocs(cmd_buffer->device, NULL, 0),
         .InterfaceDescriptor            = (struct GENX(INTERFACE_DESCRIPTOR_DATA)) {
            .NumberofThreadsinGPGPUThreadGroup = 1,
            .BTDMode                           = true,
#if INTEL_NEEDS_WA_14017794102
            .ThreadPreemption                  = false,
#endif
         },
      },
   };
   GENX(COMPUTE_WALKER_pack)(NULL, compute_walker_state.map, &cw);

   /**/
   struct anv_shader_bin *generate_kernel;
   VkResult ret =
      anv_device_get_internal_shader(device, kernel_name, &generate_kernel);
   if (ret != VK_SUCCESS) {
      anv_batch_set_error(&cmd_buffer->batch, ret);
      return NULL;
   }

   struct anv_simple_shader state = {
      .device               = device,
      .cmd_buffer           = cmd_buffer,
      .dynamic_state_stream = &cmd_buffer->dynamic_state_stream,
      .general_state_stream = &cmd_buffer->general_state_stream,
      .batch                = &cmd_buffer->batch,
      .kernel               = generate_kernel,
      .l3_config            = device->internal_kernels_l3_config,
      .urb_cfg              = &cmd_buffer->state.gfx.urb_cfg,
   };
   genX(emit_simple_shader_init)(&state);

   struct anv_state push_data_state =
      genX(simple_shader_alloc_push)(&state,
                                     sizeof(struct anv_generated_rt_commands_params));
   if (push_data_state.map == NULL)
      return NULL;

   struct anv_generated_rt_commands_params *params = push_data_state.map;
   *params = (struct anv_generated_rt_commands_params) {
      .cmd_addr   = info->preprocessAddress,
      .cmd_stride = layout->cmd_size,

      .data_addr   = info->preprocessAddress +
                     align(layout->cmd_prolog_size +
                           info->maxSequenceCount * layout->cmd_size +
                           layout->cmd_epilog_size, 64),
      .data_stride = layout->data_size,

      .seq_addr   = info->indirectAddress,
      .seq_stride = layout->vk.stride,

      .seq_count_addr = info->sequenceCountAddress,
      .max_seq_count = info->maxSequenceCount,

      .cmd_prolog_size = layout->cmd_prolog_size,
      .data_prolog_size = layout->data_prolog_size,

      .layout_addr = anv_address_physical(
         anv_cmd_buffer_temporary_state_address(cmd_buffer, layout_state)),

      .compute_walker_addr = anv_address_physical(
         anv_cmd_buffer_temporary_state_address(
            cmd_buffer, compute_walker_state)),

      .rtdg_global_addr = anv_address_physical(
         anv_cmd_buffer_temporary_state_address(cmd_buffer, rtdg_state)),

      .const_addr = anv_address_physical(
         anv_cmd_buffer_temporary_state_address(cmd_buffer,
                                                push_constants_state)),
      .const_size = pipe_state->push_constants_client_size,

      .driver_const_addr = anv_address_physical(
         anv_address_add(
            anv_cmd_buffer_temporary_state_address(cmd_buffer,
                                                   push_constants_state),
            MAX_PUSH_CONSTANTS_SIZE)),

      .flags = cmd_buffer_state->state.conditional_render_enabled ?
               ANV_GENERATED_FLAG_PREDICATED : 0,
   };

   genX(emit_simple_shader_dispatch)(&state,
                                     info->maxSequenceCount,
                                     push_data_state);

   return params;
}
#endif /* GFX_VERx10 >= 125 */

void genX(CmdPreprocessGeneratedCommandsEXT)(
    VkCommandBuffer                             commandBuffer,
    const VkGeneratedCommandsInfoEXT*           pGeneratedCommandsInfo,
    VkCommandBuffer                             stateCommandBuffer)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer_state, stateCommandBuffer);
   ANV_FROM_HANDLE(anv_indirect_command_layout, layout,
                   pGeneratedCommandsInfo->indirectCommandsLayout);
   ANV_FROM_HANDLE(anv_indirect_execution_set, indirect_set,
                   pGeneratedCommandsInfo->indirectExecutionSet);

   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

   if (cmd_buffer->state.current_pipeline == UINT32_MAX) {
      if (anv_cmd_buffer_is_compute_queue(cmd_buffer))
         genX(flush_pipeline_select_gpgpu)(cmd_buffer);
      else
         genX(flush_pipeline_select_3d)(cmd_buffer);
   }

   if (indirect_set) {
      anv_reloc_list_add_bo(cmd_buffer->batch.relocs, indirect_set->bo);
      anv_reloc_list_append(cmd_buffer->batch.relocs, &indirect_set->relocs);
   }

   switch (layout->bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      preprocess_gfx_sequences(cmd_buffer, cmd_buffer_state,
                               layout, indirect_set,
                               pGeneratedCommandsInfo,
                               anv_internal_kernel_variant(
                                  cmd_buffer, GENERATED_GFX_COMMANDS_STEP1));
      break;

   case VK_PIPELINE_BIND_POINT_COMPUTE:
      preprocess_cs_sequences(cmd_buffer, cmd_buffer_state,
                              layout, indirect_set,
                              pGeneratedCommandsInfo,
                              anv_internal_kernel_variant(
                                 cmd_buffer, GENERATED_CS_COMMANDS_STEP1),
                              false /* emit_driver_values */);
      break;

#if GFX_VERx10 >= 125
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:
      preprocess_rt_sequences(cmd_buffer, cmd_buffer_state,
                              layout, indirect_set,
                              pGeneratedCommandsInfo,
                              anv_internal_kernel_variant(
                                 cmd_buffer, GENERATED_RT_COMMANDS));
      break;
#endif

   default:
      unreachable("Invalid layout bind point");
      break;
   }
}

void genX(CmdExecuteGeneratedCommandsEXT)(
   VkCommandBuffer                             commandBuffer,
   VkBool32                                    isPreprocessed,
   const VkGeneratedCommandsInfoEXT*           pGeneratedCommandsInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_indirect_command_layout, layout,
                   pGeneratedCommandsInfo->indirectCommandsLayout);
   ANV_FROM_HANDLE(anv_indirect_execution_set, indirect_set,
                   pGeneratedCommandsInfo->indirectExecutionSet);
   struct anv_device *device = cmd_buffer->device;
   const struct intel_device_info *devinfo = device->info;

   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

   if (indirect_set) {
      anv_reloc_list_add_bo(cmd_buffer->batch.relocs, indirect_set->bo);
      anv_reloc_list_append(cmd_buffer->batch.relocs, &indirect_set->relocs);
   }

   struct mi_builder b;
   mi_builder_init(&b, devinfo, &cmd_buffer->batch);
   struct mi_goto_target t = MI_GOTO_TARGET_INIT;

   switch (layout->bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS: {
      struct anv_generated_gfx_commands_params *params = NULL;
      uint64_t *return_addr_loc = NULL;
      if (!isPreprocessed) {
         params = preprocess_gfx_sequences(
            cmd_buffer, cmd_buffer, layout, indirect_set,
            pGeneratedCommandsInfo,
            ANV_INTERNAL_KERNEL_GENERATED_GFX_COMMANDS_STEP1_FRAGMENT);
      } else {
         return_addr_loc =
            anv_batch_emitn(&cmd_buffer->batch,
                            GENX(MI_STORE_DATA_IMM_length) + 1 /* QWord write */,
                            GENX(MI_STORE_DATA_IMM),
#if GFX_VER >= 12
                            .ForceWriteCompletionCheck = true,
#endif
                            .Address = anv_address_add(
                               anv_address_from_u64(pGeneratedCommandsInfo->preprocessAddress),
                               GENX(MI_STORE_DATA_IMM_ImmediateData_start) / 8)) +
            GENX(MI_STORE_DATA_IMM_ImmediateData_start) / 8;
      }

      genX(cmd_buffer_flush_indirect_gfx_state)(cmd_buffer,
                                                layout,
                                                indirect_set);

      if (pGeneratedCommandsInfo->sequenceCountAddress != 0) {
         struct anv_address seq_count_addr =
            anv_address_from_u64(pGeneratedCommandsInfo->sequenceCountAddress);

         const uint32_t mocs = anv_mocs_for_address(device, &seq_count_addr);
         mi_builder_set_mocs(&b, mocs);

         mi_goto_if(&b, mi_ieq(&b, mi_mem32(seq_count_addr), mi_imm(0)), &t);
      }

      if (cmd_buffer->state.conditional_render_enabled)
         genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

      /* If a shader runs, flush the data to make it visible to CS. */
      if (params) {
         anv_add_pending_pipe_bits(cmd_buffer,
                                   ANV_PIPE_DATA_CACHE_FLUSH_BIT |
                                   ANV_PIPE_CS_STALL_BIT,
                                   "after generated commands");
         genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);
      }

#if GFX_VER >= 12
      /* Prior to Gfx12 we cannot disable the CS prefetch but it doesn't matter
       * as the prefetch shouldn't follow the MI_BATCH_BUFFER_START.
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_ARB_CHECK), arb) {
         arb.PreParserDisableMask = true;
         arb.PreParserDisable = true;
      }
#endif

      /* Jump into the process buffer */
      struct anv_address cmd_addr =
         anv_address_from_u64(pGeneratedCommandsInfo->preprocessAddress);
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_BATCH_BUFFER_START), bbs) {
         bbs.AddressSpaceIndicator   = ASI_PPGTT;
         bbs.SecondLevelBatchBuffer  = Firstlevelbatch;
         bbs.BatchBufferStartAddress = cmd_addr;
      }

      /* If we used a shader to generate some commands, it can generate the
       * return MI_BATCH_BUFFER_START. Otherwise we edit the
       * MI_BATCH_BUFFER_START address field from CS.
       */
      struct anv_address return_addr = anv_batch_current_address(&cmd_buffer->batch);
      if (params) {
         params->return_addr = anv_address_physical(return_addr);
      } else {
         assert(return_addr_loc);
         *return_addr_loc = anv_address_physical(return_addr);
      }

      if (pGeneratedCommandsInfo->sequenceCountAddress != 0)
         mi_goto_target(&b, &t);

      /* Dirty the bits affected by the executed commands */
      if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IB))
         cmd_buffer->state.gfx.dirty |= ANV_CMD_DIRTY_INDEX_BUFFER;
      if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_VB))
          cmd_buffer->state.gfx.vb_dirty |= ~0;
      if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_PC))
         cmd_buffer->state.push_constants_dirty |= ANV_GRAPHICS_STAGE_BITS;
      if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES))
         cmd_buffer->state.gfx.dirty |= ANV_CMD_DIRTY_PIPELINE;

      cmd_buffer->state.dgc_states |= ANV_DGC_STATE_GRAPHIC;

      break;
   }

   case VK_PIPELINE_BIND_POINT_COMPUTE: {
      struct anv_cmd_compute_state *comp_state = &cmd_buffer->state.compute;
      struct anv_cmd_pipeline_state *pipe_state = &comp_state->base;

      genX(flush_pipeline_select_gpgpu)(cmd_buffer);

      genX(flush_descriptor_buffers)(cmd_buffer, pipe_state,
                                     VK_SHADER_STAGE_COMPUTE_BIT);
      if ((layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) == 0) {
         struct anv_compute_pipeline *pipeline =
            anv_pipeline_to_compute(comp_state->base.pipeline);

         cmd_buffer->state.descriptors_dirty |=
            genX(cmd_buffer_flush_push_descriptors)(cmd_buffer,
                                                    pipe_state,
                                                    &pipeline->base,
                                                    &pipeline->base.layout);

         if ((cmd_buffer->state.descriptors_dirty & VK_SHADER_STAGE_COMPUTE_BIT) ||
             cmd_buffer->state.compute.pipeline_dirty) {
            genX(cmd_buffer_flush_shader_descriptor_sets)(cmd_buffer,
                                                          &cmd_buffer->state.compute.base,
                                                          VK_SHADER_STAGE_COMPUTE_BIT,
                                                          &pipeline->cs, 1);
         }
         cmd_buffer->state.descriptors_dirty &= ~VK_SHADER_STAGE_COMPUTE_BIT;
      } else {
         cmd_buffer->state.descriptors_dirty &=
            ~genX(cmd_buffer_flush_indirect_set_descriptors)(cmd_buffer,
                                                             pipe_state,
                                                             indirect_set,
                                                             VK_SHADER_STAGE_COMPUTE_BIT);
      }

      if (pGeneratedCommandsInfo->sequenceCountAddress != 0) {
         struct anv_address seq_count_addr =
            anv_address_from_u64(pGeneratedCommandsInfo->sequenceCountAddress);

         const uint32_t mocs = anv_mocs_for_address(device, &seq_count_addr);
         mi_builder_set_mocs(&b, mocs);

         mi_goto_if(&b, mi_ieq(&b, mi_mem32(seq_count_addr), mi_imm(0)), &t);
      }

      /* Do we need to go an edit the binding table offsets? */
      bool need_post_process =
         (devinfo->verx10 >= 125 &&
          (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) == 0) ||
         (devinfo->verx10 <= 120 &&
          (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) &&
          indirect_set->layout_type ==
          ANV_PIPELINE_DESCRIPTOR_SET_LAYOUT_TYPE_BUFFER &&
          (indirect_set->bind_map->surface_count ||
           indirect_set->bind_map->sampler_count));

      struct anv_generated_cs_commands_params *params = NULL;
      uint64_t *return_addr_loc = NULL;
      if (!isPreprocessed) {
         params = preprocess_cs_sequences(
            cmd_buffer, cmd_buffer, layout,
            indirect_set, pGeneratedCommandsInfo,
            ANV_INTERNAL_KERNEL_GENERATED_CS_COMMANDS_STEP1_COMPUTE,
            true /* emit_driver_values */);
      } else if (need_post_process) {
         /* For pipelines not compiled with the
          * VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT, we might be using
          * the binding table and unfortunately the binding table offset needs
          * to go in the COMPUTE_WALKER command and we only know the value
          * when we flush it here.
          *
          * TODO: make all compute shaders fully bindless on Gfx12.5+ ?
          */
         params = postprocess_cs_sequences(cmd_buffer, layout, indirect_set,
                                           pGeneratedCommandsInfo);
      } else {
         return_addr_loc =
            anv_batch_emitn(&cmd_buffer->batch,
                            GENX(MI_STORE_DATA_IMM_length) + 1 /* QWord write */,
                            GENX(MI_STORE_DATA_IMM),
#if GFX_VER >= 12
                            .ForceWriteCompletionCheck = true,
#endif
                            .Address = anv_address_add(
                               anv_address_from_u64(pGeneratedCommandsInfo->preprocessAddress),
                               GENX(MI_STORE_DATA_IMM_ImmediateData_start) / 8)) +
            GENX(MI_STORE_DATA_IMM_ImmediateData_start) / 8;
      }

#if GFX_VERx10 >= 125
      if ((layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) == 0) {
         struct anv_compute_pipeline *pipeline =
            anv_pipeline_to_compute(comp_state->base.pipeline);
         const struct brw_cs_prog_data *prog_data = get_cs_prog_data(pipeline);
         genX(cmd_buffer_ensure_cfe_state)(cmd_buffer, prog_data->base.total_scratch);
      } else {
         genX(cmd_buffer_ensure_cfe_state)(cmd_buffer, indirect_set->max_scratch);
      }
#endif

      /* If a shader runs, flush the data to make it visible to CS. */
      if (params) {
         anv_add_pending_pipe_bits(cmd_buffer,
                                   ANV_PIPE_DATA_CACHE_FLUSH_BIT |
                                   ANV_PIPE_CS_STALL_BIT,
                                   "after generated commands");
         genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);
      }

#if GFX_VERx10 < 125
      /* Prior to Gfx12.5 we can emit the shader */
      if ((layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) == 0) {
         struct anv_compute_pipeline *pipeline =
            anv_pipeline_to_compute(pipe_state->pipeline);

         anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->base.batch);

         uint32_t iface_desc_data_dw[GENX(INTERFACE_DESCRIPTOR_DATA_length)];
         struct GENX(INTERFACE_DESCRIPTOR_DATA) desc = {
            .BindingTablePointer =
               cmd_buffer->state.binding_tables[MESA_SHADER_COMPUTE].offset,
            .SamplerStatePointer =
               cmd_buffer->state.samplers[MESA_SHADER_COMPUTE].offset,
         };
         GENX(INTERFACE_DESCRIPTOR_DATA_pack)(NULL, iface_desc_data_dw, &desc);

         struct anv_state state =
            anv_cmd_buffer_merge_dynamic(cmd_buffer, iface_desc_data_dw,
                                         pipeline->gfx9.interface_descriptor_data,
                                         GENX(INTERFACE_DESCRIPTOR_DATA_length),
                                         64);

         uint32_t size = GENX(INTERFACE_DESCRIPTOR_DATA_length) * sizeof(uint32_t);
         anv_batch_emit(&cmd_buffer->batch,
                        GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD), mid) {
            mid.InterfaceDescriptorTotalLength        = size;
            mid.InterfaceDescriptorDataStartAddress   = state.offset;
         }
      }
#endif

      if (cmd_buffer->state.conditional_render_enabled)
         genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

#if GFX_VER >= 12
      /* Prior to Gfx12 we cannot disable the CS prefetch but it doesn't matter
       * as the prefetch shouldn't follow the MI_BATCH_BUFFER_START.
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_ARB_CHECK), arb) {
         arb.PreParserDisableMask = true;
         arb.PreParserDisable = true;
      }
#endif

      /* Jump into the process buffer */
      struct anv_address cmd_addr =
         anv_address_from_u64(pGeneratedCommandsInfo->preprocessAddress);
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_BATCH_BUFFER_START), bbs) {
         bbs.AddressSpaceIndicator   = ASI_PPGTT;
         bbs.SecondLevelBatchBuffer  = Firstlevelbatch;
         bbs.BatchBufferStartAddress = cmd_addr;
      }

      /* If we used a shader to generate some commands, it can generate the
       * return MI_BATCH_BUFFER_START. Otherwise we edit the
       * MI_BATCH_BUFFER_START address field from CS.
       */
      struct anv_address return_addr = anv_batch_current_address(&cmd_buffer->batch);
      if (params) {
         params->return_addr = anv_address_physical(return_addr);
      } else {
         assert(return_addr_loc);
         *return_addr_loc = anv_address_physical(return_addr);
      }

      if (pGeneratedCommandsInfo->sequenceCountAddress != 0)
         mi_goto_target(&b, &t);

      /* Dirty the bits affected by the executed commands */
      if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES))
         cmd_buffer->state.compute.pipeline_dirty = true;
      if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_PC))
         cmd_buffer->state.push_constants_dirty |= VK_SHADER_STAGE_COMPUTE_BIT;

      cmd_buffer->state.dgc_states |= ANV_DGC_STATE_COMPUTE;

      break;
   }

#if GFX_VERx10 >= 125
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR: {
      struct anv_cmd_ray_tracing_state *rt_state = &cmd_buffer->state.rt;
      struct anv_cmd_pipeline_state *pipe_state = &rt_state->base;

      genX(flush_pipeline_select_gpgpu)(cmd_buffer);

      genX(flush_descriptor_buffers)(cmd_buffer, pipe_state, ANV_RT_STAGE_BITS);
      if ((layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) == 0) {
         struct anv_ray_tracing_pipeline *pipeline =
            anv_pipeline_to_ray_tracing(rt_state->base.pipeline);

         cmd_buffer->state.descriptors_dirty |=
            genX(cmd_buffer_flush_push_descriptors)(cmd_buffer,
                                                    &cmd_buffer->state.rt.base,
                                                    &pipeline->base,
                                                    &pipeline->base.layout);
      } else {
         /* cmd_buffer->state.descriptors_dirty &= */
         /*    ~genX(cmd_buffer_flush_indirect_set_descriptors)(cmd_buffer, */
         /*                                                     pipe_state, */
         /*                                                     indirect_set, */
         /*                                                     VK_SHADER_STAGE_COMPUTE_BIT); */
      }

      if (pGeneratedCommandsInfo->sequenceCountAddress != 0) {
         struct anv_address seq_count_addr =
            anv_address_from_u64(pGeneratedCommandsInfo->sequenceCountAddress);

         const uint32_t mocs = anv_mocs_for_address(device, &seq_count_addr);
         mi_builder_set_mocs(&b, mocs);

         mi_goto_if(&b, mi_ieq(&b, mi_mem32(seq_count_addr), mi_imm(0)), &t);
      }

      struct anv_generated_rt_commands_params *params = NULL;
      uint64_t *return_addr_loc = NULL;
      if (!isPreprocessed) {
         params = preprocess_rt_sequences(
            cmd_buffer, cmd_buffer, layout,
            indirect_set, pGeneratedCommandsInfo,
            ANV_INTERNAL_KERNEL_GENERATED_RT_COMMANDS_COMPUTE);
      } else {
         return_addr_loc =
            anv_batch_emitn(&cmd_buffer->batch,
                            GENX(MI_STORE_DATA_IMM_length) + 1 /* QWord write */,
                            GENX(MI_STORE_DATA_IMM),
                            .ForceWriteCompletionCheck = true,
                            .Address = anv_address_add(
                               anv_address_from_u64(pGeneratedCommandsInfo->preprocessAddress),
                               GENX(MI_STORE_DATA_IMM_ImmediateData_start) / 8)) +
            GENX(MI_STORE_DATA_IMM_ImmediateData_start) / 8;
      }

      uint32_t scratch_size = (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IES)) ?
         indirect_set->max_scratch :
         anv_pipeline_to_ray_tracing(rt_state->base.pipeline)->base.scratch_size;

      genX(cmd_buffer_ensure_cfe_state)(cmd_buffer, scratch_size);

      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_BTD), btd) {
         /* TODO: This is the timeout after which the bucketed thread
          *       dispatcher will kick off a wave of threads. We go with the
          *       lowest value for now. It could be tweaked on a per
          *       application basis (drirc).
          */
         btd.DispatchTimeoutCounter = _64clocks;
         /* BSpec 43851: "This field must be programmed to 6h i.e. memory
          *               backed buffer must be 128KB."
          */
         btd.PerDSSMemoryBackedBufferSize = 6;
         btd.MemoryBackedBufferBasePointer = (struct anv_address) {
            .bo = device->btd_fifo_bo
         };
         if (scratch_size > 0) {
            struct anv_bo *scratch_bo =
               anv_scratch_pool_alloc(device,
                                      &device->scratch_pool,
                                      MESA_SHADER_COMPUTE, scratch_size);
            anv_reloc_list_add_bo(cmd_buffer->batch.relocs,
                                  scratch_bo);
            uint32_t scratch_surf =
               anv_scratch_pool_get_surf(device, &device->scratch_pool, scratch_size);
            btd.ScratchSpaceBuffer = scratch_surf >> ANV_SCRATCH_SPACE_SHIFT(GFX_VER);
         }
#if INTEL_NEEDS_WA_14017794102
         btd.BTDMidthreadpreemption = false;
#endif
      }

      anv_reloc_list_add_bo(cmd_buffer->batch.relocs, rt_state->scratch.bo);
      anv_reloc_list_add_bo(cmd_buffer->batch.relocs, device->btd_fifo_bo);

      /* If a shader runs, flush the data to make it visible to CS. */
      if (params) {
         anv_add_pending_pipe_bits(cmd_buffer,
                                   ANV_PIPE_DATA_CACHE_FLUSH_BIT |
                                   ANV_PIPE_CS_STALL_BIT,
                                   "after generated commands");
         genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);
      }

      if (cmd_buffer->state.conditional_render_enabled)
         genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

      anv_batch_emit(&cmd_buffer->batch, GENX(MI_ARB_CHECK), arb) {
         arb.PreParserDisableMask = true;
         arb.PreParserDisable = true;
      }

      /* Jump into the process buffer */
      struct anv_address cmd_addr =
         anv_address_from_u64(pGeneratedCommandsInfo->preprocessAddress);
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_BATCH_BUFFER_START), bbs) {
         bbs.AddressSpaceIndicator   = ASI_PPGTT;
         bbs.SecondLevelBatchBuffer  = Firstlevelbatch;
         bbs.BatchBufferStartAddress = cmd_addr;
      }

      /* If we used a shader to generate some commands, it can generate the
       * return MI_BATCH_BUFFER_START. Otherwise we edit the
       * MI_BATCH_BUFFER_START address field from CS.
       */
      struct anv_address return_addr = anv_batch_current_address(&cmd_buffer->batch);
      if (params) {
         params->return_addr = anv_address_physical(return_addr);
      } else {
         assert(return_addr_loc);
         *return_addr_loc = anv_address_physical(return_addr);
      }

      if (pGeneratedCommandsInfo->sequenceCountAddress != 0)
         mi_goto_target(&b, &t);

      break;
   }
#endif

   default:
      unreachable("Invalid layout binding point");
   }
}
