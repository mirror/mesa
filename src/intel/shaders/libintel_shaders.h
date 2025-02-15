/* Copyright © 2023 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#ifndef _LIBANV_SHADERS_H_
#define _LIBANV_SHADERS_H_

/* Define stdint types compatible between the CPU and GPU for shared headers */
#ifndef __OPENCL_VERSION__
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#include "util/macros.h"

#include "compiler/intel_shader_enums.h"

#else
#include "libcl_vk.h"

#define util_last_bit(val) clz(val)

#define PRAGMA_POISON(param)

#include "genxml/gen_macros.h"
#include "genxml/genX_cl_pack.h"
#include "genxml/genX_rt_cl_pack.h"

#include "compiler/intel_shader_enums.h"

#define _3DPRIM_PATCHLIST(n) (0x20 + (n - 1))
#endif

#define ANV_GENERATED_MAX_VES (29)

/**
 * Flags for generated_draws.cl
 */
enum anv_generated_draw_flags {
   ANV_GENERATED_FLAG_INDEXED        = BITFIELD_BIT(0),
   ANV_GENERATED_FLAG_PREDICATED     = BITFIELD_BIT(1),
   /* Only used on Gfx9, means the pipeline is using gl_DrawID */
   ANV_GENERATED_FLAG_DRAWID         = BITFIELD_BIT(2),
   /* Only used on Gfx9, means the pipeline is using gl_BaseVertex or
    * gl_BaseInstance
    */
   ANV_GENERATED_FLAG_BASE           = BITFIELD_BIT(3),
   /* Whether the count is indirect  */
   ANV_GENERATED_FLAG_COUNT          = BITFIELD_BIT(4),
   /* Whether the generation shader writes to the ring buffer */
   ANV_GENERATED_FLAG_RING_MODE      = BITFIELD_BIT(5),
   /* Whether TBIMR tile-based rendering shall be enabled. */
   ANV_GENERATED_FLAG_TBIMR          = BITFIELD_BIT(6),
   /* Wa_16011107343 */
   ANV_GENERATED_FLAG_WA_16011107343 = BITFIELD_BIT(7),
   /* Wa_22018402687 */
   ANV_GENERATED_FLAG_WA_22018402687 = BITFIELD_BIT(8),
   /* Wa_16014912113 */
   ANV_GENERATED_FLAG_WA_16014912113 = BITFIELD_BIT(9),
   /* Wa_18022330953 / Wa_22011440098 */
   ANV_GENERATED_FLAG_WA_18022330953 = BITFIELD_BIT(10)
};

/**
 * Flags for query_copy.cl
 */
#define ANV_COPY_QUERY_FLAG_RESULT64  BITFIELD_BIT(0)
#define ANV_COPY_QUERY_FLAG_AVAILABLE BITFIELD_BIT(1)
#define ANV_COPY_QUERY_FLAG_DELTA     BITFIELD_BIT(2)
#define ANV_COPY_QUERY_FLAG_PARTIAL   BITFIELD_BIT(3)

/**
 * Structures for generate_commands.cl
 */
enum anv_gen_command_stage {
   ANV_GENERATED_COMMAND_STAGE_VERTEX = 0,
   ANV_GENERATED_COMMAND_STAGE_TESS_CTRL,
   ANV_GENERATED_COMMAND_STAGE_TESS_EVAL,
   ANV_GENERATED_COMMAND_STAGE_GEOMETRY,
   ANV_GENERATED_COMMAND_STAGE_FRAGMENT,
   ANV_GENERATED_COMMAND_STAGE_TASK,
   ANV_GENERATED_COMMAND_STAGE_MESH,

   ANV_GENERATED_COMMAND_STAGE_COMPUTE,
   ANV_GENERATED_COMMAND_STAGE_RT,

   ANV_GENERATED_COMMAND_STAGES,
};

#define ANV_GENERATED_COMMAND_N_GFX_STAGES (ANV_GENERATED_COMMAND_STAGE_MESH + 1)

enum anv_gen_gfx_draw_type {
   ANV_GEN_GFX_DRAW,
   ANV_GEN_GFX_DRAW_INDEXED,
   ANV_GEN_GFX_DRAW_MESH,
};

/* Keep in sync with MAX_PUSH_CONSTANTS_SIZE & struct anv_driver_constants */
#define ANV_GENERATED_COMMAND_RT_GLOBAL_DISPATCH_SIZE (128)
#define ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE (256)
#define ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_SIZE   (200)
#define ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_WS_SIZE_OFFSET ( \
      ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE + 156)
#define ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_FS_MSAA_FLAGS_OFFSET ( \
      ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE + 144)
#define ANV_GENERATED_COMMAND_DRIVER_CONSTANTS_PCP_OFFSET ( \
      ANV_GENERATED_COMMAND_MAX_PUSH_CONSTANTS_SIZE + 148)

#define ANV_DYNAMIC_VISIBLE_HEAP_OFFSET (1024 * 1024 * 1024)

enum anv_gen_push_constant_flags {
   ANV_GEN_PUSH_CONSTANTS_CMD_ACTIVE   = BITFIELD_BIT(0),
   ANV_GEN_PUSH_CONSTANTS_DATA_ACTIVE  = BITFIELD_BIT(1),
};

struct anv_gen_push_layout {
   struct anv_gen_push_entry {
      /* Location of the data to copy from the stream */
      uint32_t seq_offset;

      /* Location where to write the data in anv_push_constants */
      uint16_t push_offset;

      /* Size of the data to copy */
      uint16_t size;
   } entries[32];

   uint8_t flags; /* enum anv_gen_push_constant_flags */

   uint8_t num_entries;
   uint8_t mocs;

   uint16_t seq_id_active;
   uint16_t seq_id_offset;

   uint16_t cmd_offset;
   uint16_t cmd_size;

   uint16_t data_offset;
};

struct anv_gen_gfx_layout {
   struct anv_gen_index_buffer {
      uint16_t cmd_offset;
      uint16_t cmd_size;
      uint16_t seq_offset; /* Offset of VkBindIndexBufferIndirectCommandEXT */
      uint16_t mocs;
      uint32_t u32_value;
      uint32_t u16_value;
      uint32_t u8_value;
   } index_buffer;

   struct {
      struct anv_gen_vertex_buffer {
         uint16_t seq_offset; /* Offset of VkBindVertexBufferIndirectCommandEXT */
         uint16_t binding;
      } buffers[31];
      uint16_t n_buffers;
      uint16_t mocs;
      uint16_t cmd_offset;
      uint16_t cmd_size;
   } vertex_buffers;

   struct anv_gen_push_layout push_constants;

   struct {
      uint16_t final_cmds_offset;
      uint16_t final_cmds_size;

      uint16_t partial_cmds_offset;
      uint16_t partial_cmds_size;

      uint16_t data_offset;
      uint16_t active;

      uint32_t seq_offset;
   } indirect_set;

   struct {
      uint16_t cmd_offset;
      uint16_t cmd_size;
      uint16_t draw_type; /* anv_gen_gfx_draw_type */
      uint16_t seq_offset; /* Offset of :
                            *    - VkDrawIndirectCommand
                            *    - VkDrawIndexedIndirectCommand
                            *    - VkDrawMeshTasksIndirectCommandEXT
                            */
   } draw;
};

struct anv_gen_cs_layout {
   struct anv_gen_push_layout push_constants;

   struct {
      uint32_t seq_offset;
      uint16_t data_offset;
      uint16_t active;
   } indirect_set;

   /* Offset of VkDispatchIndirectCommand */
   struct {
      uint32_t seq_offset;
      uint16_t cmd_offset;
      uint16_t pad;
   } dispatch;
};

enum anv_gen_push_slot_type {
   ANV_GEN_PUSH_SLOT_TYPE_PUSH_CONSTANTS,
   ANV_GEN_PUSH_SLOT_TYPE_OTHER,
};

struct anv_gen_gfx_state {
   struct anv_gen_gfx_layout layout;

   /* Location of commands in the preprocess buffer */
   struct {
      uint32_t vfg[4];
      uint32_t so[5];
      uint32_t sf[4];
      uint32_t raster[5];
      uint32_t ps_blend[2];
   } indirect_set;

   struct {
      uint64_t addresses[4];
   } push_constants;

   /* Dynamic state values */
   struct {
      uint32_t primitive_topology; /* HW value */
      VkTessellationDomainOrigin domain_origin;
      VkPolygonMode polygon_mode;
      VkLineRasterizationMode line_mode;
      VkProvokingVertexModeEXT provoking_vertex;
      uint32_t line_api_mode;
      bool line_msaa_raster_enable;
      bool line_stipple_enable;
      bool has_uint_rt;
      bool alpha_to_coverage;
      uint32_t samples;
      uint32_t patch_control_points;
      uint32_t n_occlusion_queries;
      uint32_t color_write_enables;
      bool has_feedback_loop;
      bool coarse_pixel_enabled;

      bool depth_clip_negative_one_to_one;

      uint32_t triangle_strip_list_provoking_vertex;
      uint32_t line_strip_list_provoking_vertex;
      uint32_t triangle_fan_provoking_vertex;

      uint32_t max_vp_index;
   } dyn;

   struct {
      uint16_t instance_multiplier;
      uint32_t flags; /* ANV_GENERATED_FLAG_* */
   } draw;
};

struct anv_gen_gfx_indirect_descriptor {
   /* Fully packed instructions ready to be copied directly into the
    * preprocess buffer
    */
   uint32_t final_commands[98];
   uint32_t final_commands_size;

   struct {
      uint32_t urb[3 * 4];
      uint32_t urb_wa_16014912113[3 * 4 + 6];
   } final;

   /* These instructions need to be merged with additional dynamic
    * parameters
    */
   struct {
      uint32_t vfg[4];
      uint32_t gs[10];
      uint32_t te[5];
      uint32_t so[5];
      uint32_t clip[4];
      uint32_t sf[4];
      uint32_t wm[2];
      uint32_t ps[12];
      uint32_t ps_msaa[12];
      uint32_t ps_extra[2];
   } partial;

   /* Some pipeline specific bits of information */
   uint32_t active_stages;
   uint32_t ds_urb_cfg;
   uint32_t tes_output_topology;
   uint32_t color_writes;
   VkPolygonMode last_preraster_topology;

   enum intel_barycentric_mode barycentric_interp_modes;
   enum intel_sometimes persample_dispatch;
   enum intel_sometimes coarse_pixel_dispatch;
   bool has_side_effects;
   bool sample_shading;
   bool uses_kill;
   bool rp_has_ds_self_dep;
   float min_sample_shading;
   bool sample_shading_enable;

   struct {
      struct anv_gen_push_stage_state {
         struct anv_gen_push_stage_slot {
            uint16_t push_data_offset;
            uint16_t push_data_size;
            uint32_t type; /* enum anv_gen_push_slot_type */
         } slots[4];
         uint32_t n_slots;
      } stages[ANV_GENERATED_COMMAND_N_GFX_STAGES];

      uint16_t active_stages; /* Bitfield of anv_gen_command_stage */
   } push_constants;
};

struct anv_gen_cs_indirect_descriptor {
   union {
      struct {
         uint32_t compute_walker[39];
      } gfx125;

      struct {
         /* Needs to be the first field because
          * MEDIA_INTERFACE_DESCRIPTOR_LOAD::InterfaceDescriptorDataStartAddress
          * needs 64B alignment.
          */
         uint32_t interface_descriptor_data[8];
         uint32_t gpgpu_walker[15];
         uint32_t media_vfe_state[9];

         uint32_t n_threads;
         uint16_t cross_thread_push_size;
         uint8_t per_thread_push_size;
         uint8_t subgroup_id_offset;
      } gfx9;
   };

   uint32_t push_data_offset;

   /* Align the struct to 64B */
   uint32_t pad[8];
};

struct anv_gen_rt_indirect_descriptor {
   uint32_t ray_stack_stride;
   uint32_t stack_ids_per_dss;
   uint32_t sw_stack_size;

   uint64_t call_handler;

   uint64_t hit_sbt;
   uint64_t miss_sbt;
   uint64_t callable_sbt;
};

#ifdef __OPENCL_VERSION__

void genX(write_address)(global void *dst_ptr,
                         global void *address, uint64_t value);

void genX(write_3DSTATE_VERTEX_BUFFERS)(global void *dst_ptr,
                                        uint32_t buffer_count);

void genX(write_VERTEX_BUFFER_STATE)(global void *dst_ptr,
                                     uint32_t mocs,
                                     uint32_t buffer_idx,
                                     uint64_t address,
                                     uint32_t size,
                                     uint32_t stride);

void genX(write_3DSTATE_INDEX_BUFFER)(global void *dst_ptr,
                                      uint64_t buffer_addr,
                                      uint32_t buffer_size,
                                      uint32_t index_format,
                                      uint32_t mocs);

void genX(write_3DSTATE_VF_TOPOLOGY)(global void *dst_ptr,
                                     uint32_t topology);

void genX(write_3DPRIMITIVE)(global void *dst_ptr,
                             bool is_predicated,
                             bool is_indexed,
                             bool use_tbimr,
                             uint32_t vertex_count_per_instance,
                             uint32_t start_vertex_location,
                             uint32_t instance_count,
                             uint32_t start_instance_location,
                             uint32_t base_vertex_location);

#if GFX_VER >= 11
void genX(write_3DPRIMITIVE_EXTENDED)(global void *dst_ptr,
                                      bool is_predicated,
                                      bool is_indexed,
                                      bool use_tbimr,
                                      uint32_t vertex_count_per_instance,
                                      uint32_t start_vertex_location,
                                      uint32_t instance_count,
                                      uint32_t start_instance_location,
                                      uint32_t base_vertex_location,
                                      uint32_t param_base_vertex,
                                      uint32_t param_base_instance,
                                      uint32_t param_draw_id);
#endif

#if GFX_VERx10 >= 125
void genX(write_3DMESH_3D)(global uint32_t *dst_ptr,
                           global void *indirect_ptr,
                           bool is_predicated,
                           bool uses_tbimr);
#endif

void genX(write_MI_BATCH_BUFFER_START)(global void *dst_ptr, uint64_t addr);

void genX(write_draw)(global uint32_t *dst_ptr,
                      global void *indirect_ptr,
                      global uint32_t *draw_id_ptr,
                      uint32_t draw_id,
                      uint32_t instance_multiplier,
                      bool is_indexed,
                      bool is_predicated,
                      bool uses_tbimr,
                      bool uses_base,
                      bool uses_draw_id,
                      uint32_t mocs);


void genX(copy_data)(global void *dst_ptr,
                     global void *src_ptr,
                     uint32_t size);

void genX(set_data)(global void *dst_ptr,
                    uint32_t data,
                    uint32_t size);

#endif /* __OPENCL_VERSION__ */

#endif /* _LIBANV_SHADERS_H_ */
