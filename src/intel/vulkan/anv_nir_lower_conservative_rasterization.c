/*
 * Copyright © 2021 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "anv_nir.h"
#include "nir_builder.h"

struct lower_crast_instr_key {
   const struct brw_wm_prog_key *wm;
   const struct intel_device_info *devinfo;
};

static nir_def *
build_fully_covered(nir_builder *b, const struct lower_crast_instr_key *key)
{
   assert(b->shader->info.fs.inner_coverage);

   /* TODO: Switch this over to the WA framework */
   if (key->devinfo->ver > 11)
      return nir_imm_true(b);

   const unsigned sample_mask = key->wm->conservative_sample_mask;

   /* We use SAMPLE_MASK_IN for both sample_mask_in and coverage_mask */
   BITSET_SET(b->shader->info.system_values_read, SYSTEM_VALUE_SAMPLE_MASK_IN);

   /* From the ICL PRM, Wa_220856683:
    *
    *    "Starting in CNL, while designing CPS and depth coverage mode for
    *    input coverage for conservative rasterization implementation changed.
    *    Especially input coverage mode = INNER started ANDing sample mask to
    *    conservative rast mask. This results in the mis-match wrt to the
    *    spec. WA for ICL is to have PS compiler logically OR input coverage
    *    mask to infer if a pixel is fully covered when
    *    INPUT_COVERAGE_MASK_MODE = INNER"
    *
    * To deal with this, we can either OR the coverage mask with the inverse
    * of the sample mask or we can always AND with the sample mask and then
    * compare to the sample mask.  We choose the later as it seems a bit more
    * obvious.
    */
   return nir_ieq(b, nir_iand_imm(b, nir_load_coverage_mask(b),
                                     sample_mask),
                     nir_imm_int(b, sample_mask));
}

static bool
lower_crast_instr(nir_builder *b, nir_instr *instr, void *_key)
{
   const struct lower_crast_instr_key *key = _key;
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   b->cursor = nir_before_instr(instr);
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_sample_mask_in:
      /* Vulkan doesn't have a concept of "inner coverage".  However, we
       * require inner coverage for our lowering to work properly in some
       * cases.  When that happens, we have to fake all-or-nothing coverage by
       * lowering it here.
       *
       * Since we know we're always doing conservative rasterization if we've
       * gotten here, we can always lower and maybe save ourselves a few
       * shader instructions because these expressions area always simpler
       * than the ones we use without conservative rasterization.
       */
      if (key->wm->persample_interp) {
         nir_def_rewrite_uses(&intrin->def,
                              nir_ishl(b, nir_imm_int(b, 1),
                              nir_load_sample_id(b)));
      } else {
         const unsigned sample_mask = key->wm->conservative_sample_mask;
         nir_def_rewrite_uses(&intrin->def, nir_imm_int(b, sample_mask));
      }
      return true;

   case nir_intrinsic_load_fully_covered:
      switch (key->wm->vk_conservative) {
      case VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT:
         nir_def_rewrite_uses(&intrin->def, build_fully_covered(b, key));
         return true;

      case VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT:
         /* If we're underestimating then we can only ever get here if all
          * samples are covered.
          */
         nir_def_rewrite_uses(&intrin->def, nir_imm_true(b));
         return true;

      case VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT:
      default:
         unreachable("Unsupported conservative rasterization mode");
      }

   default:
      return false;
   }
}

bool
anv_nir_lower_conservative_rasterization(nir_shader *nir,
                                         const struct brw_wm_prog_key *key,
                                         const struct intel_device_info *devinfo)
{
   assert(nir->info.stage == MESA_SHADER_FRAGMENT);

   /* Vulkan doesn't have a concept of inner coverage. */
   assert(!nir->info.fs.inner_coverage);

   const struct lower_crast_instr_key _key = {
      .wm = key,
      .devinfo = devinfo,
   };

   bool shader_progress = false;
   switch (key->vk_conservative) {
   case VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT:
      nir_shader_preserve_all_metadata(nir);
      return false;

   case VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT:
      if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FULLY_COVERED))
         nir->info.fs.inner_coverage = true;
      break;

   case VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT: {
      nir_builder b = nir_builder_create(nir_shader_get_entrypoint(nir));

      nir->info.fs.inner_coverage = true;
      nir->info.fs.uses_discard = true;
      b.cursor = nir_before_cf_list(&b.impl->body);
      nir_terminate_if(&b, nir_inot(&b, build_fully_covered(&b, &_key)));
      shader_progress = true;
      break;
   }

   default:
      unreachable("Invalid Vulkan conservative rasterization mode");
   }

   return nir_shader_instructions_pass(nir, lower_crast_instr,
                                       nir_metadata_none, (void *)&_key) ||
          shader_progress;
}
