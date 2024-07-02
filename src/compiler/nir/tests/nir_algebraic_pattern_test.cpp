/*
 * Copyright © 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "nir_algebraic_pattern_test.h"
#include "nir_builder.h"
#include "nir_constant_expressions.h"

#include "util/memstream.h"

#include <float.h>
#include <math.h>

nir_algebraic_pattern_test::nir_algebraic_pattern_test(const char *name)
    : nir_test(name)
{
}

static nir_const_value *
tmp_value(nir_algebraic_pattern_test *test, nir_def *def)
{
   return &test->tmp_values[def->index * NIR_MAX_VEC_COMPONENTS];
}

static bool
def_annotate_value(nir_def *def, void *data)
{
   nir_algebraic_pattern_test *test = (nir_algebraic_pattern_test *)data;

   char *annotation = NULL;
   size_t annotation_size = 0;
   u_memstream mem;
   if (!u_memstream_open(&mem, &annotation, &annotation_size))
      return true;

   FILE *output = u_memstream_get(&mem);

   nir_const_value *value = tmp_value(test, def);

   fprintf(output, "// ");
   if (def->num_components == 1) {
      fprintf(output, "0x%lx", value->u64);
      switch (def->bit_size) {
      case 16:
         fprintf(output, " = %f", _mesa_half_to_float(value->u16));
         break;
      case 32:
         fprintf(output, " = %f", value->f32);
         break;
      case 64:
         fprintf(output, " = %f", value->f64);
         break;
      default:
         break;
      }
   } else {
      fprintf(output, "(");
      for (uint32_t comp = 0; comp < def->num_components; comp++) {
         if (comp > 0)
            fprintf(output, ", ");
         fprintf(output, "0x%lx", value[comp].u64);
      }
      fprintf(output, ") = (");
      for (uint32_t comp = 0; comp < def->num_components; comp++) {
         if (comp > 0)
            fprintf(output, ", ");
         switch (def->bit_size) {
         case 16:
            fprintf(output, "%f", _mesa_half_to_float(value[comp].u16));
            break;
         case 32:
            fprintf(output, "%f", value[comp].f32);
            break;
         case 64:
            fprintf(output, "%f", value[comp].f64);
            break;
         default:
            break;
         }
      }
      fprintf(output, ")");
   }

   fputc(0, output);

   u_memstream_close(&mem);

   _mesa_hash_table_insert(test->annotations, def->parent_instr, annotation);

   return true;
}

static bool
instr_annotate_value(nir_builder *b, nir_instr *instr, void *data)
{
   nir_foreach_def(instr, def_annotate_value, data);
   return false;
}

nir_algebraic_pattern_test::~nir_algebraic_pattern_test()
{
   if (HasFailure()) {
      annotations = _mesa_pointer_hash_table_create(nullptr);
      nir_shader_instructions_pass(b->shader, instr_annotate_value, nir_metadata_all, this);
   }

   free(tmp_values);
}

static bool
count_input(nir_builder *b, nir_intrinsic_instr *intrinsic, void *data)
{
   nir_algebraic_pattern_test *test = (nir_algebraic_pattern_test *)data;

   if (intrinsic->intrinsic == nir_intrinsic_provide)
      test->input_count = MAX2(test->input_count, (uint32_t)nir_intrinsic_base(intrinsic) + 1);

   return false;
}

static bool
nir_def_is_used_as(nir_def *def, nir_alu_type type)
{
   nir_foreach_use(use, def) {
      nir_instr *use_instr = nir_src_parent_instr(use);
      if (use_instr->type != nir_instr_type_alu)
         continue;

      nir_alu_instr *use_alu = nir_instr_as_alu(use_instr);

      uint32_t i = container_of(use, nir_alu_src, src) - use_alu->src;
      assert(i < nir_op_infos[use_alu->op].num_inputs);

      if (nir_alu_type_get_base_type(nir_op_infos[use_alu->op].input_types[i]) == type)
         return true;

      if (nir_op_is_vec_or_mov(use_alu->op) && nir_def_is_used_as(&use_alu->def, type))
         return true;
   }

   return false;
}

#define INPUT_VALUE_COUNT_LOG2 3
#define INPUT_VALUE_COUNT      (1 << INPUT_VALUE_COUNT_LOG2)
#define INPUT_VALUE_MASK       ((1 << INPUT_VALUE_COUNT_LOG2) - 1)

static uint32_t
nir_def_get_seed_bit_size(nir_def *def)
{
   if (nir_def_is_used_as(def, nir_type_bool))
      return 1;
   else
      return INPUT_VALUE_COUNT_LOG2;
}

static bool
map_input(nir_builder *b, nir_intrinsic_instr *intrinsic, void *data)
{
   nir_algebraic_pattern_test *test = (nir_algebraic_pattern_test *)data;

   if (intrinsic->intrinsic != nir_intrinsic_provide)
      return false;

   test->input_map[nir_intrinsic_base(intrinsic)] = test->fuzzing_bits;
   test->fuzzing_bits += nir_def_get_seed_bit_size(&intrinsic->def) * intrinsic->def.num_components;

   return false;
}

static const uint64_t uint_inputs[INPUT_VALUE_COUNT] = {
   0,
   1,
   2,
   3,
   4,
   32,
   64,
   UINT64_MAX,
};

static const int64_t int_inputs[INPUT_VALUE_COUNT] = {
   0,
   1,
   -1,
   2,
   3,
   64,
   INT64_MIN,
   INT64_MAX,
};

static const double float_inputs[INPUT_VALUE_COUNT] = {
   0,
   1,
   -1,
   0.12345,
   NAN,
   INFINITY,
   -INFINITY,
   DBL_MIN,
};

static bool

skip_test(nir_algebraic_pattern_test *test, nir_alu_instr *alu, uint32_t bit_size,
          nir_const_value tmp, int32_t src_index, bool exact)
{
   /* Always pass the test for signed zero/nan/inf sources if they are not preserved. */
   switch (bit_size) {
   case 16:
      if ((!exact || !(test->fp_fast_math & FLOAT_CONTROLS_SIGNED_ZERO_PRESERVE_FP16)) && tmp.u16 == 0x8000)
         return true;
      if ((!exact || !(test->fp_fast_math & FLOAT_CONTROLS_NAN_PRESERVE_FP16)) && isnanf(_mesa_half_to_float(tmp.u16)))
         return true;
      if ((!exact || !(test->fp_fast_math & FLOAT_CONTROLS_INF_PRESERVE_FP16)) && isinff(_mesa_half_to_float(tmp.u16)))
         return true;
      break;
   case 32:
      if ((!exact || !(test->fp_fast_math & FLOAT_CONTROLS_SIGNED_ZERO_PRESERVE_FP32)) && tmp.f32 == 0.0 && signbit(tmp.f32))
         return true;
      if ((!exact || !(test->fp_fast_math & FLOAT_CONTROLS_NAN_PRESERVE_FP32)) && isnanf(tmp.f32))
         return true;
      if ((!exact || !(test->fp_fast_math & FLOAT_CONTROLS_INF_PRESERVE_FP32)) && isinff(tmp.f32))
         return true;
      break;
   case 64:
      if ((!exact || !(test->fp_fast_math & FLOAT_CONTROLS_SIGNED_ZERO_PRESERVE_FP64)) && tmp.f64 == 0.0 && signbit(tmp.f64))
         return true;
      if ((!exact || !(test->fp_fast_math & FLOAT_CONTROLS_NAN_PRESERVE_FP64)) && isnan(tmp.f64))
         return true;
      if ((!exact || !(test->fp_fast_math & FLOAT_CONTROLS_INF_PRESERVE_FP64)) && isinf(tmp.f64))
         return true;
      break;
   default:
      break;
   }

   switch (alu->op) {
   case nir_op_bitfield_insert:
      if (src_index > 1 && tmp.u64 >= bit_size)
         return true;
      break;
   case nir_op_ibitfield_extract:
   case nir_op_ubitfield_extract:
      if (src_index > 0 && tmp.u64 >= bit_size)
         return true;
      break;
   default:
      break;
   }

   return false;
}

static bool
compare_inexact(double a, double b, uint32_t bit_size)
{
   return abs(a - b) > pow(0.5, bit_size / 4);
}

static bool
evaluate_expression(nir_algebraic_pattern_test *test, nir_instr *instr)
{
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instr);

      if (intrinsic->intrinsic == nir_intrinsic_provide) {
         uint32_t seed_bit_size = nir_def_get_seed_bit_size(&intrinsic->def);

         for (uint32_t comp = 0; comp < intrinsic->def.num_components; comp++) {
            nir_const_value input = { 0 };

            uint32_t seed = test->seed >> (test->input_map[nir_intrinsic_base(intrinsic)] +
                                           seed_bit_size * comp);

            if (nir_def_is_used_as(&intrinsic->def, nir_type_bool)) {
               input.u64 = (seed & 1) ? ~0llu : 0;
            } else if (nir_def_is_used_as(&intrinsic->def, nir_type_float)) {
               if (intrinsic->def.bit_size == 64)
                  input.f64 = float_inputs[seed & INPUT_VALUE_MASK];
               else if (intrinsic->def.bit_size == 32)
                  input.f32 = float_inputs[seed & INPUT_VALUE_MASK];
               else if (intrinsic->def.bit_size == 16)
                  input.u16 = _mesa_float_to_half(float_inputs[seed & INPUT_VALUE_MASK]);
            } else if (nir_def_is_used_as(&intrinsic->def, nir_type_uint)) {
               input.u64 = uint_inputs[seed & INPUT_VALUE_MASK];
            } else {
               input.i64 = int_inputs[seed & INPUT_VALUE_MASK];
            }

            input.u64 = input.u64 & BITFIELD64_MASK(intrinsic->def.bit_size);

            tmp_value(test, &intrinsic->def)[comp] = input;
         }
      } else if (intrinsic->intrinsic == nir_intrinsic_assert_eq) {
         nir_const_value *src0 = tmp_value(test, intrinsic->src[0].ssa);
         nir_const_value *src1 = tmp_value(test, intrinsic->src[1].ssa);

         assert(intrinsic->src[0].ssa->bit_size == intrinsic->src[1].ssa->bit_size);
         uint32_t bit_size = intrinsic->src[0].ssa->bit_size;

         assert(intrinsic->src[0].ssa->num_components == intrinsic->src[1].ssa->num_components);
         uint32_t num_components = intrinsic->src[0].ssa->num_components;

         nir_alu_instr *alu0 = nir_src_as_alu_instr(intrinsic->src[0]);
         nir_alu_instr *alu1 = nir_src_as_alu_instr(intrinsic->src[1]);
         bool is_float = (alu0 && nir_alu_type_get_base_type(nir_op_infos[alu0->op].output_type) == nir_type_float) ||
                         (alu1 && nir_alu_type_get_base_type(nir_op_infos[alu1->op].output_type) == nir_type_float);

         for (uint32_t comp = 0; comp < num_components; comp++) {
            nir_const_value a = src0[comp];
            nir_const_value b = src1[comp];

            switch (bit_size) {
            case 1:
               if ((a.u8 & 1) != (b.u8 & 1))
                  return false;
               break;
            case 8:
               if (a.u8 != b.u8)
                  return false;
               break;
            case 16:
               if (is_float || !test->exact) {
                  if (test->exact) {
                     if (isnanf(_mesa_half_to_float(a.u16)) && isnanf(_mesa_half_to_float(b.u16)))
                        break;
                     if (a.u16 != b.u16)
                        return false;
                  } else {
                     if (compare_inexact(_mesa_half_to_float(a.u16), _mesa_half_to_float(b.u16), 16))
                        return false;
                  }
               } else {
                  if (a.u16 != b.u16)
                     return false;
               }
               break;
            case 32:
               if (is_float || !test->exact) {
                  if (test->exact) {
                     if (isnanf(a.f32) && isnanf(b.f32))
                        break;
                     if (a.u32 != b.u32)
                        return false;
                  } else {
                     if (compare_inexact(a.f32, b.f32, 32))
                        return false;
                  }
               } else {
                  if (a.u32 != b.u32)
                     return false;
               }
               break;
            case 64:
               if (is_float || !test->exact) {
                  if (test->exact) {
                     if (isnan(a.f64) && isnan(b.f64))
                        break;
                     if (a.u64 != b.u64)
                        return false;
                  } else {
                     if (compare_inexact(a.f64, b.f64, 64))
                        return false;
                  }
               } else {
                  if (a.u64 != b.u64)
                     return false;
               }
               break;
            default:
               break;
            }
         }

         return true;
      }

      return false;
   }

   if (instr->type == nir_instr_type_load_const) {
      nir_load_const_instr *load_const = nir_instr_as_load_const(instr);

      for (uint32_t i = 0; i < load_const->def.num_components; i++)
         tmp_value(test, &load_const->def)[i] = load_const->value[i];

      return false;
   }

   nir_alu_instr *alu = nir_instr_as_alu(instr);

   uint32_t bit_size = 0;
   if (!nir_alu_type_get_type_size(nir_op_infos[alu->op].output_type))
      bit_size = alu->def.bit_size;

   nir_const_value src[NIR_ALU_MAX_INPUTS][NIR_MAX_VEC_COMPONENTS];
   for (uint32_t i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
      if (bit_size == 0 &&
          !nir_alu_type_get_type_size(nir_op_infos[alu->op].input_types[i]))
         bit_size = alu->src[i].src.ssa->bit_size;

      for (uint32_t j = 0; j < nir_ssa_alu_instr_src_components(alu, i); j++) {
         nir_const_value tmp = tmp_value(test, alu->src[i].src.ssa)[alu->src[i].swizzle[j]];
         src[i][j] = tmp;

         if (skip_test(test, alu, alu->src[i].src.ssa->bit_size, tmp, i, test->exact))
            return true;
      }
   }

   if (bit_size == 0)
      bit_size = 32;

   nir_const_value *srcs[NIR_MAX_VEC_COMPONENTS];
   for (uint32_t i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
      srcs[i] = src[i];

   nir_const_value *dest = tmp_value(test, &alu->def);

   nir_eval_const_opcode(alu->op, dest, alu->def.num_components, bit_size, srcs, test->fp_fast_math);

   for (uint32_t comp = 0; comp < alu->def.num_components; comp++) {
      if (skip_test(test, alu, bit_size, dest[comp], -1, test->exact))
         return true;
   }

   return false;
}

void
nir_algebraic_pattern_test::validate_pattern()
{
   input_count = 0;
   fuzzing_bits = 0;

   nir_function_impl *impl = nir_shader_get_entrypoint(b->shader);
   nir_index_ssa_defs(impl);

   nir_validate_shader(b->shader, "validate_pattern");

   tmp_values = (nir_const_value *)calloc(NIR_MAX_VEC_COMPONENTS * impl->ssa_alloc, sizeof(nir_const_value));

   nir_shader_intrinsics_pass(b->shader, count_input, nir_metadata_all, this);

   input_map = (uint32_t *)calloc(input_count, sizeof(uint32_t));
   nir_shader_intrinsics_pass(b->shader, map_input, nir_metadata_all, this);

   bool result = true;

   bool overflow = fuzzing_bits > 16;
   if (overflow)
      fuzzing_bits = 16;

   nir_block *block = nir_impl_last_block(impl);

   uint32_t iterations = 1 << fuzzing_bits;
   for (uint32_t i = 0; i < iterations; i++) {
      if (overflow)
         seed = _mesa_hash_u32(&i);
      else
         seed = i;

      bool passed = false;
      nir_foreach_instr(instr, block) {
         if (evaluate_expression(this, instr)) {
            passed = true;
            break;
         }
      }

      if (!passed) {
         result = false;
         break;
      }
   }

   free(input_map);

   ASSERT_TRUE(result);
}
