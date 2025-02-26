/* -*- mesa-c++  -*-
 * Copyright 2019 Collabora LTD
 * Author: Gert Wollny <gert.wollny@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#include "r600_sfn.h"

#include "compiler/nir/nir.h"
#include "compiler/shader_enums.h"
#include "sfn/sfn_assembler.h"
#include "sfn/sfn_debug.h"
#include "sfn/sfn_memorypool.h"
#include "sfn/sfn_nir.h"
#include "sfn/sfn_shader.h"
#include "r600_asm.h"
#include "r600_pipe.h"
#include "util/macros.h"
#include "util/ralloc.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

char *
r600_finalize_nir(pipe_screen *screen, struct nir_shader *nir)
{
   auto rs = container_of(screen, r600_screen, b.b);
   r600_finalize_nir_common(nir, rs->b.gfx_level);
   return nullptr;
}

class MallocPoolRelease {
public:
   MallocPoolRelease() { r600::init_pool(); }
   ~MallocPoolRelease() { r600::release_pool(); }
};

static void
r600_nir_emit_polygon_stipple_ubo(nir_shader *nir,
				  const unsigned ubo_index,
				  const unsigned stride,
				  const unsigned offset_base,
				  const bool use_ubfe)
{
	nir_function_impl *impl = nir_shader_get_entrypoint(nir);
	nir_builder builder = nir_builder_at(nir_before_impl(impl));
	nir_builder *b = &builder;
	nir_def *zero = nir_imm_int(b, 0);

	assert(nir->info.stage == MESA_SHADER_FRAGMENT);
	assert(stride >= sizeof(unsigned) && util_is_power_of_two_nonzero(stride));

	nir_variable *pos_var = nir_variable_create(nir, nir_var_shader_in,
						    glsl_vec4_type(), "gl_FragCoord");
	pos_var->data.location = VARYING_SLOT_POS;
	pos_var->data.interpolation = INTERP_MODE_NONE;

	nir_def *pos = nir_load_var(b, pos_var);
	nir_def *pos_x = nir_f2u32(b, nir_channel(b, pos, 0));
	nir_def *pos_y = nir_f2u32(b, nir_channel(b, pos, 1));
	nir_def *thirtyone = nir_imm_int(b, 31);
	nir_def *mod_y = nir_iand(b, pos_y, thirtyone);

	const unsigned polygon_stipple_offset = stride * offset_base;
	nir_def *offset = nir_ishl_imm(b, mod_y, util_logbase2(stride));
	nir_def *row = nir_load_ubo(b, 1, 32,
				    nir_imm_int(b, ubo_index),
				    nir_iadd_imm(b, offset, polygon_stipple_offset),
				    .align_mul = stride,
				    .align_offset = 0,
				    .range_base = polygon_stipple_offset,
				    .range = R600_POLYGON_STIPPLE_SIZE * stride);

	nir_def *bit = use_ubfe ?
		nir_ubfe(b, row, pos_x, nir_imm_int(b, 1)) :
		nir_iand(b, nir_ushr(b, row, pos_x), nir_imm_int(b, 1));

	nir_discard_if(b, nir_ieq(b, bit, zero));
}

int
r600_shader_from_nir(struct r600_context *rctx,
                     struct r600_pipe_shader *pipeshader,
                     r600_shader_key *key)
{

   MallocPoolRelease pool_release;
   bool force_fragcoord_input = false;

   struct r600_pipe_shader_selector *sel = pipeshader->selector;

   if (rctx->screen->b.debug_flags & DBG_PREOPT_IR) {
      fprintf(stderr, "PRE-OPT-NIR-----------.------------------------------\n");
      nir_print_shader(sel->nir, stderr);
      fprintf(stderr, "END PRE-OPT-NIR--------------------------------------\n\n");
   }

   auto sh = nir_shader_clone(sel->nir, sel->nir);

   if (unlikely(sel->nir->info.stage == MESA_SHADER_FRAGMENT &&
		key->ps.poly_stipple)) {
	   if (rctx->b.gfx_level >= EVERGREEN) {
		   NIR_PASS_V(sh, r600_nir_emit_polygon_stipple_ubo,
			      R600_POLY_STIPPLE_INFO_CONST_BUFFER,
			      sizeof(unsigned), 0, true);
		   force_fragcoord_input = true;
	   } else {
		   NIR_PASS_V(sh, r600_nir_emit_polygon_stipple_ubo,
			      R600_POLY_STIPPLE_INFO_CONST_BUFFER,
			      sizeof(unsigned), 0, false);
	   }
   }

   r600_lower_and_optimize_nir(sh, key, rctx->b.gfx_level, &sel->so);

   if (rctx->screen->b.debug_flags & DBG_ALL_SHADERS) {
      fprintf(stderr,
              "-- NIR --------------------------------------------------------\n");
      struct nir_function *func =
         (struct nir_function *)exec_list_get_head(&sh->functions);
      nir_index_ssa_defs(func->impl);
      nir_print_shader(sh, stderr);
      fprintf(stderr,
              "-- END --------------------------------------------------------\n");
   }

   memset(&pipeshader->shader, 0, sizeof(r600_shader));
   pipeshader->scratch_space_needed = sh->scratch_size;

   if (sh->info.stage == MESA_SHADER_TESS_EVAL || sh->info.stage == MESA_SHADER_VERTEX ||
       sh->info.stage == MESA_SHADER_GEOMETRY) {
      pipeshader->shader.clip_dist_write |=
         ((1 << sh->info.clip_distance_array_size) - 1);
      pipeshader->shader.cull_dist_write = ((1 << sh->info.cull_distance_array_size) - 1)
                                           << sh->info.clip_distance_array_size;
      pipeshader->shader.cc_dist_mask =
         (1 << (sh->info.cull_distance_array_size + sh->info.clip_distance_array_size)) -
         1;
   }
   struct r600_shader *gs_shader = nullptr;
   if (rctx->gs_shader)
      gs_shader = &rctx->gs_shader->current->shader;
   r600_screen *rscreen = rctx->screen;

   r600::Shader *shader =
      r600::Shader::translate_from_nir(sh, &sel->so, gs_shader, *key,
                                       rctx->isa->hw_class, rscreen->b.family);

   assert(shader);
   if (!shader)
      return -2;

   pipeshader->enabled_stream_buffers_mask = shader->enabled_stream_buffers_mask();
   pipeshader->selector->info.file_count[TGSI_FILE_HW_ATOMIC] +=
      shader->atomic_file_count();
   pipeshader->selector->info.writes_memory =
      shader->has_flag(r600::Shader::sh_writes_memory);

   r600_finalize_and_optimize_shader(shader);

   auto scheduled_shader = r600_schedule_shader(shader);
   if (!scheduled_shader) {
      return -1;
   }

   scheduled_shader->get_shader_info(&pipeshader->shader);
   pipeshader->shader.uses_doubles = sh->info.bit_sizes_float & 64 ? 1 : 0;

   r600_bytecode_init(&pipeshader->shader.bc,
                      rscreen->b.gfx_level,
                      rscreen->b.family,
                      rscreen->has_compressed_msaa_texturing);

   /* We already schedule the code with this in mind, no need to handle this
    * in the backend assembler */
   pipeshader->shader.bc.ar_handling = AR_HANDLE_NORMAL;
   pipeshader->shader.bc.r6xx_nop_after_rel_dst = 0;

   r600::sfn_log << r600::SfnLog::shader_info << "pipeshader->shader.processor_type = "
                 << pipeshader->shader.processor_type << "\n";

   pipeshader->shader.bc.type = pipeshader->shader.processor_type;
   pipeshader->shader.bc.isa = rctx->isa;
   pipeshader->shader.bc.ngpr = scheduled_shader->required_registers();

   r600::Assembler afs(&pipeshader->shader, *key);
   if (!afs.lower(scheduled_shader)) {
      R600_ERR("%s: Lowering to assembly failed\n", __func__);

      scheduled_shader->print(std::cerr);
      /* For now crash if the shader could not be generated */
      assert(0);
      return -1;
   }

   if (sh->info.stage == MESA_SHADER_VERTEX) {
      pipeshader->shader.vs_position_window_space =
            sh->info.vs.window_space_position;
   }

   if (sh->info.stage == MESA_SHADER_FRAGMENT) {
      pipeshader->shader.ps_conservative_z =
            sh->info.fs.depth_layout;

      if (unlikely(force_fragcoord_input)) {
	      unsigned k;

	      for (k = 0; k < pipeshader->shader.ninput; ++k) {
		      if(pipeshader->shader.input[k].system_value == SYSTEM_VALUE_MAX &&
			 pipeshader->shader.input[k].varying_slot == VARYING_SLOT_TEX0 &&
			 pipeshader->shader.input[k].spi_sid == VARYING_SLOT_TEX0 + 1 &&
			 pipeshader->shader.input[k].interpolate == TGSI_INTERPOLATE_PERSPECTIVE)
			      break;

		      assert(pipeshader->shader.input[k].spi_sid != VARYING_SLOT_TEX0 + 1);
	      }

	      if (pipeshader->shader.ninput == k) {
		      pipeshader->shader.input[k].system_value = SYSTEM_VALUE_MAX;
		      pipeshader->shader.input[k].varying_slot = VARYING_SLOT_TEX0;
		      pipeshader->shader.input[k].spi_sid = VARYING_SLOT_TEX0 + 1;
		      pipeshader->shader.input[k].interpolate = TGSI_INTERPOLATE_PERSPECTIVE;
		      pipeshader->shader.ninput++;
	      }
      }
   }

   if (sh->info.stage == MESA_SHADER_GEOMETRY) {
      r600::sfn_log << r600::SfnLog::shader_info
                    << "Geometry shader, create copy shader\n";
      generate_gs_copy_shader(rctx, pipeshader, &sel->so);
      assert(pipeshader->gs_copy_shader);
   } else {
      r600::sfn_log << r600::SfnLog::shader_info << "This is not a Geometry shader\n";
   }
   ralloc_free(sh);

   return 0;
}
