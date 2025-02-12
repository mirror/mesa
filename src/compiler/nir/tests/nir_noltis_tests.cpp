/*
 * Copyright © 2018 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <gtest/gtest.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_noltis.h"

namespace
{

   class nir_noltis_test : public ::testing::Test
   {
   protected:
      nir_noltis_test();
      ~nir_noltis_test();

      nir_noltis_tile *add_tile(nir_def *def, uint32_t cost,
                                nir_def *edge0, nir_def *edge1,
                                nir_def *interior0, nir_def *interior1);
      void *mem_ctx;
      linear_ctx *lin_ctx;
      nir_noltis *noltis;

      nir_builder *b;
      nir_builder _b;
   };

   nir_noltis_test::nir_noltis_test()
   {
      mem_ctx = ralloc_context(NULL);
      lin_ctx = linear_context(mem_ctx);
      static const nir_shader_compiler_options options = {};
      _b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "noltis test");
      b = &_b;
   }

   nir_noltis_test::~nir_noltis_test()
   {
      if (HasFailure())
      {
         printf("\nShader from the failed test:\n\n");
         nir_print_shader(b->shader, stdout);
      }

      ralloc_free(mem_ctx);
   }

   nir_noltis_tile *
   nir_noltis_test::add_tile(nir_def *def, uint32_t cost,
                             nir_def *edge0, nir_def *edge1,
                             nir_def *interior0, nir_def *interior1)
   {
      nir_noltis_tile *tile = nir_noltis_tile_create(noltis, def->parent_instr,
                                                     NULL);
      if (edge0)
         nir_noltis_tile_add_edge(tile, edge0->parent_instr);
      if (edge1)
         nir_noltis_tile_add_edge(tile, edge1->parent_instr);
      if (interior0)
         nir_noltis_tile_add_interior(tile, interior0->parent_instr);
      if (interior1)
         nir_noltis_tile_add_interior(tile, interior1->parent_instr);

      tile->cost = cost;

      return tile;
   }

} // namespace

TEST_F(nir_noltis_test, paper_example)
{
   /* Use immediates as our reg/const nodes, since NOLTIS doesn't care about
    * the NIR instructions other than the graph's structure.
    */
   nir_def *x = nir_imm_int(b, 0);
   nir_def *y = nir_imm_int(b, 1);
   nir_def *i8 = nir_imm_int(b, 8);
   nir_def *add_x8 = nir_iadd(b, x, i8);
   nir_def *add_y8 = nir_iadd(b, y, i8);
   nir_def *add_x8_y8 = nir_iadd(b, add_x8, add_y8);

   noltis = nir_noltis_create(mem_ctx, b->impl);

   /* Since all our values are SSA, none of them should start out fixed, and
    * only i8 is shared.
    */
   nir_foreach_instr(instr, nir_cursor_current_block(b->cursor))
   {
      EXPECT_NE(nir_noltis_get_node(noltis, instr), (nir_noltis_node *)NULL);
      EXPECT_FALSE(nir_noltis_get_node(noltis, instr)->fixed);

      EXPECT_EQ(nir_noltis_get_node(noltis, instr)->shared,
                instr == i8->parent_instr);
   }

   /* The simple tiles that cover just the instruction. */
   nir_noltis_tile *tile_i8 =
       add_tile(i8, 5, NULL, NULL, NULL, NULL);
   add_tile(x, 1, NULL, NULL, NULL, NULL);
   add_tile(y, 1, NULL, NULL, NULL, NULL);

   /* The greedy tiles that NOLTIS selects before CSE. */
   add_tile(add_x8, 5, NULL, NULL, x, i8);
   add_tile(add_y8, 5, NULL, NULL, y, i8);
   nir_noltis_tile *tile_add_x8 =
       add_tile(add_x8, 1, i8, NULL, x, NULL);
   nir_noltis_tile *tile_add_y8 =
       add_tile(add_y8, 1, i8, NULL, y, NULL);
   nir_noltis_tile *tile_add_x8_y8 =
       add_tile(add_x8_y8, 1, add_x8, add_y8, NULL, NULL);

   /* Not called in the paper, but at least users of NIR NOLTIS should always
    * have a tile to cover every NIR instruction.
    */

   nir_noltis_select(noltis);

   EXPECT_EQ(nir_noltis_get_tile(noltis, i8->parent_instr), tile_i8);
   EXPECT_EQ(nir_noltis_get_tile(noltis, add_x8->parent_instr), tile_add_x8);
   EXPECT_EQ(nir_noltis_get_tile(noltis, add_y8->parent_instr), tile_add_y8);
   EXPECT_EQ(nir_noltis_get_tile(noltis, add_x8_y8->parent_instr), tile_add_x8_y8);

   /* No tiles should be selected for these. */
   EXPECT_EQ(nir_noltis_get_tile(noltis, x->parent_instr),
             (nir_noltis_tile *)NULL);
   EXPECT_EQ(nir_noltis_get_tile(noltis, y->parent_instr),
             (nir_noltis_tile *)NULL);
}


TEST_F(nir_noltis_test, fuse_ffma_basic)
{
   nir_def *x = nir_imm_int(b, 0);
   nir_def *y = nir_imm_int(b, 1);
   nir_def *z = nir_imm_int(b, 2);
   nir_def *mul = nir_fmul(b, x, y);
   nir_def *add = nir_fadd(b, mul, z);

   /* unary op to give us a reference to the add result after transformation */
   nir_def *result_fneg = nir_fneg(b, add);

   ASSERT_TRUE(nir_opt_fuse_ffma(b->shader, NULL));
   nir_validate_shader(b->shader, "after fuse_ffma");

   nir_alu_instr *fneg_instr = nir_instr_as_alu(result_fneg->parent_instr);
   nir_alu_instr *fma_instr = nir_instr_as_alu(fneg_instr->src[0].src.ssa->parent_instr);
   ASSERT_EQ(fma_instr->op, nir_op_ffma);
}

TEST_F(nir_noltis_test, fuse_ffma_matrix)
{
   nir_def *x = nir_imm_vec4(b, 0, 1, 2, 3);
   nir_def *m[4] = {
      nir_imm_vec4(b, 1, 0, 0, 0),
      nir_imm_vec4(b, 0, 2, 0, 0),
      nir_imm_vec4(b, 0, 0, 3, 0),
      nir_imm_vec4(b, 0, 0, 0, 4),
   };

   nir_def *last = nir_fmul(b, x, m[0]);
   for (int i = 1; i < 4; i++)
      last = nir_fadd(b, last, nir_fmul(b, x, m[i]));

   ASSERT_TRUE(nir_opt_fuse_ffma(b->shader, NULL));
   nir_validate_shader(b->shader, "after fuse_ffma");
}

/* XXX: test banned fuse */
/* XXX: test costs? */
