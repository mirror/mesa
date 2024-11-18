/*
 * Copyright © 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#ifndef NIR_ALGEBRAIC_PATTERN_TEST_H
#define NIR_ALGEBRAIC_PATTERN_TEST_H

#include "nir.h"
#include "nir_test.h"

class nir_algebraic_pattern_test : public nir_test {
 protected:
   nir_algebraic_pattern_test(const char *name);
   virtual ~nir_algebraic_pattern_test();

   void validate_pattern();

 public:
   uint32_t input_count;
   uint32_t *input_map;
   uint32_t fuzzing_bits;
   uint32_t seed;
   bool exact = true;
   uint32_t fp_fast_math = FLOAT_CONTROLS_SIGNED_ZERO_PRESERVE |
                           FLOAT_CONTROLS_INF_PRESERVE |
                           FLOAT_CONTROLS_NAN_PRESERVE;
   nir_const_value *tmp_values;
};

#endif
