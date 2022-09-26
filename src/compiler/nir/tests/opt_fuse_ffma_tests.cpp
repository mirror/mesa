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
   class nir_opt_fuse_ffma_test : public ::testing::Test
   {
   protected:
      nir_opt_fuse_ffma_test();
      ~nir_opt_fuse_ffma_test();

      nir_noltis_tile *add_tile(nir_def *def, uint32_t cost,
                                nir_def *edge0, nir_def *edge1,
                                nir_def *interior0, nir_def *interior1);
      void *mem_ctx;
      linear_ctx *lin_ctx;
      nir_noltis *noltis;

      nir_builder *b;
      nir_builder _b;

      int opcode_count(nir_op op) {
         int count = 0;
         nir_foreach_block(block, b->impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_alu)
                  continue;
               if (nir_instr_as_alu(instr)->op == op)
                  count++;
            }
         }
         return count;
      }
   };

   nir_opt_fuse_ffma_test::nir_opt_fuse_ffma_test()
   {
      mem_ctx = ralloc_context(NULL);
      lin_ctx = linear_context(mem_ctx);
      static const nir_shader_compiler_options options = {};
      _b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "nir_opt_fuse_ffma test");
      b = &_b;
   }

   nir_opt_fuse_ffma_test::~nir_opt_fuse_ffma_test()
   {
      if (HasFailure())
      {
         printf("\nShader from the failed test:\n\n");
         nir_print_shader(b->shader, stdout);
      }

      ralloc_free(mem_ctx);
   }

} // namespace

TEST_F(nir_opt_fuse_ffma_test, matrix)
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

   ASSERT_EQ(opcode_count(nir_op_fmul), 1);
   ASSERT_EQ(opcode_count(nir_op_ffma), 3);
}

static int filter_false(nir_alu_src *mul0, nir_alu_src *mul1, nir_alu_src *add)
{
   return -1;
}

TEST_F(nir_opt_fuse_ffma_test, filter)
{
   nir_def *x = nir_imm_int(b, 0);
   nir_def *y = nir_imm_int(b, 1);
   nir_def *z = nir_imm_int(b, 2);
   nir_def *mul = nir_fmul(b, x, y);
   nir_fadd(b, mul, z);

   ASSERT_FALSE(nir_opt_fuse_ffma(b->shader, filter_false));
   nir_validate_shader(b->shader, "after fuse_ffma");

   ASSERT_EQ(opcode_count(nir_op_fmul), 1);
   ASSERT_EQ(opcode_count(nir_op_ffma), 0);
}

TEST_F(nir_opt_fuse_ffma_test, fmaz)
{
   nir_def *x = nir_imm_int(b, 0);
   nir_def *y = nir_imm_int(b, 1);
   nir_def *z = nir_imm_int(b, 2);
   nir_def *mul = nir_fmulz(b, x, y);
   nir_fadd(b, mul, z);

   ASSERT_TRUE(nir_opt_fuse_ffma(b->shader, NULL));
   nir_validate_shader(b->shader, "after fuse_ffma");

   ASSERT_EQ(opcode_count(nir_op_ffma), 0);
   ASSERT_EQ(opcode_count(nir_op_ffmaz), 1);
}
