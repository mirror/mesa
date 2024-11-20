/*
 * Copyright © 2024 Valve Corporation
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

#include "nir_test.h"

class nir_opt_cse_test : public nir_test {
protected:
   nir_opt_cse_test()
      : nir_test::nir_test("nir_opt_cse_test")
   {
   }

   unsigned count_alu()
   {
      unsigned num_alu = 0;
      nir_foreach_block(block, nir_shader_get_entrypoint(b->shader)) {
         nir_foreach_instr(instr, block)
            num_alu += instr->type == nir_instr_type_alu;
      }
      return num_alu;
   }
};

TEST_F(nir_opt_cse_test, rewrite_header_phis)
{
   /*
    * Tests that updating the sources of loop header phis causes them to be revisited.
    *
    * loop {
    *     block b1:  // preds: b0 b1
    *     32    %7 = phi b0: %1 (0x0), b1: %4 (0x1)
    *     32    %5 = phi b0: %0 (0x0), b1: %3 (0x1)
    *     32    %3 = load_const (0x00000001)
    *     32    %4 = load_const (0x00000001)
    *     32    %6 = ineg %5
    *     32    %8 = ineg %7
    *                // succs: b1 
    * }
    */
   nir_def *zero[2] = {nir_imm_int(b, 0), nir_imm_int(b, 0)};

   nir_imm_int(b, 1); /* This exists so that both phis will to be updated before they are identical. */

   nir_push_loop(b);

   nir_def *one[2] = {nir_imm_int(b, 1), nir_imm_int(b, 1)};

   nir_phi_instr *phi[2];
   for (unsigned i = 0; i < 2; i++) {
      phi[i] = nir_phi_instr_create(b->shader);
      nir_phi_instr_add_src(phi[i], zero[i]->parent_instr->block, zero[i]);
      nir_phi_instr_add_src(phi[i], one[i]->parent_instr->block, one[i]);
      nir_def_init(&phi[i]->instr, &phi[i]->def, 1, 32);
      nir_instr_insert_before_block(one[i]->parent_instr->block, &phi[i]->instr);

      nir_ineg(b, &phi[i]->def);
   }

   nir_pop_loop(b, NULL);

   ASSERT_TRUE(nir_opt_cse(b->shader));
   ASSERT_EQ(count_alu(), 1);

   nir_validate_shader(b->shader, NULL);
}

TEST_F(nir_opt_cse_test, break_and_exit_interrupted)
{
   /*
    * Test that blocks between a break block and the loop exit do not interfere with CSE between
    * those two blocks.
    *
    * loop {
    *     block b1:  // preds: b0 b4
    *     1     %1 = load_const (false)
    *                // succs: b2 b3
    *     if %1 (false) {
    *         block b2:  // preds: b1
    *         32    %2 = ineg %0 (0x0)
    *                    break
    *                    // succs: b5
    *     } else {
    *         block b3:  // preds: b1, succs: b4
    *     }
    *     block b4:  // preds: b3
    *     32    %3 = ineg %0 (0x0)
    *                // succs: b1
    * }
    * block b5:  // preds: b2
    * 32    %4 = ineg %0 (0x0)
    */
   nir_def *src = nir_imm_int(b, 0);

   nir_push_loop(b);

   nir_push_if(b, nir_imm_false(b));
   nir_ineg(b, src);
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, NULL);

   nir_ineg(b, src);

   nir_pop_loop(b, NULL);

   nir_ineg(b, src);

   ASSERT_TRUE(nir_opt_cse(b->shader));
   ASSERT_EQ(count_alu(), 2);

   nir_validate_shader(b->shader, NULL);
}
