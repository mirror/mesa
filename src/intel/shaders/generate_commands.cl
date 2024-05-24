/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "libintel_shaders.h"
#include "dev/intel_wa.h"

#define HAS_STAGE(descriptor, stage) \
   (((descriptor)->active_stages & \
     BITFIELD_BIT(ANV_GENERATED_COMMAND_STAGE_##stage)) != 0)

#if GFX_VER >= 11

static void
merge_dwords(global void *dst, global void *src1, global void *src2, uint32_t n_dwords)
{
   for (uint32_t i = 0; i < n_dwords; i += 4) {
      if (n_dwords - i >= 4) {
         *(global uint4 *)(dst + i * 4) = *(global uint4 *)(src1 + i * 4) |
                                          *(global uint4 *)(src2 + i * 4) ;
      } else if (n_dwords - i >= 3) {
         *(global uint3 *)(dst + i * 4) = *(global uint3 *)(src1 + i * 4) |
                                          *(global uint3 *)(src2 + i * 4) ;
      } else if (n_dwords - i >= 2) {
         *(global uint2 *)(dst + i * 4) = *(global uint2 *)(src1 + i * 4) |
                                          *(global uint2 *)(src2 + i * 4) ;
      } else {
         *(global uint *)(dst + i * 4) = *(global uint *)(src1 + i * 4) |
                                         *(global uint *)(src2 + i * 4) ;
      }
   }
}

static VkPolygonMode
raster_polygon_mode(global struct anv_gen_gfx_indirect_descriptor *descriptor,
                    uint32_t stages,
                    VkPolygonMode polygon_mode,
                    VkPrimitiveTopology primitive_topology)
{
   if (stages & (BITFIELD_BIT(ANV_GENERATED_COMMAND_STAGE_MESH) |
                 BITFIELD_BIT(ANV_GENERATED_COMMAND_STAGE_GEOMETRY) |
                 BITFIELD_BIT(ANV_GENERATED_COMMAND_STAGE_TESS_EVAL))) {
      return descriptor->last_preraster_topology != VK_POLYGON_MODE_FILL ?
             descriptor->last_preraster_topology :
             polygon_mode;
   } else {
      switch (primitive_topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
         return VK_POLYGON_MODE_POINT;

      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
         return VK_POLYGON_MODE_LINE;

      default:
         return polygon_mode;
      }
   }
}

static bool
rasterization_aa_mode(VkPolygonMode raster_mode,
                      VkLineRasterizationMode line_mode)
{
   if (raster_mode == VK_POLYGON_MODE_LINE &&
       line_mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR)
      return true;
   return false;
}

#if GFX_VER >= 12
static uint32_t
write_3DSTATE_CONSTANT_ALL(global void *dst_ptr,
                           global void *push_data_addr,
                           global struct anv_gen_push_stage_state *stage_state,
                           global struct anv_gen_gfx_state *state,
                           uint32_t stage_enabled)
{
   uint32_t n_slots = stage_state->n_slots;
   struct GENX(3DSTATE_CONSTANT_ALL) v = {
      GENX(3DSTATE_CONSTANT_ALL_header),
      .DWordLength        = GENX(3DSTATE_CONSTANT_ALL_length) -
                            GENX(3DSTATE_CONSTANT_ALL_length_bias) +
                            n_slots * GENX(3DSTATE_CONSTANT_ALL_DATA_length),
      .ShaderUpdateEnable = stage_enabled,
      .MOCS               = state->layout.push_constants.mocs,
      .PointerBufferMask  = (1u << n_slots) - 1,
   };
   GENX(3DSTATE_CONSTANT_ALL_pack)(dst_ptr, &v);

   dst_ptr += GENX(3DSTATE_CONSTANT_ALL_length) * 4;

   for (uint32_t i = 0; i < n_slots; i++) {
      struct anv_gen_push_stage_slot slot = stage_state->slots[i];

      if (slot.type == ANV_GEN_PUSH_SLOT_TYPE_PUSH_CONSTANTS) {
         struct GENX(3DSTATE_CONSTANT_ALL_DATA) vd = {
            .ConstantBufferReadLength = slot.push_data_size / 32,
            .PointerToConstantBuffer  = (uint64_t) push_data_addr + slot.push_data_offset,
         };
         GENX(3DSTATE_CONSTANT_ALL_DATA_pack)(dst_ptr, &vd);
      } else {
         struct GENX(3DSTATE_CONSTANT_ALL_DATA) vd = {
            .ConstantBufferReadLength = slot.push_data_size / 32,
            .PointerToConstantBuffer  = state->push_constants.addresses[i],
         };
         GENX(3DSTATE_CONSTANT_ALL_DATA_pack)(dst_ptr, &vd);
      }

      dst_ptr += GENX(3DSTATE_CONSTANT_ALL_DATA_length) * 4;
   }

   return 4 * (GENX(3DSTATE_CONSTANT_ALL_length) +
               n_slots * GENX(3DSTATE_CONSTANT_ALL_DATA_length));
}
#else
static uint64_t
pc_slot_address(global struct anv_gen_push_stage_slot *slot,
                global uint64_t *slot_address,
                global void *push_data_addr)
{
   if (slot->type == ANV_GEN_PUSH_SLOT_TYPE_PUSH_CONSTANTS) {
      return (uint64_t) push_data_addr + slot->push_data_offset;
   } else {
      return *slot_address;
   }
}

static uint32_t
write_3DSTATE_CONSTANT_XS(global void *dst_ptr,
                          global void *push_data_addr,
                          global struct anv_gen_push_stage_state *stage_state,
                          global struct anv_gen_gfx_state *state,
                          uint32_t stage_enabled)
{
   uint32_t opcode;
   if (stage_enabled & BITFIELD_BIT(ANV_GENERATED_COMMAND_STAGE_VERTEX))
      opcode = 21;
   else if (stage_enabled & BITFIELD_BIT(ANV_GENERATED_COMMAND_STAGE_TESS_CTRL))
      opcode = 25;
   else if (stage_enabled & BITFIELD_BIT(ANV_GENERATED_COMMAND_STAGE_TESS_EVAL))
      opcode = 26;
   else if (stage_enabled & BITFIELD_BIT(ANV_GENERATED_COMMAND_STAGE_GEOMETRY))
      opcode = 22;
   else
      opcode = 23;

   struct GENX(3DSTATE_CONSTANT_VS) v = {
      GENX(3DSTATE_CONSTANT_VS_header),
      ._3DCommandSubOpcode = opcode,
      .ConstantBody = {
         .Buffer = {
            pc_slot_address(&stage_state->slots[0],
                            &state->push_constants.addresses[0],
                            push_data_addr),
            pc_slot_address(&stage_state->slots[1],
                            &state->push_constants.addresses[1],
                            push_data_addr),
            pc_slot_address(&stage_state->slots[2],
                            &state->push_constants.addresses[2],
                            push_data_addr),
            pc_slot_address(&stage_state->slots[3],
                            &state->push_constants.addresses[3],
                            push_data_addr),
         },
         .ReadLength = {
            stage_state->slots[0].push_data_size / 32,
            stage_state->slots[1].push_data_size / 32,
            stage_state->slots[2].push_data_size / 32,
            stage_state->slots[3].push_data_size / 32,
         },
      },
   };
   GENX(3DSTATE_CONSTANT_VS_pack)(dst_ptr, &v);

   return 4 * GENX(3DSTATE_CONSTANT_VS_length);
}
#endif

static void
write_app_push_constant_data(global void *push_data_ptr,
                             global struct anv_gen_push_layout *pc_layout,
                             global void *seq_ptr,
                             global void *template_ptr,
                             uint32_t template_size,
                             uint32_t seq_idx)
{
   uint32_t num_entries = pc_layout->num_entries;

   /* Copy the push constant data prepared on the CPU into the preprocess
    * buffer. Try to minimize the amount if the first entry partially or
    * entirely overlaps.
    */
   if (template_size > 0) {
      if (num_entries > 0) {
         struct anv_gen_push_entry first_entry = pc_layout->entries[0];
         uint32_t entry_end = first_entry.push_offset + first_entry.size;
         if (first_entry.push_offset > 0) {
            genX(copy_data)(push_data_ptr, template_ptr,
                            first_entry.push_offset);
         }
         if (entry_end < template_size) {
            genX(copy_data)(push_data_ptr + entry_end,
                            template_ptr + entry_end,
                            template_size - entry_end);
         }
      } else {
         genX(copy_data)(push_data_ptr, template_ptr, template_size);
      }
   }

   /* Update push constant data using the indirect stream */
   for (uint32_t i = 0; i < num_entries; i++) {
      struct anv_gen_push_entry entry = pc_layout->entries[i];
      global void *pc_ptr = seq_ptr + entry.seq_offset;
      genX(copy_data)(push_data_ptr + entry.push_offset,
                      pc_ptr, entry.size);
   }

   if (pc_layout->seq_id_active)
      *(uint32_t *)(push_data_ptr + pc_layout->seq_id_offset) = seq_idx;
}

static void
write_drv_push_constant_data(global void *driver_data_ptr,
                             global void *driver_template_ptr,
                             uint32_t size)
{
   genX(copy_data)(driver_data_ptr, driver_template_ptr, size);
}

static void
write_gfx_drv_push_constant_data(global void *driver_data_ptr,
                                 global void *driver_template_ptr,
                                 uint32_t size,
                                 uint32_t tcs_input_vertices,
                                 uint32_t fs_msaa_flags)
{
   genX(copy_data)(driver_data_ptr, driver_template_ptr, size);

   global uint32_t *tcs_input_vertices_ptr =
      driver_data_ptr +
      (ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_PCP_OFFSET -
       ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE);
   *tcs_input_vertices_ptr = tcs_input_vertices;

   global uint32_t *fs_msaa_flags_ptr =
      driver_data_ptr +
      (ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_FS_MSAA_FLAGS_OFFSET -
       ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE);
   *fs_msaa_flags_ptr = fs_msaa_flags;
}

static uint32_t
write_gfx_push_constant_commands(global void *push_cmd_ptr,
                                 global void *push_data_ptr,
                                 global struct anv_gen_gfx_state *state,
                                 global struct anv_gen_gfx_indirect_descriptor *descriptor)
{
   uint32_t cmd_offset = 0;
   uint32_t push_stages = descriptor->push_constants.active_stages;
   for (uint32_t s = ANV_GENERATED_COMMAND_STAGE_VERTEX;
        s <= ANV_GENERATED_COMMAND_STAGE_FRAGMENT && push_stages != 0; s++) {
      if ((BITFIELD_BIT(s) & push_stages) == 0)
         continue;

      global struct anv_gen_push_stage_state *stage_state =
         &descriptor->push_constants.stages[s];

#if GFX_VER >= 12
      cmd_offset += write_3DSTATE_CONSTANT_ALL(push_cmd_ptr + cmd_offset,
                                               push_data_ptr,
                                               stage_state,
                                               state,
                                               BITFIELD_BIT(s));
#else
      cmd_offset += write_3DSTATE_CONSTANT_XS(push_cmd_ptr + cmd_offset,
                                              push_data_ptr,
                                              stage_state,
                                               state,
                                              BITFIELD_BIT(s));
#endif

      push_stages &= ~BITFIELD_BIT(s);
   }

#if GFX_VERx10 >= 125
   /* Mesh & Task use a single combined push constants + driver constants
    * pointer
    */
   if (push_stages & BITFIELD_BIT(ANV_GENERATED_COMMAND_STAGE_TASK)) {
      uint64_t data_ptr = (uint64_t) push_data_ptr +
         descriptor->push_constants.stages[
            ANV_GENERATED_COMMAND_STAGE_TASK].slots[0].push_data_offset;
      struct GENX(3DSTATE_TASK_SHADER_DATA) data = {
         GENX(3DSTATE_TASK_SHADER_DATA_header),
         .InlineData = {
            data_ptr & 0xffffffff,
            data_ptr >> 32,
         },
      };
      GENX(3DSTATE_TASK_SHADER_DATA_pack)(push_cmd_ptr + cmd_offset, &data);
      cmd_offset += GENX(3DSTATE_TASK_SHADER_DATA_length) * 4;
   }
   if (push_stages & BITFIELD_BIT(ANV_GENERATED_COMMAND_STAGE_MESH)) {
      uint64_t data_ptr = (uint64_t) push_data_ptr +
         descriptor->push_constants.stages[
            ANV_GENERATED_COMMAND_STAGE_MESH].slots[0].push_data_offset;
      struct GENX(3DSTATE_MESH_SHADER_DATA) data = {
         GENX(3DSTATE_MESH_SHADER_DATA_header),
         .InlineData = {
            data_ptr & 0xffffffff,
            data_ptr >> 32,
         },
      };
      GENX(3DSTATE_MESH_SHADER_DATA_pack)(push_cmd_ptr + cmd_offset, &data);
      cmd_offset += GENX(3DSTATE_MESH_SHADER_DATA_length) * 4;
   }
#endif

   return cmd_offset;
}

static global void *
get_ptr(global void *base, uint32_t stride,
        uint32_t prolog_size, uint32_t seq_idx)
{
   return base + prolog_size + seq_idx * stride;
}

static void
write_prolog_epilog(global void *cmd_base, uint32_t cmd_stride,
                    uint32_t max_count, uint32_t cmd_prolog_size,
                    uint32_t seq_idx, uint64_t return_addr)
{
   /* A write to the location of the MI_BATCH_BUFFER_START below. */
   genX(write_address)(cmd_base,
                       get_ptr(cmd_base, cmd_stride,
                               cmd_prolog_size, max_count) + 4,
                       return_addr);

   global void *next_addr = cmd_base + (GENX(MI_STORE_DATA_IMM_length) + 1 +
                                        GENX(MI_BATCH_BUFFER_START_length)) * 4;

   genX(write_MI_BATCH_BUFFER_START)(
      cmd_base + (GENX(MI_STORE_DATA_IMM_length) + 1) * 4,
      (uint64_t)next_addr);

   /* Reenable the prefetcher. */
#if GFX_VER >= 12
   struct GENX(MI_ARB_CHECK) v = {
      GENX(MI_ARB_CHECK_header),
      /* This is a trick to get the CLC->SPIRV not to use a constant variable
       * for this. Otherwise we run into issues trying to store that variable
       * in constant memory which is inefficient for a single dword and also
       * not handled in our backend.
       */
      .PreParserDisableMask = seq_idx == 0,
      .PreParserDisable = false,
   };
   GENX(MI_ARB_CHECK_pack)(next_addr, &v);
#endif

   /* This is the epilog, returning to the main batch. */
   genX(write_MI_BATCH_BUFFER_START)(
      get_ptr(cmd_base, cmd_stride, cmd_prolog_size, max_count),
      return_addr);
}

static void
write_return_addr(global void *cmd_base, uint32_t cmd_stride,
                  uint32_t max_count, uint32_t cmd_prolog_size,
                  uint64_t return_addr)
{
   /* A write to the location of the MI_BATCH_BUFFER_START below. */
   genX(write_address)(cmd_base,
                       get_ptr(cmd_base, cmd_stride,
                               cmd_prolog_size, max_count) + 4,
                       return_addr);
}

void
genX(libanv_preprocess_gfx_generate_step1)(global void *cmd_base,
                                           uint32_t cmd_stride,
                                           global void *data_base,
                                           uint32_t data_stride,
                                           global void *seq_base,
                                           uint32_t seq_stride,
                                           global uint32_t *seq_count,
                                           uint32_t max_seq_count,
                                           uint32_t cmd_prolog_size,
                                           uint32_t data_prolog_size,
                                           global struct anv_gen_gfx_state *state,
                                           global struct anv_gen_gfx_indirect_descriptor *indirect_set,
                                           global void *const_ptr,
                                           uint32_t const_size,
                                           global void *driver_const_ptr,
                                           uint64_t return_addr,
                                           uint32_t flags,
                                           uint32_t seq_idx)
{
   uint32_t max_count = seq_count != 0 ? min(*seq_count, max_seq_count) : max_seq_count;

   if (seq_idx == 0) {
      write_prolog_epilog(cmd_base, cmd_stride, max_count,
                          cmd_prolog_size, seq_idx, return_addr);
   }

   if (seq_idx >= max_count)
      return;

   /* Pointer to the stream data, layed out as described in stream_layout. */
   global void *seq_ptr = seq_base + seq_idx * seq_stride;

   /* Where to write the commands */
   global void *cmd_ptr =
      get_ptr(cmd_base, cmd_stride, cmd_prolog_size, seq_idx);

   /* 3DSTATE_INDEX_BUFFER */
   struct anv_gen_index_buffer index_buffer = state->layout.index_buffer;
   if (index_buffer.cmd_size != 0) {
      VkBindIndexBufferIndirectCommandEXT idx_data =
         *(global VkBindIndexBufferIndirectCommandEXT *)(
            seq_ptr + index_buffer.seq_offset);

      uint32_t index_format =
         index_buffer.u32_value == idx_data.indexType ? INDEX_DWORD :
         index_buffer.u16_value == idx_data.indexType ? INDEX_WORD :
         index_buffer.u8_value  == idx_data.indexType ? INDEX_BYTE :
         INDEX_BYTE;

      genX(write_3DSTATE_INDEX_BUFFER)(cmd_ptr + index_buffer.cmd_offset,
                                       idx_data.bufferAddress,
                                       idx_data.size,
                                       index_format,
                                       index_buffer.mocs);
   }

   /* 3DSTATE_VERTEX_BUFFERS */
   uint32_t n_vertex_buffers = state->layout.vertex_buffers.n_buffers;
   if (n_vertex_buffers) {
      global void *cmd_vb = cmd_ptr + state->layout.vertex_buffers.cmd_offset;

      genX(write_3DSTATE_VERTEX_BUFFERS)(cmd_vb, n_vertex_buffers);
      cmd_vb += 4;

      uint16_t mocs = state->layout.vertex_buffers.mocs;
      for (uint32_t i = 0; i < n_vertex_buffers; i++) {
         struct anv_gen_vertex_buffer vb = state->layout.vertex_buffers.buffers[i];

         VkBindVertexBufferIndirectCommandEXT vtx_data =
            *(global VkBindVertexBufferIndirectCommandEXT *)(
               seq_ptr + vb.seq_offset);

         genX(write_VERTEX_BUFFER_STATE)(cmd_vb, mocs, vb.binding,
                                         vtx_data.bufferAddress,
                                         vtx_data.size,
                                         vtx_data.stride);
         cmd_vb += GENX(VERTEX_BUFFER_STATE_length) * 4;
      }
   }

   global struct anv_gen_gfx_indirect_descriptor *descriptor;
   if (state->layout.indirect_set.active) {
      uint32_t set_idx =
         *(global uint32_t *)(seq_ptr + state->layout.indirect_set.seq_offset);
      descriptor = &indirect_set[set_idx];
   } else {
      descriptor = indirect_set;
   }

   enum intel_msaa_flags fs_msaa_flags =
      intel_fs_msaa_flags(descriptor->sample_shading,
                          descriptor->min_sample_shading,
                          descriptor->sample_shading_enable,
                          state->dyn.samples,
                          state->dyn.coarse_pixel_enabled,
                          state->dyn.alpha_to_coverage);

   if (state->layout.indirect_set.active) {
      /* We reemit all the stages because we cannot optimize state emission,
       * that would require diffing each lane from the previous and insert a
       * bunch of MI_NOOP.
       *
       * On the flip side, this takes care of Wa_16011107343 & Wa_22018402687.
       */

      /* Fully packed stuff */
      genX(copy_data)(cmd_ptr + state->layout.indirect_set.final_cmds_offset,
                      descriptor->final_commands,
                      state->layout.indirect_set.final_cmds_size);

      /* Dynamically packed stuff */
      uint32_t dyn_cmd_offset = state->layout.indirect_set.partial_cmds_offset;

      /* Partial states that are not needed for mesh pipelines */
      if (!HAS_STAGE(descriptor, MESH)) {
         merge_dwords(cmd_ptr + dyn_cmd_offset,
                      descriptor->partial.so,
                      state->indirect_set.so,
                      GENX(3DSTATE_STREAMOUT_length));
         dyn_cmd_offset += 4 * GENX(3DSTATE_STREAMOUT_length);

         {
            uint32_t topology =
               HAS_STAGE(descriptor, TESS_EVAL) ?
               _3DPRIM_PATCHLIST(state->dyn.patch_control_points) :
               state->dyn.primitive_topology;
            genX(write_3DSTATE_VF_TOPOLOGY)(
               cmd_ptr + dyn_cmd_offset, topology);
            dyn_cmd_offset += 4 * GENX(3DSTATE_VF_TOPOLOGY_length);
         }

         {
            uint32_t tes_output_topology = descriptor->tes_output_topology;
            uint32_t output_topology =
               HAS_STAGE(descriptor, TESS_EVAL) ?
               (state->dyn.domain_origin ==
                VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT ? tes_output_topology :
                tes_output_topology == OUTPUT_TRI_CCW ? OUTPUT_TRI_CW :
                tes_output_topology == OUTPUT_TRI_CW ? OUTPUT_TRI_CCW :
                tes_output_topology) :
               OUTPUT_POINT;

            struct GENX(3DSTATE_TE) v = {
               .OutputTopology = output_topology,
            };
            GENX(3DSTATE_TE_repack)(cmd_ptr + dyn_cmd_offset,
                                    descriptor->partial.te, &v);
            dyn_cmd_offset += 4 * GENX(3DSTATE_TE_length);
         }

         {
            struct GENX(3DSTATE_GS) v = {
               .ReorderMode =
                  state->dyn.provoking_vertex ==
                  VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT ?
                  LEADING : TRAILING,
            };
            GENX(3DSTATE_GS_repack)(cmd_ptr + dyn_cmd_offset,
                                    descriptor->partial.gs, &v);
            dyn_cmd_offset += 4 * GENX(3DSTATE_GS_length);
         }
      }

#if INTEL_WA_16014912113_GFX_VER
      if (flags & ANV_GENERATED_FLAG_WA_16014912113) {
         /* When DS programming changes, emit a workaround URB programming,
          * otherwise, memset 0 for MI_NOOPs.
          */
         if (seq_idx != 0 &&
             indirect_set[seq_idx - 1].ds_urb_cfg != descriptor->ds_urb_cfg) {
            global struct anv_gen_gfx_indirect_descriptor *prev_descriptor =
               &indirect_set[seq_idx - 1];
            genX(copy_data)(cmd_ptr + dyn_cmd_offset,
                            prev_descriptor->final.urb_wa_16014912113,
                            sizeof(prev_descriptor->final.urb_wa_16014912113));
         } else {
            genX(set_data)(cmd_ptr + dyn_cmd_offset, 0,
                           sizeof(descriptor->final.urb_wa_16014912113));
         }
         dyn_cmd_offset += sizeof(descriptor->final.urb_wa_16014912113);
         genX(copy_data)(cmd_ptr + dyn_cmd_offset,
                         descriptor->final.urb,
                         sizeof(descriptor->final.urb));
         dyn_cmd_offset += sizeof(descriptor->final.urb);
      }
#endif

#if GFX_VERx10 >= 125
      {
         merge_dwords(cmd_ptr + dyn_cmd_offset,
                      descriptor->partial.vfg,
                      state->indirect_set.vfg,
                      GENX(3DSTATE_VFG_length));
         dyn_cmd_offset += 4 * GENX(3DSTATE_VFG_length);
      }
#endif

      {
         merge_dwords(cmd_ptr + dyn_cmd_offset,
                      descriptor->partial.sf,
                      state->indirect_set.sf,
                      GENX(3DSTATE_SF_length));
         dyn_cmd_offset += 4 * GENX(3DSTATE_SF_length);
      }

      VkPolygonMode dynamic_raster_mode =
         raster_polygon_mode(descriptor,
                             descriptor->active_stages,
                             state->dyn.polygon_mode,
                             state->dyn.primitive_topology);

#define SET_PROVOKING_VERTEX(mode) \
      .TriangleStripListProvokingVertexSelect = \
         mode == VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT ? 0 : 2, \
      .LineStripListProvokingVertexSelect = \
         mode == VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT ? 0 : 1, \
      .TriangleFanProvokingVertexSelect = \
         mode == VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT ? 1 : 2

      {
         struct GENX(3DSTATE_CLIP) v = {
            SET_PROVOKING_VERTEX(state->dyn.provoking_vertex),
            .APIMode = (state->dyn.depth_clip_negative_one_to_one ?
                        APIMODE_OGL : APIMODE_D3D),
            .ViewportXYClipTestEnable =
               dynamic_raster_mode == VK_POLYGON_MODE_FILL,
            .MaximumVPIndex = state->dyn.max_vp_index,
         };
         GENX(3DSTATE_CLIP_repack)(cmd_ptr + dyn_cmd_offset,
                                   descriptor->partial.clip, &v);
         dyn_cmd_offset += 4 * GENX(3DSTATE_CLIP_length);
      }

#undef SET_PROVOKING_VERTEX

      {
         uint32_t api_mode =
            dynamic_raster_mode == VK_POLYGON_MODE_LINE ?
            state->dyn.line_api_mode :
            DX101;
         bool msaa_raster_enable =
            dynamic_raster_mode == VK_POLYGON_MODE_LINE ?
            state->dyn.line_msaa_raster_enable :
            true;
         bool aa_enable =
            rasterization_aa_mode(dynamic_raster_mode, state->dyn.line_mode) &&
            !state->dyn.has_uint_rt &&
            !(GFX_VER >= 12 && state->dyn.samples > 1);

         struct GENX(3DSTATE_RASTER) v = {
            .AntialiasingEnable = aa_enable,
            .APIMode = api_mode,
            .DXMultisampleRasterizationEnable = msaa_raster_enable,
         };
         GENX(3DSTATE_RASTER_repack)(cmd_ptr + dyn_cmd_offset,
                                     state->indirect_set.raster, &v);
         dyn_cmd_offset += 4 * GENX(3DSTATE_RASTER_length);
      }

      {
         struct GENX(3DSTATE_WM) v = {
            .LineStippleEnable = state->dyn.line_stipple_enable,
            .BarycentricInterpolationMode = intel_fs_barycentric_modes(
               descriptor->persample_dispatch,
               descriptor->barycentric_interp_modes, fs_msaa_flags),
         };
         GENX(3DSTATE_WM_repack)(cmd_ptr + dyn_cmd_offset,
                                 descriptor->partial.wm, &v);
         dyn_cmd_offset += 4 * GENX(3DSTATE_WM_length);
      }

      {
         if (state->dyn.samples > 1) {
            genX(copy_data)(cmd_ptr + dyn_cmd_offset,
                            descriptor->partial.ps_msaa,
                            4 * GENX(3DSTATE_PS_length));
         } else {
            genX(copy_data)(cmd_ptr + dyn_cmd_offset,
                            descriptor->partial.ps,
                            4 * GENX(3DSTATE_PS_length));
         }
         dyn_cmd_offset += 4 * GENX(3DSTATE_PS_length);
      }

      {
         struct GENX(3DSTATE_PS_EXTRA) v = {
            .PixelShaderHasUAV = (
               descriptor->has_side_effects ||
               (GFX_VERx10 >= 125 ?
                (state->dyn.color_write_enables == 0 &&
                 state->dyn.n_occlusion_queries > 0) : 0)),
            .PixelShaderKillsPixel = (
               HAS_STAGE(descriptor, FRAGMENT) &&
               (descriptor->rp_has_ds_self_dep ||
                state->dyn.has_feedback_loop ||
                descriptor->uses_kill)),
            .PixelShaderIsPerSample = intel_fs_is_persample(
               descriptor->persample_dispatch,
               descriptor->sample_shading,
               fs_msaa_flags),
            .PixelShaderIsPerCoarsePixel = intel_fs_is_coarse(
               descriptor->coarse_pixel_dispatch, fs_msaa_flags),
#if GFX_VERx10 >= 125
            .EnablePSDependencyOnCPsizeChange = false,
#endif
         };
         GENX(3DSTATE_PS_EXTRA_repack)(cmd_ptr + dyn_cmd_offset,
                                       descriptor->partial.ps_extra, &v);
         dyn_cmd_offset += 4 * GENX(3DSTATE_PS_EXTRA_length);
      }

      {
         bool has_writeable_rt = HAS_STAGE(descriptor, FRAGMENT) &&
            (descriptor->color_writes & state->dyn.color_write_enables) != 0;

         struct GENX(3DSTATE_PS_BLEND) v = {
            .HasWriteableRT = has_writeable_rt,
         };
         GENX(3DSTATE_PS_BLEND_repack)(cmd_ptr + dyn_cmd_offset,
                                       state->indirect_set.ps_blend, &v);
         dyn_cmd_offset += 4 * GENX(3DSTATE_PS_BLEND_length);

      }

      /* Fill the potential gap with the push constant commands with 0s */
      genX(set_data)(cmd_ptr + dyn_cmd_offset, 0,
                     state->layout.push_constants.cmd_offset - dyn_cmd_offset);

   } else {
#if INTEL_WA_16011107343_GFX_VER || INTEL_WA_22018402687_GFX_VER
      genX(copy_data)(cmd_ptr + state->layout.indirect_set.final_cmds_offset,
                      descriptor->final_commands,
                      state->layout.indirect_set.final_cmds_size);
#endif
   }

   /* Push constants */
   enum anv_gen_push_constant_flags pc_flags =
      state->layout.push_constants.flags;
   if (pc_flags & ANV_GEN_PUSH_CONSTANTS_CMD_ACTIVE) {
      global void *push_data_ptr =
         get_ptr(data_base, data_stride, data_prolog_size, seq_idx) +
         state->layout.push_constants.data_offset;

      if (pc_flags & ANV_GEN_PUSH_CONSTANTS_DATA_ACTIVE) {
         write_app_push_constant_data(push_data_ptr,
                                      &state->layout.push_constants,
                                      seq_ptr, const_ptr,
                                      const_size, seq_idx);
         write_gfx_drv_push_constant_data(push_data_ptr +
                                          ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE,
                                          driver_const_ptr,
                                          ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_SIZE,
                                          state->dyn.patch_control_points,
                                          fs_msaa_flags);
      }

      uint32_t end_cmd_offset =
         write_gfx_push_constant_commands(cmd_ptr +
                                          state->layout.push_constants.cmd_offset,
                                          push_data_ptr,
                                          state, descriptor);

      /* Fill the potential gap with the draw command with 0s */
      genX(set_data)(cmd_ptr +
                     state->layout.push_constants.cmd_offset + end_cmd_offset,
                     0,
                     state->layout.draw.cmd_offset -
                     (state->layout.push_constants.cmd_offset + end_cmd_offset));
   }

   /* 3DPRIMITIVE / 3DMESH_3D */
   bool is_predicated = (flags & ANV_GENERATED_FLAG_PREDICATED) != 0;
   bool tbimr_enabled = (flags & ANV_GENERATED_FLAG_TBIMR) != 0;
   switch (state->layout.draw.draw_type) {
   case ANV_GEN_GFX_DRAW:
      genX(write_draw)(cmd_ptr + state->layout.draw.cmd_offset,
                       seq_ptr + state->layout.draw.seq_offset,
                       0 /* draw_id_ptr */,
                       0 /* draw_id, always 0 per spec */,
                       1 /* instance_multiplier (multiview banned) */,
                       false /* indexed */,
                       is_predicated,
                       tbimr_enabled,
                       true /* uses_base, unused for Gfx11+ */,
                       true /* uses_draw_id, unused for Gfx11+ */,
                       0 /* mocs, unused for Gfx11+ */);
      break;

   case ANV_GEN_GFX_DRAW_INDEXED:
      genX(write_draw)(cmd_ptr + state->layout.draw.cmd_offset,
                       seq_ptr + state->layout.draw.seq_offset,
                       0 /* draw_id_ptr */,
                       0 /* draw_id, always 0 per spec */,
                       1 /* instance_multiplier (multiview banned) */,
                       true /* indexed */,
                       is_predicated,
                       tbimr_enabled,
                       true /* uses_base, unused for Gfx11+ */,
                       true /* uses_draw_id, unused for Gfx11+ */,
                       0 /* mocs, unused for Gfx11+ */);
      break;

#if GFX_VERx10 >= 125
   case ANV_GEN_GFX_DRAW_MESH:
      genX(write_3DMESH_3D)(cmd_ptr + state->layout.draw.cmd_offset,
                            seq_ptr + state->layout.draw.seq_offset,
                            is_predicated,
                            tbimr_enabled);
      break;
#endif
   }
}

#if GFX_VERx10 >= 125
static void
emit_dispatch_commands(global void *cmd_base,
                       uint32_t cmd_stride,
                       uint32_t seq_idx,
                       uint32_t prolog_size,
                       global void *data_ptr,
                       global void *seq_ptr,
                       global struct anv_gen_cs_layout *layout,
                       global struct anv_gen_cs_indirect_descriptor *descriptor,
                       global void *interface_descriptor_data_ptr,
                       uint32_t flags)
{
   global void *cmd_ptr = get_ptr(cmd_base, cmd_stride, prolog_size, seq_idx);

   VkDispatchIndirectCommand info =
      *((global VkDispatchIndirectCommand *)(seq_ptr + layout->dispatch.seq_offset));
   uint64_t pc_addr = (uint64_t)data_ptr + descriptor->push_data_offset;

   struct GENX(COMPUTE_WALKER) v = {
      .PredicateEnable = (flags & ANV_GENERATED_FLAG_PREDICATED) != 0,
      .body = {
         .ThreadGroupIDXDimension = info.x,
         .ThreadGroupIDYDimension = info.y,
         .ThreadGroupIDZDimension = info.z,
         .InlineData              = {
            pc_addr & 0xffffffff,
            pc_addr >> 32,
            info.x,
            info.y,
            info.z,
         },
      },
   };

   GENX(COMPUTE_WALKER_repack)(cmd_ptr, descriptor->gfx125.compute_walker, &v);
}
#else
static void
emit_dispatch_commands(global void *cmd_base,
                       uint32_t cmd_stride,
                       uint32_t seq_idx,
                       uint32_t cmd_prolog_size,
                       global void *data_ptr,
                       global void *seq_ptr,
                       global struct anv_gen_cs_layout *layout,
                       global struct anv_gen_cs_indirect_descriptor *descriptor,
                       global void *interface_descriptor_data_ptr,
                       uint32_t flags)
{
   global void *cmd_ptr = get_ptr(cmd_base, cmd_stride, cmd_prolog_size, seq_idx);

   if (layout->indirect_set.active != 0) {
      /* Emit MEDIA_VFE_STATE either for each sequence */
      genX(copy_data)(cmd_ptr, descriptor->gfx9.media_vfe_state,
                      sizeof(descriptor->gfx9.media_vfe_state));
      cmd_ptr += sizeof(descriptor->gfx9.media_vfe_state);

      /* Load the shader descriptor */
      global void *idd_ptr =
         data_ptr + layout->indirect_set.data_offset;
      merge_dwords(idd_ptr,
                   interface_descriptor_data_ptr,
                   descriptor->gfx9.interface_descriptor_data,
                   GENX(INTERFACE_DESCRIPTOR_DATA_length));

      uint32_t idd_offset =
         ANV_DYNAMIC_VISIBLE_HEAP_OFFSET + ((uint64_t)idd_ptr) & 0xffffffff;

      struct GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD) mdd = {
         GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD_header),
         .InterfaceDescriptorTotalLength      = GENX(INTERFACE_DESCRIPTOR_DATA_length) * 4,
         .InterfaceDescriptorDataStartAddress = idd_offset,
      };
      GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD_pack)(cmd_ptr, &mdd);
      cmd_ptr += GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD_length) * 4;
   }

   /* Push constant offset relative to the dynamic state heap */
   uint32_t dyn_push_data_offset =
      ANV_DYNAMIC_VISIBLE_HEAP_OFFSET + (((uint64_t)data_ptr) & 0xffffffff);

   struct GENX(MEDIA_CURBE_LOAD) mdl = {
      GENX(MEDIA_CURBE_LOAD_header),
      .CURBETotalDataLength    = descriptor->gfx9.cross_thread_push_size +
                                 descriptor->gfx9.n_threads *
                                 descriptor->gfx9.per_thread_push_size,
      .CURBEDataStartAddress   = dyn_push_data_offset,
   };
   GENX(MEDIA_CURBE_LOAD_pack)(cmd_ptr, &mdl);
   cmd_ptr += GENX(MEDIA_CURBE_LOAD_length) * 4;

   /* Emit the walker */
   VkDispatchIndirectCommand info =
      *((global VkDispatchIndirectCommand *)(seq_ptr + layout->dispatch.seq_offset));

   struct GENX(GPGPU_WALKER) walker = {
      .PredicateEnable           = (flags & ANV_GENERATED_FLAG_PREDICATED) != 0,
      .ThreadGroupIDXDimension   = info.x,
      .ThreadGroupIDYDimension   = info.y,
      .ThreadGroupIDZDimension   = info.z,
   };
   GENX(GPGPU_WALKER_repack)(cmd_ptr, descriptor->gfx9.gpgpu_walker, &walker);
   global uint32_t *walker_ptr = cmd_ptr;
   cmd_ptr += GENX(GPGPU_WALKER_length) * 4;

   /* Write the workgroup size in the driver constants */
   uint32_t push_data_offset = descriptor->push_data_offset;
   if (push_data_offset < (ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_WS_SIZE_OFFSET)) {
      global uint3 *wg_size = data_ptr +
         ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_WS_SIZE_OFFSET -
         push_data_offset;
      *wg_size = (uint3)(info.x, info.y, info.z);
   }

   uint32_t per_thread_push_size = descriptor->gfx9.per_thread_push_size;
   if (per_thread_push_size > 0) {
      uint32_t cross_thread_push_size = descriptor->gfx9.cross_thread_push_size;
      global void *per_thread_ptr0 = data_ptr + cross_thread_push_size;
      global void *per_thread_ptr = per_thread_ptr0;
      for (uint32_t t = 0; t < descriptor->gfx9.n_threads; t++) {
         if (t > 0) {
            genX(copy_data)(per_thread_ptr, per_thread_ptr0,
                            per_thread_push_size);
         }
         *(uint32_t*)(per_thread_ptr + descriptor->gfx9.subgroup_id_offset) = t;
         per_thread_ptr += per_thread_push_size;
      }
   }

#if 0
   /* For some reason we're unable to avoid LLVM/SPIRV putting this in the
    * constant space...
    */
   struct GENX(MEDIA_STATE_FLUSH) flush = {
      GENX(MEDIA_STATE_FLUSH_header),
      .InterfaceDescriptorOffset = (cmd_ptr == 1),
   };
   GENX(MEDIA_STATE_FLUSH_pack)(cmd_ptr, &walker);
#else
   *((uint2 *)cmd_ptr) = (uint2)(0x70040000, 0x00000000);
#endif
}
#endif

void
genX(libanv_preprocess_cs_generate_step1)(global void *cmd_base,
                                          uint32_t cmd_stride,
                                          global void *data_base,
                                          uint32_t data_stride,
                                          global void *seq_base,
                                          uint32_t seq_stride,
                                          global uint32_t *seq_count,
                                          uint32_t max_seq_count,
                                          uint32_t cmd_prolog_size,
                                          uint32_t data_prolog_size,
                                          global struct anv_gen_cs_layout *layout,
                                          global struct anv_gen_cs_indirect_descriptor *indirect_set,
                                          global void *interface_descriptor_data_ptr,
                                          global void *const_ptr,
                                          uint32_t const_size,
                                          global void *driver_const_ptr,
                                          uint64_t return_addr,
                                          uint32_t flags,
                                          uint32_t seq_idx)
{
   uint32_t max_count = seq_count != 0 ? min(*seq_count, max_seq_count) : max_seq_count;

   if (seq_idx == 0) {
      write_prolog_epilog(cmd_base, cmd_stride, max_count,
                          cmd_prolog_size, seq_idx, return_addr);
   }

   if (seq_idx >= max_count)
      return;

   /* Pointer to the application generated data, layed out as described in
    * stream_layout.
    */
   global void *seq_ptr = seq_base + seq_idx * seq_stride;

   /* Get the shader descriptor. */
   global struct anv_gen_cs_indirect_descriptor *descriptor;
   if (layout->indirect_set.active != 0) {
      uint32_t set_idx = *(global uint32_t *)(seq_ptr + layout->indirect_set.seq_offset);
      descriptor = &indirect_set[set_idx];
   } else {
      descriptor = indirect_set;
   }

   /* Prepare the push constant data. If none is updated per sequence, we'll
    * use a single location for all the sequences.
    */
   bool per_sequence_push_constants =
      GFX_VERx10 <= 120 ||
      layout->push_constants.num_entries > 0 ||
      layout->push_constants.seq_id_active;
   uint32_t push_data_offset = descriptor->push_data_offset;

   /* */
   global void *push_data_base =
      per_sequence_push_constants ?
      (get_ptr(data_base, data_stride, data_prolog_size, seq_idx) +
       layout->push_constants.data_offset) :
      data_base;
#if GFX_VERx10 >= 125
   uint32_t push_data_size =
      ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE;
   global void *push_data_ptr = push_data_base;
   uint32_t driver_data_size =
      ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_SIZE;
   global void *driver_data_ptr =
      push_data_ptr + ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE;
#else
   uint32_t push_data_size =
      ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE -
      MIN2(ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE, push_data_offset);
   global void *push_data_ptr = push_data_base +
      ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE -
      push_data_size;
   uint32_t driver_data_size =
      MIN2(ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_SIZE,
           (ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE +
            ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_SIZE) - push_data_offset);
   global void *driver_data_ptr =
      push_data_base +
      ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE +
      ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_SIZE -
      driver_data_size;
#endif
   if (per_sequence_push_constants || seq_idx == 0) {
      write_app_push_constant_data(
         push_data_ptr, &layout->push_constants,
         seq_ptr, const_ptr, const_size, seq_idx);
      write_drv_push_constant_data(
         driver_data_ptr, driver_const_ptr,
         ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_SIZE);
   }
   /* Finally write the commands */
   emit_dispatch_commands(cmd_base, cmd_stride, seq_idx, cmd_prolog_size,
                          push_data_ptr, seq_ptr, layout,
                          descriptor, interface_descriptor_data_ptr, flags);
}

void
genX(libanv_postprocess_cs_generate)(global void *cmd_base,
                                     uint32_t cmd_stride,
                                     global void *data_base,
                                     uint32_t data_stride,
                                     global uint32_t *seq_count,
                                     uint32_t max_seq_count,
                                     uint32_t cmd_prolog_size,
                                     uint32_t data_prolog_size,
                                     uint32_t data_idd_offset,
                                     global struct anv_gen_cs_indirect_descriptor *descriptor,
                                     uint64_t return_addr,
                                     uint32_t seq_idx)
{
   uint32_t max_count = seq_count != 0 ? min(*seq_count, max_seq_count) : max_seq_count;

   if (seq_idx == 0) {
      write_prolog_epilog(cmd_base, cmd_stride, max_count,
                          cmd_prolog_size, seq_idx, return_addr);
   }

   if (seq_idx >= max_count)
      return;

   /* Where to write the commands */
   global void *cmd_ptr =
      get_ptr(cmd_base, cmd_stride, cmd_prolog_size, seq_idx);

   /* OR the driver INTERFACE_DESCRIPTOR_DATA dwords with the device generated
    * ones.
    */
   uint32_t n_dwords = 2; /* dwords covered from
                           * INTERFACE_DESCRIPTOR_DATA::SamplerCount to
                           * INTERFACE_DESCRIPTOR_DATA::BindingTablePointer
                           */

#if GFX_VERx10 >= 125
   uint32_t inst_offset_B = (GFX_VERx10 >= 120 ? 76 : 72) /* offset in COMPUTE_WALKER */ +
                            12 /* offset in INTERFACE_DESCRIPTOR_DATA */;
   merge_dwords(cmd_ptr + inst_offset_B,
                cmd_ptr + inst_offset_B,
                &descriptor->gfx125.compute_walker[inst_offset_B / 4],
                n_dwords);
#else
   global void *idd_ptr =
      get_ptr(data_base, data_stride, data_prolog_size, seq_idx) +
      data_idd_offset;
   uint32_t inst_offset_B = 12 /* offset in INTERFACE_DESCRIPTOR_DATA */;
   merge_dwords(idd_ptr + inst_offset_B,
                idd_ptr + inst_offset_B,
                &descriptor->gfx9.interface_descriptor_data[inst_offset_B / 4],
                n_dwords);
#endif
}

#if GFX_VERx10 >= 125
static uint3
calc_local_trace_size(uint3 global_size)
{
   unsigned total_shift = 0;
   uint3 local_shift = (0, 0, 0);

   bool progress;
   do {
      progress = false;
      for (unsigned i = 0; i < 3; i++) {
         if ((1 << local_shift[i]) < global_size[i]) {
            progress = true;
            local_shift[i]++;
            total_shift++;
         }

         if (total_shift == 3)
            return local_shift;
      }
   } while (progress);

   /* Assign whatever's left to x */
   local_shift[0] += 3 - total_shift;

   return local_shift;
}

void
genX(libanv_preprocess_rt_generate)(global void *cmd_base,
                                    uint32_t cmd_stride,
                                    global void *data_base,
                                    uint32_t data_stride,
                                    global void *seq_base,
                                    uint32_t seq_stride,
                                    global uint32_t *seq_count,
                                    uint32_t max_seq_count,
                                    uint32_t cmd_prolog_size,
                                    uint32_t data_prolog_size,
                                    global struct anv_gen_cs_layout *layout,
                                    global void *compute_walker_template,
                                    global void *rtdg_global_template,
                                    global void *const_ptr,
                                    uint32_t const_size,
                                    global void *driver_const_ptr,
                                    uint64_t return_addr,
                                    uint32_t flags,
                                    uint32_t seq_idx)
{
   uint32_t max_count = seq_count != 0 ? min(*seq_count, max_seq_count) : max_seq_count;

   if (seq_idx == 0) {
      write_prolog_epilog(cmd_base, cmd_stride, max_count,
                          cmd_prolog_size, seq_idx, return_addr);
   }

   if (seq_idx >= max_count)
      return;

   /* Where to write the commands */
   global void *cmd_ptr =
      get_ptr(cmd_base, cmd_stride, cmd_prolog_size, seq_idx);

   /* Pointer to the application generated data, layed out as described in
    * stream_layout.
    */
   global void *seq_ptr = seq_base + seq_idx * seq_stride;

   VkTraceRaysIndirectCommand2KHR *info =
      ((global VkTraceRaysIndirectCommand2KHR *)(seq_ptr + layout->dispatch.seq_offset));
   uint3 launch_size = (uint3)(info->width, info->height, info->depth);

   /* RTDG + push constants */
   global void *push_data_ptr =
      get_ptr(data_base, data_stride, data_prolog_size, seq_idx) +
      layout->push_constants.data_offset;
   global void *rtdg_ptr = push_data_ptr;
   struct GENX(RT_DISPATCH_GLOBALS) rtdg = {
      .LaunchWidth  = launch_size.x,
      .LaunchHeight = launch_size.y,
      .LaunchDepth  = launch_size.z,
      .HitGroupTable = (struct GENX(RT_SHADER_TABLE)) {
         .BaseAddress = info->hitShaderBindingTableAddress,
         .Stride      = info->hitShaderBindingTableStride,
      },
      .MissGroupTable = (struct GENX(RT_SHADER_TABLE)) {
         .BaseAddress = info->missShaderBindingTableAddress,
         .Stride      = info->missShaderBindingTableStride,
      },
      .CallableGroupTable = (struct GENX(RT_SHADER_TABLE)) {
         .BaseAddress = info->callableShaderBindingTableAddress,
         .Stride      = info->callableShaderBindingTableStride,
      },
   };
   GENX(RT_DISPATCH_GLOBALS_repack)(rtdg_ptr, rtdg_global_template, &rtdg);

   write_app_push_constant_data(
      push_data_ptr + ANV_GENERATED_COMMAND_RT_GLOBAL_DISPATCH_SIZE,
      &layout->push_constants,
      seq_ptr, const_ptr, const_size, seq_idx);
   write_drv_push_constant_data(
      push_data_ptr +
      ANV_GENERATED_COMMAND_RT_GLOBAL_DISPATCH_SIZE +
      ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE,
      driver_const_ptr,
      ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_SIZE);

   uint3 local_size_log2 = calc_local_trace_size(launch_size);
   uint3 one = 1;
   uint3 local_size = one << local_size_log2;
   uint3 global_size = DIV_ROUND_UP(launch_size, local_size);

   /* Finally write the commands */
   global uint64_t *sbt = (global uint64_t *)info->raygenShaderRecordAddress;
   struct GENX(COMPUTE_WALKER) v = {
      .PredicateEnable = (flags & ANV_GENERATED_FLAG_PREDICATED) != 0,
      .body = {
         .LocalXMaximum           = (1u << local_size_log2.x) - 1,
         .LocalYMaximum           = (1u << local_size_log2.y) - 1,
         .LocalZMaximum           = (1u << local_size_log2.z) - 1,
         .ThreadGroupIDXDimension = global_size.x,
         .ThreadGroupIDYDimension = global_size.y,
         .ThreadGroupIDZDimension = global_size.z,
         /* See struct brw_rt_raygen_trampoline_params */
         .InlineData              = {
            ((uint64_t) rtdg_ptr) & 0xffffffff,
            ((uint64_t) rtdg_ptr) >> 32,
            info->raygenShaderRecordAddress & 0xffffffff,
            info->raygenShaderRecordAddress >> 32,
            local_size_log2.x << 8 |
            local_size_log2.y << 16 |
            local_size_log2.z << 24,
         },
      },
   };
   GENX(COMPUTE_WALKER_repack)(cmd_ptr, compute_walker_template, &v);
}
#endif /* GFX_VERx10 >= 125 */

#endif /* GFX_VER >= 11 */
