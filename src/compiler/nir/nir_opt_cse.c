/*
 * Copyright © 2014 Intel Corporation
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

#include "nir_instr_set.h"

/*
 * Implements common subexpression elimination
 */

static bool
dominates(const nir_instr *old_instr, const nir_instr *new_instr)
{
   return nir_block_dominates(old_instr->block, new_instr->block);
}

static nir_block *
block_dom_tree_next(nir_block *block)
{
   if (block->num_dom_children)
      return block->dom_children[0];

   while (true) {
      nir_block *parent = block->imm_dom;
      if (!parent)
         return NULL;

      assert(parent->num_dom_children > 0);
      unsigned index = 0;
      for (; parent->dom_children[index] != block; index++)
         assert(index + 1 != parent->num_dom_children);

      if (index + 1 == parent->num_dom_children)
         block = parent;
      else
         return parent->dom_children[index + 1];
   }
}

static bool
nir_opt_cse_impl(nir_function_impl *impl)
{
   struct set *instr_set = nir_instr_set_create(NULL);

   _mesa_set_resize(instr_set, impl->ssa_alloc);

   nir_metadata_require(impl, nir_metadata_dominance);

   bool progress = false;
   for (nir_block *block = nir_start_block(impl); block;) {
      nir_block *loop_header = NULL;

      nir_foreach_instr_safe(instr, block) {
         nir_instr *match = nir_instr_set_add(instr_set, instr, dominates);
         if (match) {
            nir_def *def = nir_instr_def(instr);
            nir_def *new_def = nir_instr_def(match);

            /* Rewrite the uses */
            nir_foreach_use_including_if_safe(use_src, def) {
               if (!nir_src_is_if(use_src)) {
                  nir_instr *phi = nir_src_parent_instr(use_src);
                  if (phi->type == nir_instr_type_phi && nir_block_dominates(phi->block, block)) {
                     /* This is a loop header phi that we have already visited. */
                     nir_instr_set_remove(instr_set, phi);
                     nir_src_rewrite(use_src, new_def);

                     /* Revisit the block if there's a CSE opportunity. */
                     if ((!loop_header || nir_block_dominates(phi->block, loop_header)) &&
                         nir_instr_set_add(instr_set, phi, dominates))
                        loop_header = phi->block;

                     continue;
                  }
               }

               nir_src_rewrite(use_src, new_def);
            }

            progress = true;
            nir_instr_remove(instr);
         }
      }

      if (loop_header) {
         for (nir_block *revisit = loop_header; ; revisit = block_dom_tree_next(revisit)) {
            /* Remove entries before we invalidate them by modifying their sources. */
            nir_foreach_instr(instr, revisit)
               nir_instr_set_remove(instr_set, instr);
            if (revisit == block)
               break;
         }
         block = loop_header;
      } else {
         block = block_dom_tree_next(block);
      }
   }

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_control_flow);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   nir_instr_set_destroy(instr_set);
   return progress;
}

bool
nir_opt_cse(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader) {
      progress |= nir_opt_cse_impl(impl);
   }

   return progress;
}
