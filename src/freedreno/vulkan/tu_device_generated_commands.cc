/*
 * Copyright © 2021 Google
 * Copyright © 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "tu_cmd_buffer.h"
#include "tu_device_generated_commands.h"
#include "tu_device.h"
#include "tu_descriptor_set.h"

#include "common/freedreno_gpu_event.h"

static const uint32_t preprocess_spv[] = {
#include "dgc/preprocess.spv.h"
};

#define COMPUTE_DP_SIZE 8
#define SHADER_INLINE_UBO_CMDS_SIZE (MAX_INLINE_UBOS * 6)
#define GRAPHICS_INLINE_UBO_CMDS_SIZE (5 * SHADER_INLINE_UBO_CMDS_SIZE)

struct tu_dgc_shader_draw_state {
   uint64_t iova;
   uint32_t word0;
   uint32_t padding;
};

static tu_dgc_shader_draw_state
emit_draw_state(struct tu_draw_state state)
{
   return (struct tu_dgc_shader_draw_state) {
      .iova = state.iova,
      .word0 = state.size ? CP_SET_DRAW_STATE__0_COUNT(state.size) :
         CP_SET_DRAW_STATE__0_DISABLE,
   };
}

struct tu_dgc_compute_pipeline_data {
   uint64_t shader_iova;
   uint32_t shader_size;
   uint32_t driver_param_opcode; /* Either CP_LOAD_STATE6_FRAG or a NOP */
   uint32_t driver_param_ubo_idx;
   uint32_t compute_driver_params[COMPUTE_DP_SIZE];
   uint32_t cs_ndrange_0;
   uint32_t exec_cs_indirect_3;
   uint32_t user_consts_size;
   uint32_t inline_ubo_commands[SHADER_INLINE_UBO_CMDS_SIZE];
};


struct tu_dgc_graphics_pipeline_data {
   /* This isn't really part of the pipeline, but just pass it here to avoid
    * creating a new thing for cmdbuf state that must be patched in.
    */
   uint64_t index_base;
   uint32_t max_index;

   uint32_t vs_params_offset;
   uint32_t pc_tess_cntl;

   struct tu_dgc_shader_draw_state program_config;
   struct tu_dgc_shader_draw_state vs, vs_binning;
   struct tu_dgc_shader_draw_state hs;
   struct tu_dgc_shader_draw_state ds;
   struct tu_dgc_shader_draw_state gs, gs_binning;
   struct tu_dgc_shader_draw_state vpc;
   struct tu_dgc_shader_draw_state fs;
   struct tu_dgc_shader_draw_state patch_control_points;

   uint32_t draw_initiator;

   uint32_t vbo_size;
   uint32_t vbo_stride_size;
   uint32_t user_consts_size;
   uint32_t inline_ubo_commands[GRAPHICS_INLINE_UBO_CMDS_SIZE];
};

static_assert(sizeof(tu_dgc_compute_pipeline_data) <= TU_DGC_PIPELINE_SIZE * 4);
static_assert(sizeof(tu_dgc_graphics_pipeline_data) <= TU_DGC_PIPELINE_SIZE * 4);

static void
emit_patchpoint(struct tu_indirect_command_layout *layout,
                struct tu_dgc_cs *cs, unsigned dwords,
                const struct tu_dgc_patchpoint *patchpoint)
{
   struct tu_dgc_patchpoint patchpoint_out = *patchpoint;
   patchpoint_out.size = dwords;
   patchpoint_out.dst_offset = cs->cs.cur - cs->cs.start;
   tu_cs_emit_array(&cs->patchpoint_cs, (uint32_t *)&patchpoint_out,
                    sizeof(patchpoint_out) / 4);
   cs->patchpoint_count++;
}

static void
emit_direct_src_patchpoint(struct tu_indirect_command_layout *layout,
                           struct tu_dgc_cs *cs, unsigned offset, unsigned dwords)
{
   struct tu_dgc_patchpoint patchpoint = { };
   patchpoint.src_offset = offset;
   patchpoint.type = TU_DGC_PATCHPOINT_SRC_DIRECT;
   emit_patchpoint(layout, cs, dwords, &patchpoint);
   for (unsigned i = 0; i < dwords; i++)
      tu_cs_emit(&cs->cs, 0);
}

static void
_emit_direct_pipeline_patchpoint(struct tu_indirect_command_layout *layout,
                                 struct tu_dgc_cs *cs,
                                 unsigned pipeline_offset,
                                 unsigned dwords)
{
   struct tu_dgc_patchpoint patchpoint = { };
   patchpoint.src_offset = pipeline_offset;
   patchpoint.type = TU_DGC_PATCHPOINT_PIPELINE_DIRECT;
   emit_patchpoint(layout, cs, dwords, &patchpoint);
   for (unsigned i = 0; i < dwords; i++)
      tu_cs_emit(&cs->cs, 0);
}

#define emit_direct_pipeline_patchpoint_compute(layout, cs, pipeline_field, dwords)   \
   _emit_direct_pipeline_patchpoint(layout, cs,                               \
                                    offsetof(struct tu_dgc_compute_pipeline_data, \
                                             pipeline_field) / 4, dwords)

#define emit_direct_pipeline_patchpoint_graphics(layout, cs, pipeline_field, dwords)   \
   _emit_direct_pipeline_patchpoint(layout, cs,                               \
                                    offsetof(struct tu_dgc_graphics_pipeline_data, \
                                             pipeline_field) / 4, dwords)
static void
emit_indirect_src_patchpoint(struct tu_indirect_command_layout *layout,
                             struct tu_dgc_cs *cs, unsigned offset,
                             unsigned mask, unsigned shift)
{
   assert(mask <= UINT16_MAX);
   assert(shift < 64);
   struct tu_dgc_patchpoint patchpoint = { };
   patchpoint.src_offset = offset;
   patchpoint.mask = mask;
   patchpoint.shift = shift;
   patchpoint.type = TU_DGC_PATCHPOINT_SRC_INDIRECT;
   emit_patchpoint(layout, cs, 2, &patchpoint);
   tu_cs_emit(&cs->cs, 0);
   tu_cs_emit(&cs->cs, 0);
}

static void
emit_indirect_dst_patchpoint(struct tu_indirect_command_layout *layout,
                             struct tu_dgc_cs *cs, struct tu_dgc_cs *dst,
                             unsigned offset, unsigned mask, unsigned shift)
{
   assert(mask <= UINT16_MAX);
   assert(shift < 64);
   struct tu_dgc_patchpoint patchpoint = { };
   patchpoint.src_offset = offset;
   patchpoint.src_buffer = dst->idx;
   patchpoint.mask = mask;
   patchpoint.shift = shift;
   patchpoint.type = TU_DGC_PATCHPOINT_DST_INDIRECT;
   emit_patchpoint(layout, cs, 2, &patchpoint);
   tu_cs_emit(&cs->cs, 0);
   tu_cs_emit(&cs->cs, 0);
}

static void
_emit_pipeline_field_patchpoint(struct tu_indirect_command_layout *layout,
                                struct tu_dgc_cs *cs, uint32_t mask,
                                unsigned pipeline_offset, unsigned shift)
{
   assert(shift < 32);
   struct tu_dgc_patchpoint patchpoint = { };
   patchpoint.src_offset = pipeline_offset;
   patchpoint.shift = shift;
   patchpoint.type = TU_DGC_PATCHPOINT_PIPELINE_FIELD;
   emit_patchpoint(layout, cs, 1, &patchpoint);
   tu_cs_emit(&cs->cs, mask);
}

#define emit_pipeline_field_patchpoint_compute(layout, cs, mask, pipeline_field, shift) \
   _emit_pipeline_field_patchpoint(layout, cs, mask,                          \
                                   offsetof(struct tu_dgc_compute_pipeline_data, \
                                            pipeline_field) / 4, shift)

#define emit_pipeline_field_patchpoint_graphics(layout, cs, mask, pipeline_field, shift) \
   _emit_pipeline_field_patchpoint(layout, cs, mask,                          \
                                   offsetof(struct tu_dgc_graphics_pipeline_data, \
                                            pipeline_field) / 4, shift)
static void
emit_sequence_index_patchpoint(struct tu_indirect_command_layout *layout,
                               struct tu_dgc_cs *cs)
{
   struct tu_dgc_patchpoint patchpoint = { };
   patchpoint.type = TU_DGC_PATCHPOINT_SEQUENCE_INDEX;
   emit_patchpoint(layout, cs, 1, &patchpoint);
   tu_cs_emit(&cs->cs, 0);
}

static void
emit_index_patchpoint(struct tu_indirect_command_layout *layout,
                      struct tu_dgc_cs *cs,
                      unsigned offset, bool dxgi_index_types)
{
   struct tu_dgc_patchpoint patchpoint = { };
   patchpoint.type =
      dxgi_index_types ? TU_DGC_PATCHPOINT_INDEX_DX :
      TU_DGC_PATCHPOINT_INDEX_VULKAN;
   patchpoint.src_offset = offset;
   emit_patchpoint(layout, cs, 3, &patchpoint);
   tu_cs_emit(&cs->cs, 0);
   tu_cs_emit(&cs->cs, 0);
   tu_cs_emit(&cs->cs, 0);
}

static void
emit_draw_initiator_patchpoint(struct tu_indirect_command_layout *layout,
                               struct tu_dgc_cs *cs,
                               unsigned offset, unsigned field_shift,
                               bool dxgi_index_types)
{
   struct tu_dgc_patchpoint patchpoint = { };
   patchpoint.type =
      dxgi_index_types ? TU_DGC_PATCHPOINT_DRAW_INITIATOR_DX :
      TU_DGC_PATCHPOINT_DRAW_INITIATOR_VULKAN;
   patchpoint.src_offset = offset;
   patchpoint.shift = field_shift;
   patchpoint.src_buffer = offsetof(struct tu_dgc_graphics_pipeline_data,
                                    draw_initiator) / 4;
   emit_patchpoint(layout, cs, 1, &patchpoint);
   tu_cs_emit(&cs->cs, 0);
}

static void
emit_max_draw_count_patchpoint(struct tu_indirect_command_layout *layout,
                               struct tu_dgc_cs *cs)
{
   struct tu_dgc_patchpoint patchpoint = { };
   patchpoint.type = TU_DGC_PATCHPOINT_MAX_DRAW_COUNT;
   emit_patchpoint(layout, cs, 1, &patchpoint);
   tu_cs_emit(&cs->cs, 0);
}

static void
emit_vbo_patchpoint(struct tu_indirect_command_layout *layout,
                    struct tu_dgc_cs *cs, unsigned offset, unsigned dwords)
{
   struct tu_dgc_patchpoint patchpoint = { };
   patchpoint.src_offset = offset;
   patchpoint.type = TU_DGC_PATCHPOINT_VBO;
   emit_patchpoint(layout, cs, dwords, &patchpoint);
   for (unsigned i = 0; i < dwords; i++)
      tu_cs_emit(&cs->cs, 0);
}


struct tu_dgc_builder {
   bool dxgi_index_types;

   uint32_t draw_params_offset;
   uint32_t index_buffer_offset;

   bool dispatch_copy_driver_params;
   uint32_t dispatch_params_offset;

   uint32_t vbo_offsets[MAX_VBS];

   BITSET_DECLARE(push_constant_mask, MAX_PUSH_CONSTANTS_SIZE / 4);
   BITSET_DECLARE(push_constant_seq_index_mask, MAX_PUSH_CONSTANTS_SIZE / 4);
   uint32_t push_constant_offsets[MAX_PUSH_CONSTANTS_SIZE / 4];

   VkResult result;
};

static VkResult
tu_dgc_cs_begin(struct tu_dgc_cs *cs,
                struct tu_indirect_command_layout *layout,
                uint32_t dwords)
{
   cs->idx = layout->buffer_count++;
   assert(layout->buffer_count <= TU_DGC_MAX_BUFFERS);

   VkResult result =
      tu_cs_begin_sub_stream_aligned(&layout->cs, DIV_ROUND_UP(dwords, 16), 16, &cs->cs);
   if (result != VK_SUCCESS)
      return result;

   return tu_cs_begin_sub_stream_aligned(
      &layout->patchpoint_cs,
      DIV_ROUND_UP(sizeof(struct tu_dgc_patchpoint) * TU_DGC_MAX_PATCHPOINTS, 64), 16,
      &cs->patchpoint_cs);
}

static void
tu_dgc_cs_end(struct tu_dgc_cs *cs,
              struct tu_indirect_command_layout *layout)
{
   layout->buffers[cs->idx] =
      tu_cs_end_draw_state(&layout->cs, &cs->cs);
   layout->patchpoints[cs->idx] =
      tu_cs_end_draw_state(&layout->patchpoint_cs, &cs->patchpoint_cs);
   assert(layout->buffers[cs->idx].size <= TU_DGC_BUFFER_MAX_SIZE / 4);
   assert(layout->patchpoints[cs->idx].size <= TU_DGC_MAX_PATCHPOINTS *
          (sizeof(tu_dgc_patchpoint) / 4));
}

static void
emit_user_consts(struct tu_indirect_command_layout *layout,
                 struct tu_dgc_builder *builder,
                 unsigned push_const_dwords,
                 struct tu_dgc_cs *cs)
{
   if (push_const_dwords) {
      tu_cs_emit_pkt4(&cs->cs, REG_A7XX_HLSQ_SHARED_CONSTS_IMM(0),
                      layout->push_constant_size / 4);
   }

   for (unsigned i = 0; i < push_const_dwords;) {
      if (!BITSET_TEST(builder->push_constant_mask, i)) {
         tu_cs_emit(&cs->cs, 0);
         i++;
         continue;
      }

      unsigned offset = builder->push_constant_offsets[i];

      if (BITSET_TEST(builder->push_constant_seq_index_mask, i)) {
         emit_sequence_index_patchpoint(layout, cs);
         i++;
         continue;
      }

      /* Scan forward looking for a contiguous block of push constants */
      unsigned count;
      for (count = 1; i + count < layout->push_constant_size / 4 &&
           BITSET_TEST(builder->push_constant_mask, i + count) &&
           !BITSET_TEST(builder->push_constant_seq_index_mask, i + count) &&
           builder->push_constant_offsets[i + count] == offset + count * 4;
           count++)
         ;

      emit_direct_src_patchpoint(layout, cs, offset / 4, count);

      i += count;
   }

   if (layout->dispatch) {
      if (layout->bind_pipeline) {
         emit_direct_pipeline_patchpoint_compute(layout, cs,
                                                 inline_ubo_commands,
                                                 SHADER_INLINE_UBO_CMDS_SIZE);
      }
   } else {
      emit_direct_pipeline_patchpoint_graphics(layout, cs,
                                               inline_ubo_commands,
                                               GRAPHICS_INLINE_UBO_CMDS_SIZE);
   }
}

/* We don't know the static push constants until preprocessing, so we have to
 * emit this separately.
 */
static void
emit_user_consts_template(struct tu_cmd_buffer *state_cmd,
                          struct tu_indirect_command_layout *layout,
                          unsigned push_const_dwords,
                          struct tu_cs *cs)
{
   if (push_const_dwords) {
      tu_cs_emit_pkt4(cs, REG_A7XX_HLSQ_SHARED_CONSTS_IMM(0),
                      push_const_dwords);

      tu_cs_emit_array(cs, state_cmd->push_constants, push_const_dwords);
   }

   if (layout->dispatch) {
      if (layout->bind_pipeline) {
         for (unsigned i = 0; i < SHADER_INLINE_UBO_CMDS_SIZE; i++) {
            tu_cs_emit(cs, 0);
         }
      }
   } else {
      for (unsigned i = 0; i < GRAPHICS_INLINE_UBO_CMDS_SIZE; i++) {
         tu_cs_emit(cs, 0);
      }
   }
}

static uint32_t
user_consts_size(struct tu_indirect_command_layout *layout,
                 unsigned push_const_dwords)
{
   uint32_t size =
      push_const_dwords ? (push_const_dwords + 1) : 0;

   if (layout->dispatch) {
      if (layout->bind_pipeline) {
         size += SHADER_INLINE_UBO_CMDS_SIZE;
      }
   } else {
      size += GRAPHICS_INLINE_UBO_CMDS_SIZE;
   }

   return size;
}

static void
emit_compute_driver_params(struct tu_indirect_command_layout *layout,
                           struct tu_dgc_builder *builder,
                           struct tu_dgc_cs *cs)
{
   emit_direct_src_patchpoint(layout, cs,
                              builder->dispatch_params_offset / 4, 
                              sizeof(VkDispatchIndirectCommand) / 4);
   tu_cs_emit(&cs->cs, 0);
   emit_direct_pipeline_patchpoint_compute(layout, cs,
                                           compute_driver_params,
                                           COMPUTE_DP_SIZE);
}

static void
emit_dispatch(struct tu_indirect_command_layout *layout,
              struct tu_dgc_builder *builder,
              struct tu_dgc_cs *cs,
              struct tu_dgc_cs *user_consts_cs,
              struct tu_dgc_cs *dp_cs)
{
   unsigned num_consts = builder->dispatch_copy_driver_params ?
      4 + COMPUTE_DP_SIZE : 4;

   emit_direct_pipeline_patchpoint_compute(layout, cs, driver_param_opcode, 1);
   emit_pipeline_field_patchpoint_compute(layout, cs,
      CP_LOAD_STATE6_0_STATE_TYPE(ST6_UBO) |
      CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) |
      CP_LOAD_STATE6_0_STATE_BLOCK(tu6_stage2shadersb(MESA_SHADER_COMPUTE)) |
      CP_LOAD_STATE6_0_NUM_UNIT(1),
      driver_param_ubo_idx, CP_LOAD_STATE6_0_DST_OFF__SHIFT);
   tu_cs_emit(&cs->cs, CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
   tu_cs_emit(&cs->cs, CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));
   int size_vec4s = DIV_ROUND_UP(num_consts, 4);
   if (builder->dispatch_copy_driver_params) {
      emit_indirect_dst_patchpoint(layout, cs, dp_cs, 0 /* offset */,
                                   size_vec4s, A6XX_UBO_1_SIZE__SHIFT + 32);
   } else {
      emit_indirect_src_patchpoint(layout, cs,
                                   builder->dispatch_params_offset / 4,
                                   size_vec4s,
                                   A6XX_UBO_1_SIZE__SHIFT + 32);
   }

   if (layout->emit_push_constants) {
      tu_cs_emit_pkt7(&cs->cs, CP_INDIRECT_BUFFER, 3);
      emit_indirect_dst_patchpoint(layout, cs, user_consts_cs, 0 /* offset */,
                                   0 /* mask */, 0 /* shift */);
      if (layout->bind_pipeline) {
         emit_direct_pipeline_patchpoint_compute(layout, cs, user_consts_size, 1);
      } else {
         tu_cs_emit(&cs->cs, user_consts_cs->cs.cur - user_consts_cs->cs.start);
      }
   }

   if (layout->bind_pipeline) {
      tu_cs_emit_pkt7(&cs->cs, CP_INDIRECT_BUFFER, 3);
      emit_direct_pipeline_patchpoint_compute(layout, cs, shader_iova, 3);

      tu_cs_emit_pkt4(&cs->cs, REG_A7XX_HLSQ_CS_NDRANGE_0, 1);
      emit_direct_pipeline_patchpoint_compute(layout, cs, cs_ndrange_0, 1);
   }

   tu_cs_emit_pkt7(&cs->cs, CP_EXEC_CS_INDIRECT, 4);
   tu_cs_emit(&cs->cs, 0);
   emit_indirect_src_patchpoint(layout, cs,
                                builder->dispatch_params_offset / 4, 0, 0);
   emit_direct_pipeline_patchpoint_compute(layout, cs, exec_cs_indirect_3, 1);
}

static void
emit_vertex_buffers(struct tu_indirect_command_layout *layout,
                    struct tu_dgc_builder *builder,
                    struct tu_dgc_cs *cs)
{
   for (unsigned i = 0; i < util_last_bit(layout->bind_vbo_mask); i++) {
      if (layout->bind_vbo_mask & (1u << i)) {
         tu_cs_emit_pkt4(&cs->cs, REG_A6XX_VFD_FETCH_BASE(i), 3);
         /* The beginning of VkBindVertexBufferIndirectCommandEXT matches the
          * layout of the registers (base followed by size) but we must set
          * the size to 0 if the base is 0.
          */
         emit_vbo_patchpoint(layout, cs, builder->vbo_offsets[i] / 4, 3);
      } else {
         tu_cs_emit_regs(&cs->cs,
                         A6XX_VFD_FETCH_BASE(i, .qword = 0),
                         A6XX_VFD_FETCH_SIZE(i, 0));
      }
   }
}

/* We don't know the number of vertex buffers bound and the static vertex
 * buffers until preprocess time, so we have to generate the template
 * separately.
 */
static void
emit_vertex_buffers_template(struct tu_cmd_buffer *cmd,
                             struct tu_indirect_command_layout *layout,
                             struct tu_cs *cs)
{
   for (unsigned i = 0; i < MAX2(util_last_bit(layout->bind_vbo_mask),
                                 cmd->state.max_vbs_bound); i++) {
      if (layout->bind_vbo_mask & (1u << i)) {
         tu_cs_emit_regs(cs,
                         A6XX_VFD_FETCH_BASE(i, .qword = 0),
                         A6XX_VFD_FETCH_SIZE(i, 0));
      } else {
         tu_cs_emit_regs(cs,
                         A6XX_VFD_FETCH_BASE(i, .qword = cmd->state.vb[i].base),
                         A6XX_VFD_FETCH_SIZE(i, cmd->state.vb[i].size));
      }
   }
}

#define VERTEX_BUFFERS_MAX_SIZE 4 * MAX_VBS

static uint32_t
vertex_buffers_size(struct tu_indirect_command_layout *layout,
                    struct tu_cmd_buffer *state_cmd)
{
   return 4 * MAX2(util_last_bit(layout->bind_vbo_mask),
                   state_cmd->state.max_vbs_bound);
}

static void
emit_vertex_buffers_stride(struct tu_indirect_command_layout *layout,
                           struct tu_dgc_builder *builder,
                           struct tu_dgc_cs *cs)
{
   for (unsigned i = 0; i < util_last_bit(layout->bind_vbo_mask); i++) {
      if (layout->bind_vbo_mask & (1u << i)) {
         tu_cs_emit_pkt4(&cs->cs, REG_A6XX_VFD_FETCH_STRIDE(i), 1);
         emit_direct_src_patchpoint(layout, cs, 
                                    (builder->vbo_offsets[i] +
                                     offsetof(VkBindVertexBufferIndirectCommandEXT,
                                              stride)) / 4, 1);
      } else {
         tu_cs_emit_regs(&cs->cs,
                         A6XX_VFD_FETCH_STRIDE(i, 0));
      }
   }
}

static void
emit_vertex_buffers_stride_template(struct tu_cmd_buffer *state_cmd,
                                    struct tu_indirect_command_layout *layout,
                                    struct tu_cs *cs)
{
   uint16_t *vi_binding_strides = state_cmd->vk.dynamic_graphics_state.vi_binding_strides;
   for (unsigned i = 0; i < MAX2(util_last_bit(layout->bind_vbo_mask),
                                 state_cmd->state.max_vbs_bound); i++) {
      if (layout->bind_vbo_mask & (1u << i)) {
         tu_cs_emit_regs(cs,
                         A6XX_VFD_FETCH_STRIDE(i, 0));
      } else {
         tu_cs_emit_regs(cs,
            A6XX_VFD_FETCH_STRIDE(i, vi_binding_strides[i]));
      }
   }
}

#define VERTEX_BUFFERS_STRIDE_MAX_SIZE 2 * MAX_VBS

static uint32_t
vertex_buffers_stride_size(struct tu_indirect_command_layout *layout,
                           struct tu_cmd_buffer *state_cmd)
{
   return 2 * MAX2(util_last_bit(layout->bind_vbo_mask),
                   state_cmd->state.max_vbs_bound);
}

static void
_emit_shader_draw_state(struct tu_indirect_command_layout *layout,
                        struct tu_dgc_cs *cs,
                        unsigned pipeline_offset,
                        uint32_t sds_word)
{
   _emit_pipeline_field_patchpoint(layout, cs, sds_word,
                                   pipeline_offset + 2, 0);
   _emit_direct_pipeline_patchpoint(layout, cs, pipeline_offset, 2);
}

#define emit_shader_draw_state(layout, cs, field, sds_word)                   \
   _emit_shader_draw_state(layout, cs,                                        \
                           offsetof(struct tu_dgc_graphics_pipeline_data,     \
                                    field) / 4, sds_word)

static void
emit_draw(struct tu_indirect_command_layout *layout,
          struct tu_dgc_builder *builder,
          struct tu_dgc_cs *cs,
          struct tu_dgc_cs *user_consts_cs,
          struct tu_dgc_cs *vbo_cs,
          struct tu_dgc_cs *vbo_stride_cs)
{
   unsigned draw_states = 0;
   if (layout->emit_push_constants)
      draw_states++;
   if (layout->bind_vbo_mask)
      draw_states += 2;
   if (layout->bind_pipeline)
      draw_states += 10;

   tu_cs_emit_pkt7(&cs->cs, CP_SET_DRAW_STATE, 3 * draw_states);
   if (layout->emit_push_constants) {
      emit_pipeline_field_patchpoint_graphics(layout, cs,
         CP_SET_DRAW_STATE__0_GMEM |
         CP_SET_DRAW_STATE__0_SYSMEM |
         CP_SET_DRAW_STATE__0_BINNING |
         CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_CONST),
         user_consts_size, CP_SET_DRAW_STATE__0_COUNT__SHIFT); 
      emit_indirect_dst_patchpoint(layout, cs, user_consts_cs, 0, 0, 0);
   }
   if (layout->bind_vbo_mask) {
      emit_pipeline_field_patchpoint_graphics(layout, cs,
         CP_SET_DRAW_STATE__0_GMEM |
         CP_SET_DRAW_STATE__0_SYSMEM |
         CP_SET_DRAW_STATE__0_BINNING |
         CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_VB),
         vbo_size, CP_SET_DRAW_STATE__0_COUNT__SHIFT); 
      emit_indirect_dst_patchpoint(layout, cs, vbo_cs, 0, 0, 0);

      emit_pipeline_field_patchpoint_graphics(layout, cs,
         CP_SET_DRAW_STATE__0_GMEM |
         CP_SET_DRAW_STATE__0_SYSMEM |
         CP_SET_DRAW_STATE__0_BINNING |
         CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_DYNAMIC +
                                       TU_DYNAMIC_STATE_VB_STRIDE),
         vbo_stride_size, CP_SET_DRAW_STATE__0_COUNT__SHIFT); 
      emit_indirect_dst_patchpoint(layout, cs, vbo_stride_cs, 0, 0, 0);
   }
   if (layout->bind_pipeline) {
      emit_shader_draw_state(layout, cs, program_config,
                             CP_SET_DRAW_STATE__0_GMEM |
                             CP_SET_DRAW_STATE__0_SYSMEM |
                             CP_SET_DRAW_STATE__0_BINNING |
                             CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_PROGRAM_CONFIG));
      emit_shader_draw_state(layout, cs, vs,
                             CP_SET_DRAW_STATE__0_GMEM |
                             CP_SET_DRAW_STATE__0_SYSMEM |
                             CP_SET_DRAW_STATE__0_DIRTY |
                             CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_VS));
      emit_shader_draw_state(layout, cs, vs_binning,
                             CP_SET_DRAW_STATE__0_BINNING |
                             CP_SET_DRAW_STATE__0_DIRTY |
                             CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_VS_BINNING));
      emit_shader_draw_state(layout, cs, hs,
                             CP_SET_DRAW_STATE__0_GMEM |
                             CP_SET_DRAW_STATE__0_SYSMEM |
                             CP_SET_DRAW_STATE__0_BINNING |
                             CP_SET_DRAW_STATE__0_DIRTY |
                             CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_HS));
      emit_shader_draw_state(layout, cs, ds,
                             CP_SET_DRAW_STATE__0_GMEM |
                             CP_SET_DRAW_STATE__0_SYSMEM |
                             CP_SET_DRAW_STATE__0_BINNING |
                             CP_SET_DRAW_STATE__0_DIRTY |
                             CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_DS));
      emit_shader_draw_state(layout, cs, gs,
                             CP_SET_DRAW_STATE__0_GMEM |
                             CP_SET_DRAW_STATE__0_SYSMEM |
                             CP_SET_DRAW_STATE__0_DIRTY |
                             CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_GS));
      emit_shader_draw_state(layout, cs, gs_binning,
                             CP_SET_DRAW_STATE__0_BINNING |
                             CP_SET_DRAW_STATE__0_DIRTY |
                             CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_GS_BINNING));
      emit_shader_draw_state(layout, cs, vpc,
                             CP_SET_DRAW_STATE__0_GMEM |
                             CP_SET_DRAW_STATE__0_SYSMEM |
                             CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_VPC));
      emit_shader_draw_state(layout, cs, fs,
                             CP_SET_DRAW_STATE__0_GMEM |
                             CP_SET_DRAW_STATE__0_SYSMEM |
                             CP_SET_DRAW_STATE__0_DIRTY |
                             CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_FS));
      emit_shader_draw_state(layout, cs, patch_control_points,
                             CP_SET_DRAW_STATE__0_GMEM |
                             CP_SET_DRAW_STATE__0_SYSMEM |
                             CP_SET_DRAW_STATE__0_BINNING |
                             CP_SET_DRAW_STATE__0_GROUP_ID(TU_DRAW_STATE_DYNAMIC +
                                                           TU_DYNAMIC_STATE_PATCH_CONTROL_POINTS));

      if (layout->tess) {
         tu_cs_emit_pkt4(&cs->cs, REG_A6XX_PC_TESS_CNTL, 1);
         emit_direct_pipeline_patchpoint_graphics(layout, cs, pc_tess_cntl, 1);
      }
   }
   
   tu_cs_emit_pkt7(&cs->cs, CP_DRAW_INDIRECT_MULTI,
                   3 + (layout->draw_indexed ? 3 : 0) +
                   (layout->draw_indirect_count ? 5 : 3));
   if (layout->bind_index_buffer) {
      emit_draw_initiator_patchpoint(layout, cs, builder->index_buffer_offset / 4,
                                     CP_DRAW_INDX_OFFSET_0_INDEX_SIZE__SHIFT,
                                     builder->dxgi_index_types);
   } else {
      emit_direct_pipeline_patchpoint_graphics(layout, cs, draw_initiator, 1);
   }
   enum a6xx_draw_indirect_opcode opcode;
   if (layout->draw_indirect_count) {
      opcode = layout->draw_indexed ? INDIRECT_OP_INDIRECT_COUNT_INDEXED :
         INDIRECT_OP_INDIRECT_COUNT;
   } else {
      opcode = layout->draw_indexed ? INDIRECT_OP_INDEXED :
         INDIRECT_OP_NORMAL;
   }
   emit_pipeline_field_patchpoint_graphics(layout, cs,
      A6XX_CP_DRAW_INDIRECT_MULTI_1_OPCODE(opcode),
      vs_params_offset, A6XX_CP_DRAW_INDIRECT_MULTI_1_DST_OFF__SHIFT);
   if (layout->draw_indirect_count) {
      emit_max_draw_count_patchpoint(layout, cs);
   } else {
      tu_cs_emit(&cs->cs, 1);
   }
   if (layout->draw_indexed) {
      if (layout->bind_index_buffer) {
         emit_index_patchpoint(layout, cs, builder->index_buffer_offset / 4,
                               builder->dxgi_index_types);
      } else {
         emit_direct_pipeline_patchpoint_graphics(layout, cs, index_base, 3);
      }
   }
   if (layout->draw_indirect_count) {
      emit_direct_src_patchpoint(layout, cs,
                                 (builder->draw_params_offset +
                                  offsetof(VkDrawIndirectCountIndirectCommandEXT,
                                           bufferAddress)) / 4, 2);
      emit_indirect_src_patchpoint(layout, cs,
                                   (builder->draw_params_offset +
                                    offsetof(VkDrawIndirectCountIndirectCommandEXT,
                                             commandCount)) / 4, 0, 0);
      emit_direct_src_patchpoint(layout, cs,
                                 (builder->draw_params_offset +
                                  offsetof(VkDrawIndirectCountIndirectCommandEXT,
                                           stride)) / 4, 1);
   } else {
      emit_indirect_src_patchpoint(layout, cs,
                                   builder->draw_params_offset / 4, 0, 0);
      tu_cs_emit(&cs->cs, 0); /* stride is unused */
   }
}

static VkResult
emit(struct tu_indirect_command_layout *layout,
     struct tu_dgc_builder *builder)
{
   VkResult result;

   struct tu_dgc_cs user_consts_cs;
   result =
      tu_dgc_cs_begin(&user_consts_cs, layout,
                      MAX2(user_consts_size(layout, layout->push_constant_size / 4), 1));

   if (result != VK_SUCCESS)
      return result;

   if (layout->emit_push_constants)
      emit_user_consts(layout, builder, layout->push_constant_size / 4, &user_consts_cs);

   tu_dgc_cs_end(&user_consts_cs, layout);
   layout->user_consts_cs_idx = user_consts_cs.idx;

   layout->vertex_buffer_idx = -1;
   layout->vertex_buffer_stride_idx = -1;

   if (layout->dispatch) {
      struct tu_dgc_cs dp_cs;
      if (builder->dispatch_copy_driver_params) {
         result = tu_dgc_cs_begin(&dp_cs, layout, 4 + COMPUTE_DP_SIZE);
         if (result != VK_SUCCESS)
            return result;
         emit_compute_driver_params(layout, builder, &dp_cs);
         tu_dgc_cs_end(&dp_cs, layout);
      }
      
      struct tu_dgc_cs cs;
      result = tu_dgc_cs_begin(&cs, layout, 6 + 4 + 2 + 5); 

      if (result != VK_SUCCESS)
         return result;

      emit_dispatch(layout, builder, &cs, &user_consts_cs, &dp_cs);

      tu_dgc_cs_end(&cs, layout);
      layout->main_cs_idx = cs.idx;
   } else {
      struct tu_dgc_cs vbo_cs, vbo_stride_cs;
      if (layout->bind_vbo_mask) {
         result = tu_dgc_cs_begin(&vbo_cs, layout, 4 *
                                  util_last_bit(layout->bind_vbo_mask));
         if (result != VK_SUCCESS)
            return result;

         emit_vertex_buffers(layout, builder, &vbo_cs);
         tu_dgc_cs_end(&vbo_cs, layout);
         layout->vertex_buffer_idx = vbo_cs.idx;

         result = tu_dgc_cs_begin(&vbo_stride_cs, layout, 2 *
                                  util_last_bit(layout->bind_vbo_mask));
         if (result != VK_SUCCESS)
            return result;

         emit_vertex_buffers_stride(layout, builder, &vbo_stride_cs);
         tu_dgc_cs_end(&vbo_stride_cs, layout);
         layout->vertex_buffer_idx = vbo_cs.idx;
         layout->vertex_buffer_stride_idx = vbo_stride_cs.idx;
      }

      unsigned draw_states = 3 + (layout->bind_pipeline ? 10 : 0);
      struct tu_dgc_cs cs;
      result = tu_dgc_cs_begin(&cs, layout, 1 + 3 * draw_states + 12); 

      if (result != VK_SUCCESS)
         return result;

      emit_draw(layout, builder, &cs, &user_consts_cs, &vbo_cs, &vbo_stride_cs);

      tu_dgc_cs_end(&cs, layout);
      layout->main_cs_idx = cs.idx;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateIndirectCommandsLayoutEXT(VkDevice _device, const VkIndirectCommandsLayoutCreateInfoEXT *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkIndirectCommandsLayoutEXT *pIndirectCommandsLayout)
{
   VK_FROM_HANDLE(tu_device, device, _device);
   VK_FROM_HANDLE(tu_pipeline_layout, pipeline_layout,
                  pCreateInfo->pipelineLayout);
   struct tu_indirect_command_layout *layout;
   struct tu_dgc_builder builder = {};

   layout = (struct tu_indirect_command_layout *)
      vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*layout),
                 alignof(struct tu_indirect_command_layout),
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!layout)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &layout->base,
                       VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT);

   layout->flags = pCreateInfo->flags;
   layout->input_stride = pCreateInfo->indirectStride;
   layout->tess = pCreateInfo->shaderStages &
      VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;

   for (unsigned i = 0; i < pCreateInfo->tokenCount; ++i) {
      switch (pCreateInfo->pTokens[i].type) {
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT:
         builder.draw_params_offset = pCreateInfo->pTokens[i].offset;
         break;
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT:
         layout->draw_indexed = true;
         builder.draw_params_offset = pCreateInfo->pTokens[i].offset;
         break;
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_COUNT_EXT:
         layout->draw_indirect_count = true;
         builder.draw_params_offset = pCreateInfo->pTokens[i].offset;
         break;
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_COUNT_EXT:
         layout->draw_indirect_count = true;
         layout->draw_indexed = true;
         builder.draw_params_offset = pCreateInfo->pTokens[i].offset;
         break;
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT:
         layout->dispatch = true;
         builder.dispatch_params_offset = pCreateInfo->pTokens[i].offset;
         builder.dispatch_copy_driver_params =
            (builder.dispatch_params_offset & 0xf) ||
            true; /* TODO remove this once we rewrite compute driver params */
         break;
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT:
         layout->bind_index_buffer = true;
         builder.index_buffer_offset = pCreateInfo->pTokens[i].offset;
         builder.dxgi_index_types =
            pCreateInfo->pTokens[i].data.pIndexBuffer->mode ==
            VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT;
         break;
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_VERTEX_BUFFER_EXT: {
         unsigned unit =
            pCreateInfo->pTokens[i].data.pVertexBuffer->vertexBindingUnit;
         layout->bind_vbo_mask |= 1u << unit;
         builder.vbo_offsets[unit] = pCreateInfo->pTokens[i].offset;
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT:
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_SEQUENCE_INDEX_EXT: {
         const VkPushConstantRange *range =
            &pCreateInfo->pTokens[i].data.pPushConstant->updateRange;
         for (unsigned j = range->offset / 4, k = 0;
              k < range->size / 4; ++j, ++k) {
            BITSET_SET(builder.push_constant_mask, j);
            if (pCreateInfo->pTokens[i].type ==
                VK_INDIRECT_COMMANDS_TOKEN_TYPE_SEQUENCE_INDEX_EXT)
               BITSET_SET(builder.push_constant_seq_index_mask, j);
            else
               builder.push_constant_offsets[j] = pCreateInfo->pTokens[i].offset + k * 4;
         }
         layout->emit_push_constants = true;
         layout->push_constant_size = pipeline_layout->push_constant_size;
         break;
      }
      case VK_INDIRECT_COMMANDS_TOKEN_TYPE_EXECUTION_SET_EXT: {
         layout->bind_pipeline = true;
         layout->pipeline_offset = pCreateInfo->pTokens[i].offset;
         break;
      }
      default:
         unreachable("Unhandled token type");
      }
   }

   /* For graphics, inline uniforms are in the same draw state as push
    * constants, and they are pipeline-specific.
    */
   layout->emit_push_constants |= layout->bind_pipeline;

   tu_cs_init(&layout->cs, device, TU_CS_MODE_SUB_STREAM,
              4096, "dgc commands");
   tu_cs_init(&layout->patchpoint_cs, device, TU_CS_MODE_SUB_STREAM,
              4096, "dgc patchpoints");

   VkResult result = emit(layout, &builder);
   if (result != VK_SUCCESS) {
      tu_cs_finish(&layout->cs);
      tu_cs_finish(&layout->patchpoint_cs);
      vk_free2(&device->vk.alloc, pAllocator, layout);
      return result;
   }

   *pIndirectCommandsLayout = tu_indirect_command_layout_to_handle(layout);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
tu_DestroyIndirectCommandsLayoutEXT(
    VkDevice                                    _device,
    VkIndirectCommandsLayoutEXT                 indirectCommandsLayout,
    const VkAllocationCallbacks*                pAllocator)
{
   VK_FROM_HANDLE(tu_device, device, _device);
   VK_FROM_HANDLE(tu_indirect_command_layout, layout, indirectCommandsLayout);

   if (!layout)
      return;

   tu_cs_finish(&layout->cs);
   tu_cs_finish(&layout->patchpoint_cs);
   vk_object_base_finish(&layout->base);
   vk_free2(&device->vk.alloc, pAllocator, layout);
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateIndirectExecutionSetEXT(
    VkDevice                                   _device,
    const VkIndirectExecutionSetCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks*               pAllocator,
    VkIndirectExecutionSetEXT*                 pIndirectExecutionSet)
{
   VK_FROM_HANDLE(tu_device, device, _device);

   size_t size = sizeof(struct tu_indirect_execution_set);
   size += pCreateInfo->info.pPipelineInfo->maxPipelineCount *
      sizeof(struct tu_pipeline *);
   struct tu_indirect_execution_set *iset =
      (struct tu_indirect_execution_set *)
      vk_zalloc2(&device->vk.alloc, pAllocator, size, alignof(struct tu_indirect_execution_set),
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!iset)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &iset->base, VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT);

   VK_FROM_HANDLE(tu_pipeline, pipeline,
                  pCreateInfo->info.pPipelineInfo->initialPipeline);
   iset->pipelines[0] = pipeline;
   iset->pipeline_count = 1;

   *pIndirectExecutionSet = tu_indirect_execution_set_to_handle(iset);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
tu_DestroyIndirectExecutionSetEXT(
    VkDevice                      _device,
    VkIndirectExecutionSetEXT     indirectExecutionSet,
    const VkAllocationCallbacks*  pAllocator)
{
   VK_FROM_HANDLE(tu_device, device, _device);
   VK_FROM_HANDLE(tu_indirect_execution_set, iset, indirectExecutionSet);

   if (!iset)
      return;

   vk_object_base_finish(&iset->base);
   vk_free2(&device->vk.alloc, pAllocator, iset);
}

VKAPI_ATTR void VKAPI_CALL
tu_UpdateIndirectExecutionSetPipelineEXT(
    VkDevice                              device,
    VkIndirectExecutionSetEXT             indirectExecutionSet,
    uint32_t                              executionSetWriteCount,
    const VkWriteIndirectExecutionSetPipelineEXT* pExecutionSetWrites)
{
   VK_FROM_HANDLE(tu_indirect_execution_set, iset, indirectExecutionSet);

   for (unsigned i = 0; i < executionSetWriteCount; i++) {
      VK_FROM_HANDLE(tu_pipeline, pipeline, pExecutionSetWrites[i].pipeline);
      iset->pipelines[pExecutionSetWrites[i].index] = pipeline;
      iset->pipeline_count = MAX2(iset->pipeline_count,
                                  pExecutionSetWrites[i].index + 1);
   }
}

static void
emit_direct_compute_pipeline(struct tu_cmd_buffer *cmd)
{
   struct tu_cs *cs = &cmd->cs;
   struct tu_shader *shader = cmd->state.shaders[MESA_SHADER_COMPUTE];
   const uint16_t *local_size = shader->variant->local_size;

   tu_cs_emit_regs(cs,
                   HLSQ_CS_NDRANGE_0(A7XX, .kerneldim = 3,
                                           .localsizex = local_size[0] - 1,
                                           .localsizey = local_size[1] - 1,
                                           .localsizez = local_size[2] - 1));

   bool emit_instrlen_workaround =
      shader->variant->instrlen >
      cmd->device->physical_device->info->a6xx.instr_cache_size;

   if (emit_instrlen_workaround) {
      tu_cs_emit_regs(cs, A6XX_SP_FS_INSTRLEN(shader->variant->instrlen));
      tu_emit_event_write<A7XX>(cmd, cs, FD_LABEL);
   }
}

void
tu_dgc_begin(struct tu_cmd_buffer *cmd,
             const VkGeneratedCommandsInfoEXT *info)
{
   VK_FROM_HANDLE(tu_indirect_command_layout, layout,
                  info->indirectCommandsLayout);

   if (layout->dispatch) {
      /* If we didn't emit the push constants as part of the indirect command
       * buffer, emit them here.
       */
      if (!layout->emit_push_constants) {
         assert(!layout->bind_pipeline);
         tu_cs_emit_state_ib(&cmd->cs, tu_emit_consts(cmd, true));
      } else if (!layout->bind_pipeline) {
         struct tu_shader *shader = cmd->state.shaders[MESA_SHADER_COMPUTE];
         tu_emit_inline_ubo(
            &cmd->cs, &shader->const_state,
            shader->variant->const_state,
            shader->variant->constlen,
            MESA_SHADER_COMPUTE,
            tu_get_descriptors_state(cmd, VK_PIPELINE_BIND_POINT_COMPUTE));
      }

      if (!layout->bind_pipeline)
         emit_direct_compute_pipeline(cmd);
   }
}

static void
emit_direct_compute_pipeline_end(struct tu_cmd_buffer *cmd)
{
   struct tu_cs *cs = &cmd->cs;
   struct tu_shader *shader = cmd->state.shaders[MESA_SHADER_COMPUTE];

   bool emit_instrlen_workaround =
      shader->variant->instrlen >
      cmd->device->physical_device->info->a6xx.instr_cache_size;

   if (emit_instrlen_workaround) {
      tu_emit_event_write<A7XX>(cmd, cs, FD_LABEL);
   }
}

void
tu_dgc_end(struct tu_cmd_buffer *cmd,
           const VkGeneratedCommandsInfoEXT *info)
{
   VK_FROM_HANDLE(tu_indirect_command_layout, layout,
                  info->indirectCommandsLayout);

   if (layout->dispatch && !layout->bind_pipeline)
      emit_direct_compute_pipeline_end(cmd);
}

struct tu_preprocess_layout {
   uint64_t trampoline_offset;
   uint64_t buffers_offset[TU_DGC_MAX_BUFFERS];
   uint64_t pipeline_offset;
   uint64_t size;
   uint32_t sequences_per_ib;
   uint32_t max_ibs;
};

/* TODO make this a common define and use in tu_cs */
#define MAX_IB_DWORDS 0x0fffff

static void
alloc_preprocess_buffer(struct tu_device *device,
                        struct tu_indirect_command_layout *layout,
                        struct tu_pipeline **pipelines,
                        unsigned pipeline_count,
                        uint32_t max_sequence_count,
                        struct tu_preprocess_layout *preprocess)
{
   uint64_t size = 0;
   preprocess->trampoline_offset = size;
   /* CP_INDIRECT_BUFFER_CHAIN + address + size */
   size += 4 * sizeof(uint32_t);

   for (unsigned i = 0; i < layout->buffer_count; i++) {
      /* Some buffers are used as UBOs, so make sure they are aligned for
       * that.
       */
      size = align64(size, 64);
      preprocess->buffers_offset[i] = size;

      /* TODO: switch over to using the pipelines to determine the VBO draw
       * state size, so that we can use that here to avoid always
       * allocating the max size.
       */
      unsigned buffer_size = layout->buffers[i].size;
      if (i == layout->vertex_buffer_idx)
         buffer_size = VERTEX_BUFFERS_MAX_SIZE;
      if (i == layout->vertex_buffer_stride_idx)
         buffer_size = VERTEX_BUFFERS_STRIDE_MAX_SIZE;

      size += buffer_size * sizeof(uint32_t) * max_sequence_count;
      if (i == layout->main_cs_idx) {
         preprocess->sequences_per_ib =
            (MAX_IB_DWORDS - 4) / layout->buffers[i].size;
         preprocess->max_ibs =
            DIV_ROUND_UP(max_sequence_count, preprocess->sequences_per_ib);
         /* Each extra IB after the first one will need an extra trampoline to
          * jump to the next one.
          */
         size += 4 * sizeof(uint32_t) * (preprocess->max_ibs - 1);
      }
   }

   size = align64(size, 64);
   preprocess->pipeline_offset = size;

   if (!layout->dispatch || layout->bind_pipeline) {
      for (unsigned i = 0; i < pipeline_count; i++) {
         if (!pipelines[i])
            continue;

         struct tu_pipeline *pipeline = pipelines[i];
         for (unsigned j = 0; j < ARRAY_SIZE(pipeline->shaders); j++) {
            if (!pipeline->shaders[j] || !pipeline->shaders[j]->variant)
               continue;

            size = align64(size, 64);
            size += sizeof(uint64_t) *
               pipeline->shaders[j]->const_state.num_inline_ubos;
         }

         if (pipeline->shaders[MESA_SHADER_TESS_CTRL] &&
             pipeline->shaders[MESA_SHADER_TESS_CTRL]->variant) {
            size += tu6_patch_control_points_size<A7XX>(
               device, pipeline->shaders[MESA_SHADER_VERTEX],
               pipeline->shaders[MESA_SHADER_TESS_CTRL],
               pipeline->shaders[MESA_SHADER_TESS_EVAL],
               &pipeline->program, 0);
         }
      }
   }

   preprocess->size = size;
}

VKAPI_ATTR void VKAPI_CALL 
tu_GetGeneratedCommandsMemoryRequirementsEXT(
    VkDevice                                    _device,
    const VkGeneratedCommandsMemoryRequirementsInfoEXT* pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements)
{
   VK_FROM_HANDLE(tu_device, device, _device);
   VK_FROM_HANDLE(tu_indirect_command_layout, layout,
                  pInfo->indirectCommandsLayout);
   VK_FROM_HANDLE(tu_indirect_execution_set, iset,
                  pInfo->indirectExecutionSet);

   struct tu_pipeline *pipeline;
   struct tu_pipeline **pipelines;
   unsigned pipeline_count = 1;

   if (iset) {
      pipelines = iset->pipelines;
      pipeline_count = iset->pipeline_count;
   } else {
      const VkGeneratedCommandsPipelineInfoEXT *pipeline_info =
         vk_find_struct_const(pInfo->pNext,
                              GENERATED_COMMANDS_PIPELINE_INFO_EXT);

      VK_FROM_HANDLE(tu_pipeline, single_pipeline, pipeline_info->pipeline);
      pipeline = single_pipeline;
      pipelines = &pipeline;
      pipeline_count = 1;
   }

   struct tu_preprocess_layout preprocess;
   alloc_preprocess_buffer(device, layout, pipelines, pipeline_count,
                           pInfo->maxSequenceCount, &preprocess);
   pMemoryRequirements->memoryRequirements.size = preprocess.size;
   pMemoryRequirements->memoryRequirements.alignment = 16; /* UBO alignment */
   pMemoryRequirements->memoryRequirements.memoryTypeBits =
      (1 << device->physical_device->memory.type_count) - 1;
}

static uint32_t
emit_pipeline_inline_uniforms(struct tu_indirect_command_layout *layout,
                              struct tu_pipeline *pipeline,
                              uint32_t push_const_dwords,
                              struct tu_descriptor_state *descriptors,
                              uint64_t *preprocess_iova,
                              struct tu_cs *cs,
                              struct tu_cs *preprocess_cs)
{
   uint64_t inline_ubo_va[ARRAY_SIZE(pipeline->shaders)] = { };
   uint64_t iova = *preprocess_iova;
   iova = align64(iova, 64);

   for (unsigned i = 0; i < ARRAY_SIZE(pipeline->shaders); i++) {
      if (!pipeline->shaders[i] ||
          !pipeline->shaders[i]->variant)
         continue;

      const struct tu_const_state *const_state =
         &pipeline->shaders[i]->const_state;
      if (!const_state->num_inline_ubos)
         continue;

      /* Emit the packets to setup the UBO with pointers to the data to push
       * at preprocess time.
       */
      tu_cs_emit_pkt7(cs, CP_MEM_WRITE, 2 + 2 * const_state->num_inline_ubos);
      tu_cs_emit_qw(cs, iova);
      for (unsigned j = 0; j < const_state->num_inline_ubos; j++) {
         const struct tu_inline_ubo *ubo = &const_state->ubos[j];

         uint64_t va = descriptors->set_iova[ubo->base] & ~0x3f;
         tu_cs_emit_qw(cs, va + ubo->offset);
      }

      inline_ubo_va[i] = iova;
      iova += align64(8 * const_state->num_inline_ubos, 64);
   }

   *preprocess_iova = iova;

   for (unsigned i = 0; i < ARRAY_SIZE(pipeline->shaders); i++) {
      if (!pipeline->shaders[i] ||
          !pipeline->shaders[i]->variant)
         continue;

      const struct tu_const_state *const_state =
         &pipeline->shaders[i]->const_state;
      if (!const_state->num_inline_ubos)
         continue;

      uint64_t iova = inline_ubo_va[i];

      tu_cs_emit_pkt7(preprocess_cs, tu6_stage2opcode((gl_shader_stage)i), 5);
      tu_cs_emit(preprocess_cs, CP_LOAD_STATE6_0_DST_OFF(const_state->inline_uniforms_ubo.idx) |
               CP_LOAD_STATE6_0_STATE_TYPE(ST6_UBO) |
               CP_LOAD_STATE6_0_STATE_SRC(SS6_DIRECT) |
               CP_LOAD_STATE6_0_STATE_BLOCK(tu6_stage2shadersb((gl_shader_stage)i)) |
               CP_LOAD_STATE6_0_NUM_UNIT(1));
      tu_cs_emit(preprocess_cs, CP_LOAD_STATE6_1_EXT_SRC_ADDR(0));
      tu_cs_emit(preprocess_cs, CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0));
      int size_vec4s = DIV_ROUND_UP(const_state->num_inline_ubos * 2, 4);
      tu_cs_emit_qw(preprocess_cs, iova | ((uint64_t)A6XX_UBO_1_SIZE(size_vec4s) << 32));
   }

   /* Return the total size of the user_consts CS after inline uniforms are
    * factored in.
    */

   return preprocess_cs->cur - preprocess_cs->start +
      (push_const_dwords ? push_const_dwords + 1 : 0);
}

static void
setup_compute_pipeline(struct tu_indirect_command_layout *layout,
                       struct tu_cmd_buffer *cmd,
                       struct tu_cs *cs,
                       struct tu_pipeline *pipeline,
                       uint32_t push_const_dwords,
                       void *mem,
                       uint64_t *preprocess_iova)
{
   struct tu_shader *shader = pipeline->shaders[MESA_SHADER_COMPUTE];

   struct tu_dgc_compute_pipeline_data *data =
      (struct tu_dgc_compute_pipeline_data *) mem;

   if (shader->variant->const_state->driver_params_ubo.size == 0) {
      data->driver_param_opcode = pm4_pkt7_hdr(CP_NOP, 5);
   } else {
      data->driver_param_opcode =
         pm4_pkt7_hdr(tu6_stage2opcode(MESA_SHADER_COMPUTE), 5);
      data->driver_param_ubo_idx =
         shader->variant->const_state->driver_params_ubo.idx;
   }

   unsigned subgroup_size = shader->variant->info.subgroup_size;
   unsigned subgroup_shift = util_logbase2(subgroup_size);
   const uint16_t *local_size = shader->variant->local_size;
   struct ir3_driver_params_cs driver_params = {};
   driver_params.subgroup_size = subgroup_size;
   driver_params.subgroup_id_shift = subgroup_shift;

   /* The first 4 params are the group count, skip them */
   memcpy(data->compute_driver_params, &driver_params.base_group_x,
          sizeof(data->compute_driver_params));

   data->cs_ndrange_0 =
      HLSQ_CS_NDRANGE_0(A7XX, .kerneldim = 3,
                              .localsizex = local_size[0] - 1,
                              .localsizey = local_size[1] - 1,
                              .localsizez = local_size[2] - 1).value;

   data->exec_cs_indirect_3 =
      A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEX(local_size[0] - 1) |
      A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEY(local_size[1] - 1) |
      A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEZ(local_size[2] - 1);

   if (layout->bind_pipeline) {
      struct tu_descriptor_state *descriptors =
         &cmd->descriptors[VK_PIPELINE_BIND_POINT_COMPUTE];
      struct tu_cs iub_cs;
      tu_cs_init_external(&iub_cs, cmd->device, &data->inline_ubo_commands[0],
                          &data->inline_ubo_commands[SHADER_INLINE_UBO_CMDS_SIZE],
                          0, false);
      tu_cs_reserve_space(&iub_cs, SHADER_INLINE_UBO_CMDS_SIZE);
      data->user_consts_size =
         emit_pipeline_inline_uniforms(layout, pipeline, push_const_dwords,
                                       descriptors, preprocess_iova, cs,
                                       &iub_cs);
   }

   data->shader_iova = shader->state.iova;
   data->shader_size = shader->state.size;
}

static uint32_t
get_draw_initiator(struct tu_indirect_command_layout *layout,
                   struct tu_cmd_buffer *state_cmd,
                   struct tu_graphics_pipeline *pipeline)
{
   VkPrimitiveTopology topology =
      (VkPrimitiveTopology)state_cmd->vk.dynamic_graphics_state.ia.primitive_topology;

   unsigned patch_control_points =
      state_cmd->vk.dynamic_graphics_state.ts.patch_control_points;

   /* If the index buffer is dynamic, then the index size must be patched in
    * during preprocessing.
    */
   enum a4xx_index_size index_size =
      (enum a4xx_index_size)(layout->bind_index_buffer ? 0 :
                             state_cmd->state.index_size);

   return tu_draw_initiator_from_state(topology, patch_control_points,
                                       pipeline->base.shaders,
                                       index_size,
                                       layout->draw_indexed ?
                                       DI_SRC_SEL_DMA :
                                       DI_SRC_SEL_AUTO_INDEX);
}

static uint32_t
vs_params_offset(struct tu_graphics_pipeline *pipeline)
{
   const struct tu_program_descriptor_linkage *link =
      &pipeline->base.program.link[MESA_SHADER_VERTEX];
   const struct ir3_const_state *const_state = &link->const_state;

   if (const_state->offsets.driver_param >= link->constlen)
      return 0;

   /* 0 means disabled for CP_DRAW_INDIRECT_MULTI */
   assert(const_state->offsets.driver_param != 0);

   return const_state->offsets.driver_param;
}

static void
setup_graphics_pipeline(struct tu_indirect_command_layout *layout,
                        struct tu_cmd_buffer *cmd,
                        struct tu_cmd_buffer *state_cmd,
                        struct tu_cs *cs,
                        struct tu_graphics_pipeline *pipeline,
                        uint32_t push_const_dwords,
                        void *mem,
                        uint64_t *preprocess_iova)
{
   struct tu_descriptor_state *descriptors =
      &state_cmd->descriptors[VK_PIPELINE_BIND_POINT_GRAPHICS];

   struct tu_dgc_graphics_pipeline_data *data =
      (struct tu_dgc_graphics_pipeline_data *) mem;

   data->draw_initiator = get_draw_initiator(layout, state_cmd, pipeline);
   if (layout->tess) {
      bool tess_upper_left_domain_origin =
         (VkTessellationDomainOrigin)state_cmd->vk.dynamic_graphics_state.ts.domain_origin ==
         VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT;
      struct tu_shader *tes = pipeline->base.shaders[MESA_SHADER_TESS_EVAL];
      data->pc_tess_cntl = A6XX_PC_TESS_CNTL(
         .spacing = tes->tes.tess_spacing,
         .output = tess_upper_left_domain_origin ?
            tes->tes.tess_output_upper_left :
            tes->tes.tess_output_lower_left).value;
   }

   data->vs_params_offset = vs_params_offset(pipeline);

   data->index_base = state_cmd->state.index_va;
   data->max_index = state_cmd->state.max_index_count;

   data->vbo_size = vertex_buffers_size(layout, state_cmd);
   data->vbo_stride_size = vertex_buffers_stride_size(layout, state_cmd);

   struct tu_cs iub_cs;
   tu_cs_init_external(&iub_cs, cmd->device, &data->inline_ubo_commands[0],
                       &data->inline_ubo_commands[GRAPHICS_INLINE_UBO_CMDS_SIZE],
                       0, false);
   tu_cs_reserve_space(&iub_cs, GRAPHICS_INLINE_UBO_CMDS_SIZE);
   data->user_consts_size =
      emit_pipeline_inline_uniforms(layout, &pipeline->base,
                                    push_const_dwords, descriptors,
                                    preprocess_iova, cs, &iub_cs);

   if (layout->bind_pipeline) {
      struct tu_program_state *program = &pipeline->base.program;
      data->program_config = emit_draw_state(program->config_state);
      data->vs = emit_draw_state(program->vs_state);
      data->vs_binning = emit_draw_state(program->vs_binning_state);
      data->hs = emit_draw_state(program->hs_state);
      data->ds = emit_draw_state(program->ds_state);
      data->gs = emit_draw_state(program->gs_state);
      data->gs_binning = emit_draw_state(program->gs_binning_state);
      data->vpc = emit_draw_state(program->vpc_state);
      data->fs = emit_draw_state(program->fs_state);

      unsigned patch_control_points =
         state_cmd->vk.dynamic_graphics_state.ts.patch_control_points;
      unsigned max_size =
         tu6_patch_control_points_size<A7XX>(cmd->device,
                                             pipeline->base.shaders[MESA_SHADER_VERTEX],
                                             pipeline->base.shaders[MESA_SHADER_TESS_CTRL],
                                             pipeline->base.shaders[MESA_SHADER_TESS_EVAL],
                                             program, patch_control_points);
      uint32_t *pcp_data = (uint32_t *)malloc(sizeof(uint32_t) * max_size);

      struct tu_cs pcp_cs;
      tu_cs_init_external(&pcp_cs, cmd->device, pcp_data, pcp_data + max_size,
                          *preprocess_iova, false);
      tu_cs_reserve_space(&pcp_cs, max_size);
      tu6_emit_patch_control_points<A7XX>(&pcp_cs,
                                          pipeline->base.shaders[MESA_SHADER_VERTEX],
                                          pipeline->base.shaders[MESA_SHADER_TESS_CTRL],
                                          pipeline->base.shaders[MESA_SHADER_TESS_EVAL],
                                          program, patch_control_points);
      unsigned size = pcp_cs.cur - pcp_cs.start;

      tu_cs_emit_pkt7(cs, CP_MEM_WRITE, size + 2);
      tu_cs_emit_qw(cs, *preprocess_iova);
      tu_cs_emit_array(cs, pcp_data, size);

      struct tu_draw_state pcp_state = {
         .iova = *preprocess_iova,
         .size = size,
      };
      data->patch_control_points = emit_draw_state(pcp_state);

      *preprocess_iova += size * 4;
      free(pcp_data);
   }
}

static struct tu_draw_state
setup_pipelines(struct tu_indirect_command_layout *layout,
                struct tu_cmd_buffer *cmd,
                struct tu_cmd_buffer *state_cmd,
                struct tu_cs *cs,
                uint64_t preprocess_iova,
                struct tu_pipeline **pipelines,
                uint32_t push_const_dwords,
                unsigned pipeline_count)
{
   struct tu_cs_memory mem;
   tu_cs_alloc(&cmd->sub_cs, TU_DGC_PIPELINE_SIZE * pipeline_count, 1, &mem);

   for (unsigned i = 0; i < pipeline_count; i++) {
      if (layout->dispatch) {
         setup_compute_pipeline(layout, cmd, cs, pipelines[i],
                                push_const_dwords,
                                (void *)&mem.map[TU_DGC_PIPELINE_SIZE * i],
                                &preprocess_iova);
      } else {
         setup_graphics_pipeline(layout, cmd, state_cmd, cs,
                                 tu_pipeline_to_graphics(pipelines[i]),
                                 push_const_dwords,
                                 (void *)&mem.map[TU_DGC_PIPELINE_SIZE * i],
                                 &preprocess_iova);
      }
   }

   return (struct tu_draw_state) {
      .iova = mem.iova,
      .size = TU_DGC_PIPELINE_SIZE * pipeline_count,
   };
}

static VkResult
get_preprocess_pipeline(struct tu_device *device,
                        VkPipeline *pipeline,
                        VkPipelineLayout *layout)
{
   const char *key = "preprocess";
   const unsigned key_length = strlen(key);

   VkDescriptorSetLayoutBinding bindings[] = {
      /* src_buffers */
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .descriptorCount = TU_DGC_MAX_BUFFERS,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      /* src_patchpoints */
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .descriptorCount = TU_DGC_MAX_BUFFERS,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      /* pipeline */
      {
         .binding = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };

   VkDescriptorSetLayoutCreateInfo dl_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = ARRAY_SIZE(bindings),
      .pBindings = bindings,
   };

   VkDescriptorSetLayout ds_layout;
   VkResult result = 
      vk_meta_create_descriptor_set_layout(
         &device->vk, &device->meta, &dl_info, key, key_length, &ds_layout);

   if (result != VK_SUCCESS)
      return result;

   VkPushConstantRange pc_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = sizeof(struct tu_dgc_args),
   };

   VkPipelineLayoutCreateInfo pl_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &ds_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pc_range,
   };

   result =
      vk_meta_create_pipeline_layout(
         &device->vk, &device->meta, &pl_info, key, key_length, layout);

   if (result != VK_SUCCESS)
      return result;

   VkPipeline pipeline_from_cache =
      vk_meta_lookup_pipeline(&device->meta, key, key_length);
   if (pipeline_from_cache != VK_NULL_HANDLE) {
      *pipeline = pipeline_from_cache;
      return VK_SUCCESS;
   }

   VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .codeSize = sizeof(preprocess_spv),
      .pCode = preprocess_spv,
   };

   VkPipelineShaderStageCreateInfo shader_stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &module_info,
      .flags = 0,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .pName = "main",
      .pSpecializationInfo = NULL,
   };

   VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .flags = 0,
      .stage = shader_stage,
      .layout = *layout,
   };

   return vk_meta_create_compute_pipeline(&device->vk, &device->meta,
                                          &pipeline_info, key, key_length,
                                          pipeline);
}

static void
write_buffer(struct tu_device *device,
             uint32_t *set_mem,
             struct tu_descriptor_set_layout *ds_layout,
             unsigned binding, unsigned descriptor,
             const struct tu_draw_state *mem,
             VkDescriptorType type)
{
   unsigned offset = ds_layout->binding[binding].offset / 4 +
      ds_layout->binding[binding].size * descriptor / 4;

   VkDescriptorAddressInfoEXT info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
      .address = mem->iova,
      .range = mem->size * 4,
   };

   VkDescriptorGetInfoEXT get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = type,
      .data = {
         .pStorageBuffer = &info,
      }
   };

   tu_GetDescriptorEXT(tu_device_to_handle(device),
                       &get_info, ds_layout->binding[binding].size,
                       &set_mem[offset]);
}

static void
write_ubo(struct tu_device *device,
          uint32_t *set_mem,
          struct tu_descriptor_set_layout *ds_layout,
          unsigned binding, unsigned descriptor,
          const struct tu_draw_state *mem)
{
   write_buffer(device, set_mem, ds_layout, binding, descriptor, mem,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
}

static void
write_ssbo(struct tu_device *device,
           uint32_t *set_mem,
           struct tu_descriptor_set_layout *ds_layout,
           unsigned binding, unsigned descriptor,
           const struct tu_draw_state *mem)
{
   write_buffer(device, set_mem, ds_layout, binding, descriptor, mem,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
}

template <chip CHIP>
void
tu_preprocess(struct tu_cmd_buffer *cmd,
              struct tu_cmd_buffer *state_cmd,
              const VkGeneratedCommandsInfoEXT *info)
{
   VK_FROM_HANDLE(tu_indirect_command_layout, layout,
                  info->indirectCommandsLayout);
   VK_FROM_HANDLE(tu_indirect_execution_set, iset, info->indirectExecutionSet);

   tu_iova_allow_dump(cmd->device, info->preprocessAddress,
                      info->preprocessSize);

   tu_iova_allow_dump(cmd->device, info->indirectAddress,
                      info->indirectAddressSize);

   struct tu_pipeline *pipeline;
   struct tu_pipeline **pipelines;
   unsigned pipeline_count = 1;
   uint32_t push_const_dwords = layout->push_constant_size;

   bool replace_const_patchpoints = false;
   struct tu_draw_state consts_patchpoints;

   if (iset) {
      pipelines = iset->pipelines;
      pipeline_count = iset->pipeline_count;

      if (layout->push_constant_size == 0) {
         /* We need to emit const state even if there aren't any push const
          * tokens, because we need to emit inline uniform state that depends
          * on the pipeline. In this case we have to figure out of the push
          * const size here at preprocess time and emit the template
          * dynamically, replacing the original template.
          */
         for (unsigned i = 0; i < iset->pipeline_count; i++) {
            struct tu_pipeline *pipeline = pipelines[i];

            push_const_dwords = MAX2(push_const_dwords,
                                     pipeline->program.shared_consts.lo +
                                     pipeline->program.shared_consts.dwords);
         }

         struct tu_dgc_cs cs;
         cs.idx = layout->user_consts_cs_idx;

         unsigned user_const_dwords =
            (push_const_dwords ? push_const_dwords + 1 : 0) +
            (layout->dispatch ? SHADER_INLINE_UBO_CMDS_SIZE :
             GRAPHICS_INLINE_UBO_CMDS_SIZE);

         tu_cs_draw_state(&cmd->sub_cs, &cs.cs, user_const_dwords);

         VkResult result = tu_cs_begin_sub_stream_aligned(
            &cmd->sub_cs,
            DIV_ROUND_UP(sizeof(struct tu_dgc_patchpoint) * TU_DGC_MAX_PATCHPOINTS, 64), 16,
            &cs.patchpoint_cs);
         if (result != VK_SUCCESS) {
            vk_command_buffer_set_error(&cmd->vk, result);
            return;
         }

         struct tu_dgc_builder builder = {};
         emit_user_consts(layout, &builder, push_const_dwords, &cs);

         consts_patchpoints =
            tu_cs_end_draw_state(&cmd->sub_cs, &cs.patchpoint_cs);

         replace_const_patchpoints = true;
      } else {
         push_const_dwords = layout->push_constant_size / 4;
      }
   } else {
      const VkGeneratedCommandsPipelineInfoEXT *pipeline_info =
         vk_find_struct_const(info->pNext,
                              GENERATED_COMMANDS_PIPELINE_INFO_EXT);

      VK_FROM_HANDLE(tu_pipeline, single_pipeline, pipeline_info->pipeline);
      pipeline = single_pipeline;
      pipelines = &pipeline;
      pipeline_count = 1;
      push_const_dwords = layout->push_constant_size / 4;
   }

   struct tu_cs *cs = cmd->state.subpass ? &cmd->draw_cs : &cmd->cs;

   struct tu_preprocess_layout preprocess_layout;
   alloc_preprocess_buffer(cmd->device, layout, pipelines, pipeline_count,
                           info->maxSequenceCount, &preprocess_layout);

   struct tu_draw_state pipeline_data =
      setup_pipelines(layout, cmd, state_cmd, cs,
                      info->preprocessAddress +
                      preprocess_layout.pipeline_offset,
                      pipelines, push_const_dwords, pipeline_count);

   VkPipeline preprocess_pipeline;
   VkPipelineLayout preprocess_pipeline_layout;

   VkResult result = get_preprocess_pipeline(cmd->device,
                                             &preprocess_pipeline,
                                             &preprocess_pipeline_layout);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }

   struct tu_cs user_consts_cs;
   result = tu_cs_begin_sub_stream_aligned(&cmd->sub_cs,
      DIV_ROUND_UP(user_consts_size(layout, push_const_dwords), 16), 16,
      &user_consts_cs);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }

   if (layout->emit_push_constants)
      emit_user_consts_template(state_cmd ? state_cmd : cmd, layout,
                                push_const_dwords, &user_consts_cs);

   struct tu_draw_state user_consts = 
      tu_cs_end_draw_state(&cmd->sub_cs, &user_consts_cs);

   if (!replace_const_patchpoints) {
      assert(user_consts.size ==
             layout->buffers[layout->user_consts_cs_idx].size);
   }

   struct tu_cs vbo_cs, vbo_stride_cs;
   struct tu_draw_state vbo, vbo_stride;
   if (!layout->dispatch) {
      result =
         tu_cs_begin_sub_stream_aligned(&cmd->sub_cs,
                                        DIV_ROUND_UP(vertex_buffers_size(layout, cmd), 16), 16,
                                        &vbo_cs);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return;
      }

      if (layout->bind_vbo_mask)
         emit_vertex_buffers_template(cmd, layout, &vbo_cs);

      vbo = tu_cs_end_draw_state(&cmd->sub_cs, &vbo_cs);

      result =
         tu_cs_begin_sub_stream_aligned(&cmd->sub_cs,
                                        DIV_ROUND_UP(vertex_buffers_stride_size(layout, cmd), 16), 16,
                                        &vbo_stride_cs);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return;
      }

      if (layout->bind_vbo_mask)
         emit_vertex_buffers_stride_template(state_cmd, layout, &vbo_stride_cs);

      vbo_stride = tu_cs_end_draw_state(&cmd->sub_cs, &vbo_stride_cs);
   }

   uint32_t old_push_constants[MAX_PUSH_CONSTANTS_SIZE / 4];
   struct tu_shader *old_compute = cmd->state.shaders[MESA_SHADER_COMPUTE];
   memcpy(old_push_constants, cmd->push_constants, sizeof(cmd->push_constants));
   struct tu_descriptor_state old_descriptors =
      cmd->descriptors[VK_PIPELINE_BIND_POINT_COMPUTE];

   VK_FROM_HANDLE(tu_pipeline_layout, tu_pipeline_layout,
                  preprocess_pipeline_layout);
   struct tu_descriptor_set_layout *ds_layout =
      tu_pipeline_layout->set[0].layout;
   struct tu_cs_memory set_mem;
   result = tu_cs_alloc(&cmd->sub_cs,
                        ds_layout->size / (4 * A6XX_TEX_CONST_DWORDS),
                        A6XX_TEX_CONST_DWORDS, &set_mem);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }

   for (unsigned i = 0; i < layout->buffer_count; i++) {
      const struct tu_draw_state *template_cs = &layout->buffers[i];
      const struct tu_draw_state *patchpoints_cs = &layout->patchpoints[i];
      if (i == layout->user_consts_cs_idx) {
         template_cs = &user_consts;
         if (replace_const_patchpoints)
            patchpoints_cs = &consts_patchpoints;
      } else if (i == layout->vertex_buffer_idx) {
         template_cs = &vbo;
      } else if (i == layout->vertex_buffer_stride_idx) {
         template_cs = &vbo_stride;
      }
      /* src_buffers */
      write_ubo(cmd->device, set_mem.map, ds_layout, 0, i, template_cs);
      /* src_patchpoints */
      write_ubo(cmd->device, set_mem.map, ds_layout, 1, i, patchpoints_cs);
   }

   write_ssbo(cmd->device, set_mem.map, ds_layout, 2, 0, &pipeline_data);

   struct tu_descriptor_set push_set = {};
   push_set.base.type = VK_OBJECT_TYPE_DESCRIPTOR_SET;
   push_set.layout = ds_layout;
   push_set.size = ds_layout->size;
   push_set.va = set_mem.iova;

   const VkDescriptorSet desc_set[] = { tu_descriptor_set_to_handle(&push_set) };
   vk_common_CmdBindDescriptorSets(tu_cmd_buffer_to_handle(cmd),
                                   VK_PIPELINE_BIND_POINT_COMPUTE,
                                   preprocess_pipeline_layout, 0, 1, desc_set,
                                   0, NULL);

   if (cmd->state.predication_active) {
      tu_cs_emit_pkt7(cs, CP_DRAW_PRED_ENABLE_LOCAL, 1);
      tu_cs_emit(cs, 0);
   }

   tu_CmdBindPipeline(tu_cmd_buffer_to_handle(cmd),
                      VK_PIPELINE_BIND_POINT_COMPUTE, preprocess_pipeline);

   VkDeviceAddress trampoline = info->preprocessAddress +
      preprocess_layout.trampoline_offset;
   VkDeviceAddress main_cs = info->preprocessAddress +
      preprocess_layout.buffers_offset[layout->main_cs_idx];

   for (unsigned i = 0; i < preprocess_layout.max_ibs; i++) {
      /* setup trampoline */
      tu_cs_emit_pkt7(cs, CP_MEM_WRITE, 5);
      tu_cs_emit_qw(cs, trampoline);
      /* This packet is written to the trampoline, i.e. here it's part of the
       * CP_MEM_WRITE packet! I heard you like packets...
       *
       * Note: We can't use tu_cs_emit_pkt7() here because we don't want to
       * reserve an extra dword for the missing size and potentially switch to
       * a new IB, splitting the CP_MEM_WRITE packet.
       */
      tu_cs_emit(cs, pm4_pkt7_hdr(CP_INDIRECT_BUFFER_CHAIN, 3));
      tu_cs_emit_qw(cs, main_cs);
      /* The size is filled by the compute shader */

      uint32_t ib_sequence_offset = i * preprocess_layout.sequences_per_ib;

      struct tu_dgc_args args = {
         .sequence_count_addr = info->sequenceCountAddress,
         .trampoline_addr = trampoline,
         .src_indirect_addr = info->indirectAddress,
         .src_indirect_stride = layout->input_stride,
         .max_sequence_count = info->maxSequenceCount,
         .max_draw_count = info->maxDrawCount,
         .ib_sequence_offset = ib_sequence_offset,
         .sequences_per_ib = preprocess_layout.sequences_per_ib,
         .src_pipeline_offset =
            layout->bind_pipeline ? layout->pipeline_offset / 4 : ~0,
         .buffer_count = layout->buffer_count,
         .main_buffer = layout->main_cs_idx,
      };

      for (unsigned j = 0; j < layout->buffer_count; j++) {
         const struct tu_draw_state *template_cs = &layout->buffers[j];
         if (j == layout->user_consts_cs_idx)
            template_cs = &user_consts;
         else if (j == layout->vertex_buffer_idx)
            template_cs = &vbo;
         else if (j == layout->vertex_buffer_stride_idx)
            template_cs = &vbo_stride;
         args.buffer_stride[j] = template_cs->size;
         if (j == layout->main_cs_idx) {
            args.dst_buffer_addr[j] = main_cs;
         } else {
            args.dst_buffer_addr[j] =
               info->preprocessAddress + preprocess_layout.buffers_offset[j] +
               template_cs->size * ib_sequence_offset * sizeof(uint32_t);
         }
         args.patchpoint_count[j] =
            layout->patchpoints[j].size / (sizeof(struct tu_dgc_patchpoint) / 4);
      }

      vk_common_CmdPushConstants(tu_cmd_buffer_to_handle(cmd),
                                 preprocess_pipeline_layout,
                                 VK_SHADER_STAGE_COMPUTE_BIT,
                                 0, sizeof(args), &args);

      uint32_t invocations = MIN2(preprocess_layout.sequences_per_ib,
                                  info->maxSequenceCount - ib_sequence_offset);
      tu_CmdDispatchBase<CHIP>(
         tu_cmd_buffer_to_handle(cmd), 0, 0, 0,
         MAX2(DIV_ROUND_UP(invocations, 128), 1), 1, 1);

      /* Each IB is followed by a trampoline jumping to the next IB. */
      trampoline = main_cs + sizeof(uint32_t) *
         layout->buffers[layout->main_cs_idx].size *
         preprocess_layout.sequences_per_ib;
      main_cs = trampoline + 4 * sizeof(uint32_t);
   }

   if (cmd->state.predication_active) {
      tu_cs_emit_pkt7(cs, CP_DRAW_PRED_ENABLE_LOCAL, 1);
      tu_cs_emit(cs, 1);
   }

   memcpy(cmd->push_constants, old_push_constants, sizeof(cmd->push_constants));
   cmd->state.shaders[MESA_SHADER_COMPUTE] = old_compute;
   if (old_compute) {
      tu_cs_emit_state_ib(&cmd->cs, old_compute->state);
   }
   cmd->descriptors[VK_PIPELINE_BIND_POINT_COMPUTE] = old_descriptors;
   cmd->state.dirty |=
      TU_CMD_DIRTY_SHADER_CONSTS |
      TU_CMD_DIRTY_COMPUTE_DESC_SETS;
}
TU_GENX(tu_preprocess);

template <chip CHIP>
VKAPI_ATTR void VKAPI_CALL
tu_CmdPreprocessGeneratedCommandsEXT(VkCommandBuffer commandBuffer,
                                     const VkGeneratedCommandsInfoEXT *pGeneratedCommandsInfo,
                                     VkCommandBuffer stateCommandBuffer)
{
   VK_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(tu_cmd_buffer, state_cmd, stateCommandBuffer);

   tu_preprocess<CHIP>(cmd, state_cmd, pGeneratedCommandsInfo);
}
TU_GENX(tu_CmdPreprocessGeneratedCommandsEXT);

