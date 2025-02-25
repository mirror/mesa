/*
 * Copyright (c) 2024 Erico Nunes
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "ppir.h"

static bool ppir_copy_prop_mov(ppir_block *block, ppir_node *node)
{
   if (node->op != ppir_op_mov)
      return false;

   if (ppir_node_is_root(node))
      return false;

   if (node->succ_different_block)
      return false;

   ppir_dest *dest = ppir_node_get_dest(node);
   if (dest->type != ppir_target_ssa)
      return false;

   if (dest->modifier != ppir_outmod_none)
      return false;

   ppir_src *mov_src = ppir_node_get_src(node, 0);

   if (mov_src->type == ppir_target_pipeline)
      return false;

   ppir_node_foreach_succ_safe(node, dep) {
      ppir_node *succ = dep->succ;
      assert(succ);

      if (succ->type != ppir_node_type_alu)
         return false;
   }

   ppir_node_foreach_succ_safe(node, dep) {
      ppir_node *succ = dep->succ;
      assert(succ && succ->type == ppir_node_type_alu);

      for (int i = 0; i < ppir_node_get_src_num(succ); i++) {
         ppir_src *src = ppir_node_get_src(succ, i);
         assert(src);

         if (src->node != node)
            continue;

         uint8_t swizzle[4];
         for (int j = 0; j < 4; j++)
            swizzle[j] = mov_src->swizzle[src->swizzle[j]];

         /* Both src or mod_src may already carry folded modifiers.
          * Account for those by saving src modifiers and applying
          * them again afterwards. */
         bool neg = src->negate;
         bool abs = src->absolute;

         *src = *mov_src;

         if (neg)
            src->negate = !src->negate;
         if (abs)
            src->absolute = true;

         memcpy(src->swizzle, swizzle, sizeof(swizzle));
      }

      /* insert the succ alu node as successor of the mod src node */
      ppir_node_foreach_pred_safe(node, dep) {
         ppir_node *pred = dep->pred;
         ppir_node_add_dep(succ, pred, ppir_dep_src);
      }
   }

   ppir_node_delete(node);
   return true;
}

bool ppir_copy_prop(ppir_compiler *comp)
{
   bool progress = false;
   list_for_each_entry(ppir_block, block, &comp->block_list, list)
      list_for_each_entry_safe(ppir_node, node, &block->node_list, list)
         progress |= ppir_copy_prop_mov(block, node);

   return progress;
}
