/*
 * Copyright © 2022 Google LLC
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

#include "nir.h"
#include "nir_builder.h"
#include "nir_noltis.h"

/**
 * fmul+fadd -> ffma fuser.
 *
 * Takes a cost function for the given mul/add operands that the driver can return a nonnegative cost for.
 */

struct fuse_state {
   nir_alu_src src[3];
   bool ffmaz;
};

static int
simple_cost(nir_alu_src *mul0, nir_alu_src *mul1, nir_alu_src *add)
{
    return 1;
}

bool
nir_opt_fuse_ffma(nir_shader *shader, int (*ffma_cost)(nir_alu_src *mul0, nir_alu_src *mul1, nir_alu_src *add))
{
   if (!ffma_cost)
      ffma_cost = simple_cost;

   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   nir_builder b = nir_builder_create(impl);

   nir_noltis *noltis = nir_noltis_create(NULL, impl);

   static bool trying_ffma = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         /* Add a tile for "This optimization pass doesn't change this NIR
          * instr".  TODO: Should this have a non-1 cost possible?  Need to
          * experiment with a driver with an interesting cost function.
          */
         nir_noltis_tile_create_noop(noltis, instr, 1);

         /* Add tiles for this instr being the fadd of a possible ffma. */
         if (instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *fadd_alu = nir_instr_as_alu(instr);
         if (fadd_alu->op != nir_op_fadd || fadd_alu->exact)
            continue;

         for (int i = 0; i < 2; i++) {
            nir_instr *fmul_instr = fadd_alu->src[i].src.ssa->parent_instr;
            if (fmul_instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *fmul_alu = nir_instr_as_alu(fmul_instr);
            if (fmul_alu->op != nir_op_fmul && fmul_alu->op != nir_op_fmulz)
               continue;

            struct fuse_state *fuse = rzalloc(noltis, struct fuse_state);
            fuse->src[0] = fmul_alu->src[0];
            fuse->src[1] = fmul_alu->src[1];
            fuse->src[2] = fadd_alu->src[1 - i];
            fuse->ffmaz = fmul_alu->op == nir_op_fmulz;

            for (int c = 0; c < fadd_alu->def.num_components; c++) {
               int s = fadd_alu->src[i].swizzle[c];
               fuse->src[0].swizzle[c] = fmul_alu->src[0].swizzle[s];
               fuse->src[1].swizzle[c] = fmul_alu->src[1].swizzle[s];
            }
            /* Ask the driver if it could handle this fused ffma, and how expensive it is. */
            int cost = ffma_cost(&fuse->src[0], &fuse->src[1], &fuse->src[2]);
            if (cost >= 0) {
               nir_noltis_tile *tile = nir_noltis_tile_create(noltis, instr, fuse);
               tile->cost = cost;

               for (int i = 0; i < ARRAY_SIZE(fuse->src); i++)
                  nir_noltis_tile_add_edge(tile, fuse->src[i].src.ssa->parent_instr);
               nir_noltis_tile_add_interior(tile, &fmul_alu->instr);

               trying_ffma = true;
            }
         }
      }
   }

   if (!trying_ffma) {
      nir_metadata_preserve(impl, nir_metadata_all);
      ralloc_free(noltis);
      return false;
   }

   nir_noltis_select(noltis);

   bool progress = false;

   nir_foreach_block_reverse(block, impl) {
      nir_foreach_instr_reverse_safe(instr, block) {
         nir_noltis_tile *tile = nir_noltis_get_tile(noltis, instr);
         if (!tile) {
            nir_instr_remove(instr);
            progress = true;
         } else if (tile->data) {
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            nir_def *old_def = &alu->def;
            struct fuse_state *fuse = tile->data;

            b.cursor = nir_before_instr(instr);

            nir_alu_instr *ffma = nir_alu_instr_create(shader, fuse->ffmaz ? nir_op_ffmaz : nir_op_ffma);
            for (int i = 0; i < ARRAY_SIZE(fuse->src); i++)
               ffma->src[i] = fuse->src[i];

            nir_def_init(&ffma->instr, &ffma->def, old_def->num_components, old_def->bit_size);
            nir_builder_instr_insert(&b, &ffma->instr);
            nir_def_rewrite_uses(old_def, &ffma->def);
            nir_instr_remove(instr);
            progress = true;
         }
      }
   }

   ralloc_free(noltis);

   nir_metadata_preserve(impl,
                           nir_metadata_block_index |
                           nir_metadata_dominance);
   return progress;
}
