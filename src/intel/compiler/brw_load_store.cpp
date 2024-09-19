/*
 * Copyright © 2024 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */
#include "brw_shader.h"
#include "brw_cfg.h"
#include "brw_analysis.h"
#include "brw_builder.h"

/* Duplicated from brw_def_analysis.cpp. */
static bool
fully_defines(const brw_shader &s, brw_inst *inst)
{
   return s.alloc.sizes[inst->dst.nr] * REG_SIZE == inst->size_written &&
          !inst->is_partial_write();
}

static bool
insert_load_reg(brw_shader &s)
{
   bool progress = false;

   const brw_def_analysis &defs = s.def_analysis.require();

   foreach_block_and_inst_safe(block, brw_inst, inst, s.cfg) {
      if (inst->opcode == SHADER_OPCODE_LOAD_REG ||
          inst->opcode == SHADER_OPCODE_MEMORY_STORE_LOGICAL ||
          inst->opcode == SHADER_OPCODE_UNDEF ||
          inst->opcode == BRW_OPCODE_DPAS) {
         continue;
      }

      /* If the destination is already SSA, there is nothing that needs to be
       * done.
       */
      if (defs.get(inst->dst) != NULL)
         continue;

      /* If there is a source that would cause def_analysis::update_for_reads
       * to mark the def as invalid, adding load_reg instructions for the
       * sources will not help.
       */
      if (inst->reads_accumulator_implicitly())
         continue;

      bool bad_source = false;
      for (int i = 0; i < inst->sources; i++) {
         if (inst->src[i].file == ARF &&
             (inst->src[i].nr == BRW_ARF_ADDRESS ||
              inst->src[i].nr == BRW_ARF_ACCUMULATOR ||
              inst->src[i].nr == BRW_ARF_FLAG)) {
            bad_source = true;
            break;
         }
      }

      if (bad_source)
         continue;

      /* If the destination is non-VGRF or the instruction does not fully
       * define the destination, adding load_reg instructions will not help.
       */
      if (inst->dst.file != VGRF || !fully_defines(s, inst))
         continue;

      /* Replace any non-SSA sources with load_reg of the source. */
      const brw_builder bld = brw_builder(&s, block, inst);
      for (int i = 0; i < inst->sources; i++) {
         if (inst->src[i].file == VGRF && defs.get(inst->src[i]) == NULL) {
            /* The source is not SSA. */

            /* If the size of the VGRF allocation is not an even multiple of
             * the SIMD size, don't emit a load_reg. This can occur for sparse
             * texture loads. These will have SIMD-size values for the texture
             * data and a single SIMD1 register for the residency information.
             */
            const unsigned reg_size =
               s.alloc.sizes[inst->src[i].nr] * reg_unit(s.devinfo);

            /* Avoid division by zero below. */
            if (inst->exec_size < 8)
               continue;

            const unsigned granularity = inst->exec_size / 8;

            if (reg_size % granularity != 0)
               continue;

            if (inst->src[i].stride == 1) {
               brw_reg_type t =
                  brw_type_with_size(BRW_TYPE_UD,
                                     brw_type_size_bits(inst->src[i].type));
               brw_reg old_src = brw_vgrf(inst->src[i].nr, t);
               brw_reg new_src;

               foreach_inst_in_block_reverse_starting_from(brw_inst, scan_inst, inst) {
                  if (scan_inst->dst.file == old_src.file &&
                      scan_inst->dst.nr == old_src.nr) {
                     break;
                  }

                  if (scan_inst->opcode == SHADER_OPCODE_LOAD_REG &&
                      old_src.equals(scan_inst->src[0])) {
                     new_src = scan_inst->dst;
                     break;
                  }
               }

               if (new_src.file == BAD_FILE)
                  new_src = bld.LOAD_REG(old_src);

               inst->src[i].nr = new_src.nr;
               progress = true;
            }
         }
      }
   }

   if (progress)
      s.invalidate_analysis(BRW_DEPENDENCY_INSTRUCTIONS |
                            BRW_DEPENDENCY_VARIABLES);

   return progress;
}

bool
brw_insert_load_and_store_reg(brw_shader &s)
{
   bool progress = false;

   if (insert_load_reg(s))
      progress = true;

   if (progress)
      s.def_analysis.require();

   return progress;
}

bool
brw_lower_load_and_store_reg(brw_shader &s)
{
   bool progress = false;

   foreach_block_and_inst_safe(block, brw_inst, inst, s.cfg) {
      if (inst->opcode == SHADER_OPCODE_LOAD_REG) {
         const brw_builder ibld = brw_builder(&s, block, inst);

         const unsigned bytes = inst->size_written;
         const unsigned type_bytes = brw_type_size_bytes(inst->dst.type);
         const unsigned bytes_per_mov = inst->exec_size * type_bytes;

         for (unsigned i = 0; i < bytes; i += bytes_per_mov) {
            ibld.MOV(byte_offset(inst->dst, i),
                     byte_offset(inst->src[0], i));
         }

         inst->remove(block);
         progress = true;
      }
   }

   if (progress)
      s.invalidate_analysis(BRW_DEPENDENCY_INSTRUCTIONS |
                            BRW_DEPENDENCY_VARIABLES);

   return progress;
}
