/*
 * Copyright (c) 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net>
 * SPDX-License-Identifier: MIT
 */

#include "rkt_ml.h"
#include "rkt_registers.h"

#include "drm-uapi/rocket_accel.h"

#include "pipe/p_state.h"
#include "util/macros.h"
#include "util/u_dynarray.h"
#include "util/u_inlines.h"

#include <stdint.h>
#include <xf86drm.h>
#include <fcntl.h>

// http://nvdla.org/hw/v1/ias/unit_description.html#convolution-buffer
#define CBUF_BANK_SIZE 32768
#define CBUF_BANKS 12
#define CBUF_ENTRIES_PER_BANK 256
#define CBUF_ENTRY_SIZE (CBUF_BANK_SIZE / CBUF_ENTRIES_PER_BANK)
#define FEATURE_ATOMIC_SIZE 16
#define WEIGHT_ATOMIC_SIZE 32
#define ATOMIC_K_SIZE 16

static void
trace_printk(const char *restrict format, ...)
{
   // static int fd = -1;
   // if (unlikely(fd == -1)) {
   //    fd = open("/sys/kernel/tracing/trace_marker", O_WRONLY);
   //    assert(fd >= 0);
   // }

   // va_list args;
   // va_start(args, format);
   // vdprintf(fd, format, args);
   // va_end(args);

   // va_list args;
   // va_start(args, format);
   // vfprintf(stderr, format, args);
   // va_end(args);
}

static void
dump_buffer(const uint8_t *ptr, char *name, int operation_nr, int suboperation_nr, int offset, unsigned size)
{
   char buffer[255];

   snprintf(buffer, sizeof(buffer), "mesa-%s-%03u-%03u.bin", name, operation_nr, suboperation_nr);

   // ML_DBG("Dumping buffer from 0x%lx at offset %d with size %d to %s\n", ptr, offset, size, buffer);

   FILE *f = fopen(buffer, "wb");
   assert(f);
   fwrite(ptr + offset, 1, size, f);
   if(ferror(f)) {
      fprintf(stderr, "Error in writing to file: %s\n", strerror(errno));
   }
   fflush(f);
   fclose(f);
}

static void
create_tensor(struct rkt_ml_subgraph *subgraph, unsigned idx, unsigned size)
{
   struct pipe_context *context = subgraph->base.context;
   struct pipe_resource **tensors = util_dynarray_begin(&subgraph->tensors);

   assert(idx < util_dynarray_num_elements(&subgraph->tensors, struct pipe_resource *));

   struct pipe_resource *res = tensors[idx];

   if (res != NULL) {
      assert(size == pipe_buffer_size(res));
      return;
   }

   res = pipe_buffer_create(context->screen, 0, PIPE_USAGE_DEFAULT, size);
   tensors[idx] = res;
}

static struct rkt_resource *
get_tensor(struct rkt_ml_subgraph *subgraph, unsigned idx)
{
   return rkt_resource(*util_dynarray_element(&subgraph->tensors, struct pipe_resource *, idx));
}

static void
emit_raw(struct util_dynarray *regs, uint32_t target, uint32_t reg, uint32_t value)
{
   uint64_t packed_value = 0;
   packed_value = ((uint64_t) target) << 48;
   packed_value |= ((uint64_t) value) << 16;
   packed_value |= (uint64_t) reg;

   util_dynarray_append(regs, uint64_t, packed_value);
}

static void
emit(struct util_dynarray *regs, uint32_t reg, uint32_t value)
{
   uint32_t target = rkt_get_target(reg) + 0x1;
   emit_raw(regs, target, reg, value);
}

#define EMIT(offset, value) emit(regs, offset, value);

static bool
is_depthwise(const struct pipe_ml_operation *poperation)
{
   unsigned input_channels = poperation->input_tensors[0]->dims[3];
   unsigned output_channels = poperation->output_tensors[0]->dims[3];

   return poperation->conv.depthwise && input_channels > 1 && output_channels > 1;
}

static unsigned
calc_entries_per_slice(struct rkt_operation *operation)
{
   unsigned bpe = sizeof(uint8_t);
   unsigned atomics_per_entry = CBUF_ENTRY_SIZE / FEATURE_ATOMIC_SIZE;
   unsigned total_c_atomics = DIV_ROUND_UP(operation->input_channels * bpe, FEATURE_ATOMIC_SIZE);
   unsigned last_c_atomics  = total_c_atomics % atomics_per_entry ;
   unsigned int_c_entries   = (total_c_atomics / atomics_per_entry) * operation->input_width;
   unsigned frac_c_entries  = (last_c_atomics == 3) ? operation->input_width : DIV_ROUND_UP(last_c_atomics * operation->input_width, atomics_per_entry);

   return int_c_entries + frac_c_entries;
}

static unsigned
calc_input_banks(struct rkt_operation *operation)
{
   unsigned entries_per_slice = calc_entries_per_slice(operation);
   return DIV_ROUND_UP(entries_per_slice * operation->input_height, CBUF_ENTRIES_PER_BANK);
}

static unsigned
calc_weights_banks(struct rkt_operation *operation)
{
   unsigned bpe = sizeof(uint8_t);
   unsigned bytes = operation->weights_width * operation->weights_height * operation->input_channels * bpe;
   unsigned entries;
   unsigned banks;

   if (!operation->depthwise)
      bytes *= operation->output_channels;
   entries = DIV_ROUND_UP(bytes, CBUF_ENTRY_SIZE);
   banks = DIV_ROUND_UP(entries, CBUF_ENTRIES_PER_BANK);

   /* Why do we need an extra bank? The calc above might be wrong on this HW */
   banks++;

   return banks;
}

static unsigned
calc_line_stride(unsigned width)
{
   return width * ATOMIC_K_SIZE * sizeof(uint8_t);
}

static void
calc_explicit_padding(const struct rkt_operation *operation, unsigned *pad_top, unsigned *pad_bottom, unsigned *pad_left, unsigned *pad_right)
{
   if (operation->padding_same && operation->weights_width > 1) {
      /* Convert from implicit to explicit padding */
      unsigned pad_along_width = MAX2((operation->output_width - 1) * operation->stride + operation->weights_width - operation->input_width, 0);
      unsigned pad_along_height = MAX2((operation->output_height - 1) * operation->stride + operation->weights_height - operation->input_height, 0);
      *pad_left = pad_along_height / 2;
      *pad_right = pad_along_height - *pad_left;
      *pad_top = pad_along_width / 2;
      *pad_bottom = pad_along_width - *pad_top;
   } else {
      *pad_left = 0;
      *pad_right = 0;
      *pad_top = 0;
      *pad_bottom = 0;
   }
}

static void
fill_task(struct rkt_ml_subgraph *subgraph, struct rkt_operation *operation, struct split_task *task)
{
   task->stride_x = operation->stride;
   task->stride_y = operation->stride;

   task->input_width = operation->input_width;
   if (task->input_width == 8 && (operation->addition_input || operation->add_tensor != -1))
      task->input_width *= 2;

   task->input_height = operation->input_height;
   task->input_channels = ALIGN(MAX2(operation->input_channels, FEATURE_ATOMIC_SIZE), FEATURE_ATOMIC_SIZE);
   task->input_channels_real = operation->input_channels;
   task->input_zero_point = operation->input_zero_point;
   task->input_scale = operation->input_scale;

   task->output_width = operation->output_width;
   task->output_height = operation->output_height;

   task->output_channels_real = operation->output_channels;
   task->output_channels = ALIGN(MAX2(operation->output_channels, 32), 32);
   if (operation->depthwise) {
      if (task->output_channels_real <= 32)
         task->output_channels *= 2;
      task->output_channels = ALIGN(task->output_channels, 64);
   }

   task->output_zero_point = operation->output_zero_point;
   task->output_scale = operation->output_scale;

   if (task->input_channels_real == 1 &&
       (task->output_channels_real > 1 || (operation->addition_input || operation->add_tensor != -1))) {
      task->input_width = MAX2(task->input_width, FEATURE_ATOMIC_SIZE);
      task->input_line_stride = MAX2(calc_line_stride(operation->input_width) / FEATURE_ATOMIC_SIZE, FEATURE_ATOMIC_SIZE);

      if (operation->input_channels == 32 && operation->input_width == 80) {
         task->input_line_stride *= 4;
         task->input_surface_stride = (float)task->input_line_stride * (((float)task->input_height / 4) - 1);
      } else
         task->input_surface_stride = (float)task->input_line_stride * (((float)task->input_height) - 1);
   } else {
      task->input_line_stride = calc_line_stride(operation->input_width) / 4;
      task->input_surface_stride = (float)task->input_line_stride * (((float)task->input_height / 4) - 1);
   }

   if (task->input_width == 8 && (operation->addition_input || operation->add_tensor != -1)) {
      task->input_line_stride /= 2;
      task->input_surface_stride = 112;
   }

   int output_line_stride = calc_line_stride(operation->output_width);
   task->output_surface_stride = output_line_stride * task->output_height;
   task->output_surface_stride /= FEATURE_ATOMIC_SIZE;

   if (task->input_channels_real == 1)
      task->input_data_entries = task->input_width * task->input_height;
   else if (task->input_width == 40 && task->input_channels_real == 40)
      task->input_data_entries = 40;
   else
      task->input_data_entries = DIV_ROUND_UP(task->input_width * 2 * DIV_ROUND_UP(task->input_channels_real, FEATURE_ATOMIC_SIZE), 8);

   task->weights_width = operation->weights_width;
   task->weights_height = operation->weights_height;
   task->weights_zero_point = operation->weights_zero_point;
   task->weights_scale = operation->weights_scale;

   if (operation->depthwise)
      task->weights_kernels = 1;
   else
      task->weights_kernels = ALIGN(operation->output_channels, 2);

   task->surfaces_per_row = task->output_width * task->output_height * 2;
   if (operation->depthwise)
      task->surfaces_per_row *= 2;
}

static void
split_tasks(struct rkt_ml_subgraph *subgraph, struct rkt_operation *operation)
{
   /* Function mostly taken from NVDLA */
   unsigned entries_per_slice = calc_entries_per_slice(operation);
   unsigned input_banks_required = calc_input_banks(operation);
   unsigned weights_banks_required = calc_weights_banks(operation);
   unsigned available_weights_banks = weights_banks_required;
   unsigned available_input_banks = CBUF_BANKS - weights_banks_required;
   unsigned pad_top;
   unsigned pad_bottom;
   unsigned pad_left;
   unsigned pad_right;

   calc_explicit_padding(operation, &pad_top, &pad_bottom, &pad_left, &pad_right);

   if (weights_banks_required + 1 < CBUF_BANKS) {
      /* Full weights, partial input */
      operation->reuse_weights_cbuf = true;
   } else {
      /* Partial weights, partial input */
      operation->reuse_weights_cbuf = false;
      available_input_banks = 7;
      available_weights_banks = CBUF_BANKS - available_input_banks;
   }

   if (input_banks_required <= available_input_banks) {
      /* Full weights, full input */

      struct split_task task = {0};
      
      task.num = 0;
      fill_task(subgraph, operation, &task);
      task.input_banks = input_banks_required;
      task.weights_banks = CBUF_BANKS - task.input_banks;
      task.input_height = operation->input_height;

      task.pad_top = pad_top;
      task.pad_bottom = pad_bottom;
      task.pad_left = pad_left;
      task.pad_right = pad_right;

      task.atomic_count = task.output_width * task.output_height;

      util_dynarray_append(&operation->tasks, struct split_task, task);

      return;
   }

   struct split_task task = {0};
   unsigned available_slices = (CBUF_ENTRIES_PER_BANK * available_input_banks) / entries_per_slice;

   task.num = 0;
   fill_task(subgraph, operation, &task);
   task.input_banks = available_input_banks;
   task.weights_banks = available_weights_banks;

   task.top_slice = 0;
   task.bottom_slice = available_slices - 1;

   task.pad_top = pad_top;
   task.pad_left = pad_left;
   task.pad_right = pad_right;

   util_dynarray_append(&operation->tasks, struct split_task, task);

   for (unsigned slice = operation->weights_height - pad_top - 1; slice < operation->input_height;) {
      memset(&task, 0, sizeof(task));

      struct split_task *prev_task = util_dynarray_element(&operation->tasks,
                                                           struct split_task,
                                                           util_dynarray_num_elements(&operation->tasks, struct split_task) - 1);

      while (slice <= prev_task->bottom_slice) {
         slice += operation->stride;
      }
      if (slice > prev_task->bottom_slice) {
         slice -= operation->stride;
      }

      task.num = util_dynarray_num_elements(&operation->tasks, struct split_task);
      fill_task(subgraph, operation, &task);
      task.top_slice = MIN2(slice, prev_task->bottom_slice) - (operation->weights_height - 1) + operation->stride;
      task.bottom_slice = task.top_slice + available_slices - 1;
      task.pad_left = pad_left;
      task.pad_right = pad_right;

      // check if current task is the last one
      if (task.bottom_slice >= operation->input_height - 1) {
         task.bottom_slice = operation->input_height - 1;
         task.pad_bottom = pad_bottom;
         util_dynarray_append(&operation->tasks, struct split_task, task);
         break;
      }

      slice = task.top_slice + operation->weights_height - 1;
      util_dynarray_append(&operation->tasks, struct split_task, task);
   }

   struct split_task *last_task = util_dynarray_element(&operation->tasks,
                                                        struct split_task,
                                                        util_dynarray_num_elements(&operation->tasks, struct split_task) - 1);
   if (last_task->top_slice >= operation->input_height ||
       last_task->bottom_slice >= (operation->input_height + pad_bottom)) {
      util_dynarray_pop(&operation->tasks, struct split_task);
   }

   // determine overlap slices between 2 split chunks
   for (int i = 1; i < util_dynarray_num_elements(&operation->tasks, struct split_task); i++) {
      struct split_task *prev_task = util_dynarray_element(&operation->tasks, struct split_task, i - 1);
      struct split_task *cur_task = util_dynarray_element(&operation->tasks, struct split_task, i);

      if (prev_task->bottom_slice >= cur_task->top_slice) {
         cur_task->num_overlap_slices = prev_task->bottom_slice - cur_task->top_slice + 1;
         prev_task->num_retain_slices = cur_task->num_overlap_slices;
      } else {
         cur_task->num_overlap_slices = 0;
         prev_task->num_retain_slices = 0;
      }
   }

   unsigned output_height_processed = 0;
   for (int i = 0; i < util_dynarray_num_elements(&operation->tasks, struct split_task); i++) {
      struct split_task *cur_task = util_dynarray_element(&operation->tasks, struct split_task, i);

      unsigned slice = cur_task->top_slice + (operation->weights_height - 1) - cur_task->pad_top;

      while (slice <= cur_task->bottom_slice + cur_task->pad_bottom) {
         slice += operation->stride;
         cur_task->convolutions++;
      }

      cur_task->bottom_slice = MIN2(cur_task->bottom_slice, operation->input_height - 1);

      cur_task->input_height = cur_task->bottom_slice - cur_task->top_slice + 1;

      cur_task->output_width = (cur_task->input_width + cur_task->pad_left + cur_task->pad_right - operation->weights_width) / operation->stride + 1;
      cur_task->output_height = (cur_task->input_height + cur_task->pad_top + cur_task->pad_bottom - operation->weights_height) / operation->stride + 1;
      cur_task->atomic_count = cur_task->output_width * cur_task->output_height;

      cur_task->input_offset = calc_line_stride(operation->input_width) * cur_task->top_slice;
      cur_task->output_offset = calc_line_stride(operation->output_width) * output_height_processed;

      cur_task->input_banks = available_input_banks;
      cur_task->weights_banks = available_weights_banks;

      output_height_processed += cur_task->output_height;
   }
}

static unsigned
calc_raw_output_size(struct rkt_operation *operation)
{
   unsigned output_channels_1 = DIV_ROUND_UP(operation->output_channels, FEATURE_ATOMIC_SIZE) * 2;
   unsigned output_channels_2 = FEATURE_ATOMIC_SIZE;

   return operation->output_width * operation->output_height * output_channels_1 * output_channels_2;
}

static void
fill_first_regcmd(struct rkt_ml_subgraph *subgraph, const struct rkt_operation *operation, struct util_dynarray *regs, unsigned task_num)
{
   struct pipe_context *pcontext = subgraph->base.context;
   struct split_task *task = util_dynarray_element(&operation->tasks, struct split_task, task_num);
   unsigned num_tasks = util_dynarray_num_elements(&operation->tasks, struct split_task);
   unsigned input_zero_point = task->input_zero_point;
   unsigned output_zero_point = task->output_zero_point;
   unsigned weights_zero_point = task->weights_zero_point;
   unsigned offset = output_zero_point - 0x80;

   uint32_t con0 = CNA_CBUF_CON0_WEIGHT_BANK (task->weights_banks) |
                   CNA_CBUF_CON0_DATA_BANK (task->input_banks);
   if (task_num > 0 && operation->reuse_weights_cbuf)
      con0 |= CNA_CBUF_CON0_WEIGHT_REUSE(1);

   EMIT(REG_CNA_CBUF_CON0, con0);

   EMIT(REG_CNA_DCOMP_REGNUM, 0);
   EMIT(REG_CNA_DCOMP_CTRL, 0);

   uint32_t con1 = 0x0;
   if (task->input_channels_real == 1) {
      con1 |= CNA_CONV_CON1_NONALIGN_DMA (1) | CNA_CONV_CON1_GROUP_LINE_OFF (1) | CNA_CONV_CON1_ARGB_IN (8);
   }

   if (operation->depthwise)
      con1 |= CNA_CONV_CON1_CONV_MODE(3);

   EMIT(REG_CNA_CONV_CON1, con1);

   EMIT(REG_DPU_S_POINTER, DPU_S_POINTER_POINTER_PP_MODE (1) | DPU_S_POINTER_EXECUTER_PP_EN (1) | DPU_S_POINTER_POINTER_PP_EN (1));
   EMIT(REG_DPU_RDMA_RDMA_S_POINTER, DPU_RDMA_RDMA_S_POINTER_POINTER_PP_MODE (1) | DPU_RDMA_RDMA_S_POINTER_EXECUTER_PP_EN (1) | DPU_RDMA_RDMA_S_POINTER_POINTER_PP_EN (1));
   EMIT(REG_CNA_CONV_CON1, con1);
   EMIT(REG_CNA_CONV_CON2, CNA_CONV_CON2_FEATURE_GRAINS (50 + task->stride_y + 1)); /* Magic: Seems to pass the most tests */
   EMIT(REG_CNA_CONV_CON3, CNA_CONV_CON3_CONV_X_STRIDE (task->stride_x) |
                           CNA_CONV_CON3_CONV_Y_STRIDE (task->stride_y));
   EMIT(REG_CNA_DATA_SIZE0, CNA_DATA_SIZE0_DATAIN_WIDTH (task->input_width) |
                            CNA_DATA_SIZE0_DATAIN_HEIGHT (task->input_height));

   EMIT(REG_CNA_DATA_SIZE1, CNA_DATA_SIZE1_DATAIN_CHANNEL_REAL (task->input_channels_real - 1) |
                            CNA_DATA_SIZE1_DATAIN_CHANNEL (task->input_channels));

   EMIT(REG_CNA_DATA_SIZE2, CNA_DATA_SIZE2_DATAOUT_WIDTH (task->output_width));
   EMIT(REG_CNA_DATA_SIZE3, CNA_DATA_SIZE3_DATAOUT_ATOMICS (task->atomic_count));
   EMIT(REG_CNA_WEIGHT_SIZE0, task->weights_width * task->weights_height * task->input_channels * task->weights_kernels);
   EMIT(REG_CNA_WEIGHT_SIZE1, task->weights_width * task->weights_height * task->input_channels);
   EMIT(REG_CNA_WEIGHT_SIZE2, CNA_WEIGHT_SIZE2_WEIGHT_WIDTH (task->weights_width) |
                              CNA_WEIGHT_SIZE2_WEIGHT_HEIGHT (task->weights_height) |
                              CNA_WEIGHT_SIZE2_WEIGHT_KERNELS (task->weights_kernels));

   EMIT(REG_CNA_CBUF_CON0, con0);

   EMIT(REG_CNA_CBUF_CON1, CNA_CBUF_CON1_DATA_ENTRIES (task->input_data_entries));

   if (task->input_channels_real == 1) {
      unsigned truncate = 14;
      unsigned scale = 16384;
      unsigned offset = 65408;

      if (operation->addition_input || operation->add_tensor != -1)  {
         truncate = 15;
         scale = 32388;
      }

      EMIT(REG_CNA_CVT_CON0, CNA_CVT_CON0_CVT_TRUNCATE_3(truncate) | CNA_CVT_CON0_CVT_TRUNCATE_2(truncate) | CNA_CVT_CON0_CVT_TRUNCATE_1(truncate) | CNA_CVT_CON0_CVT_TRUNCATE_0(truncate));
      EMIT(REG_CNA_CVT_CON1, CNA_CVT_CON1_CVT_SCALE0(scale) | CNA_CVT_CON1_CVT_OFFSET0(offset));
      EMIT(REG_CNA_CVT_CON2, CNA_CVT_CON2_CVT_SCALE1(scale) | CNA_CVT_CON2_CVT_OFFSET1(offset));
      EMIT(REG_CNA_CVT_CON3, CNA_CVT_CON3_CVT_SCALE2(scale) | CNA_CVT_CON3_CVT_OFFSET2(offset));
      EMIT(REG_CNA_CVT_CON4, CNA_CVT_CON4_CVT_SCALE3(scale) | CNA_CVT_CON4_CVT_OFFSET3(offset));
   } else {
      EMIT(REG_CNA_CVT_CON0, CNA_CVT_CON0_DATA_SIGN(1) | CNA_CVT_CON0_CVT_TYPE(1) | CNA_CVT_CON0_CVT_BYPASS(1));
      EMIT(REG_CNA_CVT_CON1, CNA_CVT_CON1_CVT_SCALE0(1));
      EMIT(REG_CNA_CVT_CON2, CNA_CVT_CON2_CVT_SCALE1(1));
      EMIT(REG_CNA_CVT_CON3, CNA_CVT_CON3_CVT_SCALE2(1));
      EMIT(REG_CNA_CVT_CON4, CNA_CVT_CON4_CVT_SCALE3(1));
   }

   EMIT(REG_CNA_FC_CON0, 0);
   EMIT(REG_CNA_FC_CON1, 0);
   EMIT(REG_CNA_PAD_CON0, CNA_PAD_CON0_PAD_LEFT(task->pad_left) | CNA_PAD_CON0_PAD_TOP(task->pad_top));
   EMIT(REG_CNA_FEATURE_DATA_ADDR, get_tensor(subgraph, operation->input_index)->phys_addr + task->input_offset);
   EMIT(REG_CNA_FC_CON2, 0);
   EMIT(REG_CNA_DMA_CON0, CNA_DMA_CON0_WEIGHT_BURST_LEN (15) | CNA_DMA_CON0_DATA_BURST_LEN (15));
   EMIT(REG_CNA_DMA_CON1, CNA_DMA_CON1_LINE_STRIDE (task->input_line_stride));
   EMIT(REG_CNA_DMA_CON2, CNA_DMA_CON2_SURF_STRIDE (task->input_surface_stride));

   EMIT(REG_CNA_FC_DATA_SIZE0, CNA_FC_DATA_SIZE0_DMA_WIDTH (operation->input_width) | CNA_FC_DATA_SIZE0_DMA_HEIGHT (task->input_height));

   EMIT(REG_CNA_FC_DATA_SIZE1, CNA_FC_DATA_SIZE1_DMA_CHANNEL (task->input_channels));
   EMIT(REG_CNA_DCOMP_CTRL, 0);
   EMIT(REG_CNA_DCOMP_REGNUM, 0);
   EMIT(REG_CNA_DCOMP_ADDR0, rkt_resource(operation->weights)->phys_addr);
   EMIT(REG_CNA_DCOMP_AMOUNT0, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT1, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT2, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT3, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT4, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT5, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT6, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT7, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT8, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT9, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT10, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT11, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT12, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT13, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT14, 0);
   EMIT(REG_CNA_DCOMP_AMOUNT15, 0);

   if (task->input_channels_real == 1) {
      EMIT(REG_CNA_CVT_CON5, 65535);
   } else {
      EMIT(REG_CNA_CVT_CON5, 0);
   }

   int32_t pad_con1;
   if (task->weights_width >= 3 &&
       task->input_zero_point == 0x0)
      pad_con1 = 0xffff8080;
   else
      pad_con1 = task->input_zero_point - 0x80;

   if (operation->addition_input || operation->add_tensor != -1)
      pad_con1 = 0xffffff80;

   if (operation->depthwise && task->input_zero_point == 0x8b)
      pad_con1 = 0x0b0b;

   EMIT(REG_CNA_PAD_CON1, pad_con1);

   uint32_t misc_cfg = CORE_MISC_CFG_QD_EN (1);
   if (operation->depthwise)
      misc_cfg |= CORE_MISC_CFG_DW_EN (1);

   EMIT(REG_CORE_MISC_CFG, misc_cfg);
   EMIT(REG_CORE_DATAOUT_SIZE_0, CORE_DATAOUT_SIZE_0_DATAOUT_HEIGHT (task->output_height - 1) | CORE_DATAOUT_SIZE_0_DATAOUT_WIDTH (task->output_width - 1));
   EMIT(REG_CORE_DATAOUT_SIZE_1, CORE_DATAOUT_SIZE_1_DATAOUT_CHANNEL (task->output_channels - 1));
   EMIT(REG_CORE_CLIP_TRUNCATE, CORE_CLIP_TRUNCATE_CLIP_TRUNCATE(operation->truncate_bits));
   emit_raw(regs, CORE | 0x1, 0x3030, 0);

   uint32_t feat_mode_cfg = DPU_FEATURE_MODE_CFG_BURST_LEN (15) | DPU_FEATURE_MODE_CFG_OUTPUT_MODE (2);
   if (operation->depthwise)
      feat_mode_cfg |= DPU_FEATURE_MODE_CFG_CONV_MODE(3);

   EMIT(REG_DPU_FEATURE_MODE_CFG, feat_mode_cfg);
   EMIT(REG_DPU_DATA_FORMAT, 0);
   EMIT(REG_DPU_OFFSET_PEND, 0);
   EMIT(REG_DPU_DST_BASE_ADDR, get_tensor(subgraph, operation->output_index)->phys_addr + task->output_offset);
   EMIT(REG_DPU_DST_SURF_STRIDE, DPU_DST_SURF_STRIDE_DST_SURF_STRIDE (task->output_surface_stride));
   EMIT(REG_DPU_DATA_CUBE_WIDTH, DPU_DATA_CUBE_WIDTH_WIDTH (task->output_width - 1));
   EMIT(REG_DPU_DATA_CUBE_HEIGHT, DPU_DATA_CUBE_HEIGHT_HEIGHT (task->output_height - 1));
   EMIT(REG_DPU_DATA_CUBE_NOTCH_ADDR, 0);
   EMIT(REG_DPU_DATA_CUBE_CHANNEL, DPU_DATA_CUBE_CHANNEL_ORIG_CHANNEL (task->output_channels_real - 1) | DPU_DATA_CUBE_CHANNEL_CHANNEL (task->output_channels - 1));
   EMIT(REG_DPU_BS_CFG, DPU_BS_CFG_BS_ALU_ALGO (2) | DPU_BS_CFG_BS_ALU_SRC (1) | DPU_BS_CFG_BS_RELU_BYPASS (1) | DPU_BS_CFG_BS_MUL_BYPASS (1));
   EMIT(REG_DPU_BS_ALU_CFG, 0);
   EMIT(REG_DPU_BS_MUL_CFG, 0);
   EMIT(REG_DPU_BS_RELUX_CMP_VALUE, 0);

   if (operation->depthwise) {
      EMIT(REG_DPU_BS_OW_CFG, DPU_BS_OW_CFG_SIZE_E_2 (3) | DPU_BS_OW_CFG_SIZE_E_1 (3) | DPU_BS_OW_CFG_SIZE_E_0 (3));
   } else {
      EMIT(REG_DPU_BS_OW_CFG, DPU_BS_OW_CFG_SIZE_E_2 (1) | DPU_BS_OW_CFG_SIZE_E_1 (1) | DPU_BS_OW_CFG_SIZE_E_0 (1));
   }

   EMIT(REG_DPU_BS_OW_OP, DPU_BS_OW_OP_OW_OP (0x80 - weights_zero_point));

   EMIT(REG_DPU_WDMA_SIZE_0, DPU_WDMA_SIZE_0_CHANNEL_WDMA (task->output_channels - 1));
   EMIT(REG_DPU_WDMA_SIZE_1, DPU_WDMA_SIZE_1_HEIGHT_WDMA (task->output_height - 1) | DPU_WDMA_SIZE_1_WIDTH_WDMA (task->output_width - 1));
   EMIT(REG_DPU_BN_CFG, DPU_BN_CFG_BN_RELU_BYPASS (1) | DPU_BN_CFG_BN_MUL_BYPASS (1) | DPU_BN_CFG_BN_ALU_BYPASS (1) | DPU_BN_CFG_BN_BYPASS (1));
   EMIT(REG_DPU_BN_ALU_CFG, 0);
   EMIT(REG_DPU_BN_MUL_CFG, 0);
   EMIT(REG_DPU_BN_RELUX_CMP_VALUE, 0);

   if (operation->add_tensor != -1) {
      EMIT(REG_DPU_EW_CFG, DPU_EW_CFG_EW_CVT_TYPE(1) | DPU_EW_CFG_EW_DATA_MODE(1) | DPU_EW_CFG_EDATA_SIZE(1) | DPU_EW_CFG_EW_ALU_ALGO(2) | DPU_EW_CFG_EW_RELU_BYPASS(1) | DPU_EW_CFG_EW_LUT_BYPASS(1) | DPU_EW_CFG_EW_OP_SRC(1));

      /* See http://nvdla.org/hw/v1/ias/precision.html#element-wise */
      EMIT(REG_DPU_EW_CVT_OFFSET_VALUE, operation->addition_offset);

      float add_scale = 0.0;
      //fprintf(stderr, "operation->addition_scale %f %d\n", operation->addition_scale, operation->addition_scale - 0.090192 < 0.0000001);
      if (fabs(operation->addition_scale - 0.090192) < 0.00001) {
         add_scale = 299.671889248;
      } else if (fabs(operation->addition_scale - 0.399250) < 0.00001) {
         add_scale = 1326.499209406;
      } else if (fabs(operation->addition_scale - 0.364902) < 0.00001) {
         add_scale = 780.34375;
      } else if (fabs(operation->addition_scale - 0.422037) < 0.00001) {
         add_scale = 715.5625;
      } else if (fabs(operation->addition_scale - 0.213016) < 0.00001) {
         add_scale = 564.6875;
      } else if (fabs(operation->addition_scale - 0.244231) < 0.00001) {
         add_scale = 499.796875;
      } else if (fabs(operation->addition_scale - 0.283416) < 0.00001) {
         add_scale = 488.203125;
      } else if (fabs(operation->addition_scale - 0.171151) < 0.00001) {
         add_scale = 602.90625;
      } else if (fabs(operation->addition_scale - 0.164588) < 0.00001) {
         add_scale = 271.921875;
      } else if (fabs(operation->addition_scale - 0.204098) < 0.00001) {
         add_scale = 262.90625;
      } else if (fabs(operation->addition_scale - 0.116532) < 0.00001) {
         add_scale = 450.140625;
      } else if (fabs(operation->addition_scale - 0.134499) < 0.00001) {
         add_scale = 212.1953125;
      } else if (fabs(operation->addition_scale - 0.220141) < 0.00001) {
         add_scale = 368.28125;
      } else if (fabs(operation->addition_scale - 0.094560) < 0.00001) {
         add_scale = 416.421875;
      } else if (fabs(operation->addition_scale - 0.093230) < 0.00001) {
         add_scale = 305.421875;
      } else if (fabs(operation->addition_scale - 0.100618) < 0.00001) {
         add_scale = 313.671875;
      } else {
         add_scale = 0.0;
      }

      //fprintf(stderr, "add_scale %f\n", 1.0 / add_scale);
      uint32_t add_scale_bits = fui(add_scale);
      /* Taken from https://github.com/pytorch/QNNPACK/blob/master/src/qnnpack/requantization.h#L130 */
      unsigned add_shift = 127 + 31 - 32 - (add_scale_bits >> 23) + 16;

      unsigned scale = ((add_scale_bits >> 9) & 0x7fff);
      if (scale < 1 << 14)
         scale |= 1 << 14;

      EMIT(REG_DPU_EW_CVT_SCALE_VALUE, DPU_EW_CVT_SCALE_VALUE_EW_OP_CVT_SHIFT(add_shift - 1) |
                                       DPU_EW_CVT_SCALE_VALUE_EW_OP_CVT_SCALE(scale));

      EMIT(REG_DPU_EW_RELUX_CMP_VALUE, 0x0);

      if (fabs(operation->addition_scale - 0.213016) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0x4);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(25914));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT (24));
      } else if (fabs(operation->addition_scale - 0.244231) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0x1);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(28927));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(24));
      } else if (fabs(operation->addition_scale - 0.283416) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0x6);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(26050));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(24));
      } else if (fabs(operation->addition_scale - 0.171151) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0xfffffffd);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(28937));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(24));
      } else if (fabs(operation->addition_scale - 0.164588) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0x1);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(24877));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(23));
      } else if (fabs(operation->addition_scale - 0.204098) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0x0);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(23272));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(23));
      } else if (fabs(operation->addition_scale - 0.116532) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0xfffffff8);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(32292));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(24));
      } else if (fabs(operation->addition_scale - 0.134499) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0xfffffffb);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(24153));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(23));
      } else if (fabs(operation->addition_scale - 0.220141) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0xb);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(27655));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(24));
      } else if (fabs(operation->addition_scale - 0.094560) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0x5);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(20432));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(23));
      } else if (fabs(operation->addition_scale - 0.093230) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0xffffffff);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(25449));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(23));
      } else if (fabs(operation->addition_scale - 0.100618) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, offset);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(16874));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(23));
      } else if (fabs(operation->addition_scale - 0.422037) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0x1);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(22559));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(24));
      } else if (fabs(operation->addition_scale - 0.364902) < 0.00001) {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0x4);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(18589));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(24));
      } else {
         EMIT(REG_DPU_OUT_CVT_OFFSET, 0x6);
         EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(27676));
         EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT (25));
      }
   } else {
      EMIT(REG_DPU_EW_CFG, DPU_EW_CFG_EW_RELU_BYPASS (1) | DPU_EW_CFG_EW_OP_CVT_BYPASS (1) | DPU_EW_CFG_EW_LUT_BYPASS (1) | DPU_EW_CFG_EW_OP_BYPASS (1) | DPU_EW_CFG_EW_BYPASS (1));
      EMIT(REG_DPU_EW_CVT_OFFSET_VALUE, 0);
      EMIT(REG_DPU_EW_CVT_SCALE_VALUE, DPU_EW_CVT_SCALE_VALUE_EW_OP_CVT_SCALE(1));
      EMIT(REG_DPU_EW_RELUX_CMP_VALUE, 0);
      EMIT(REG_DPU_OUT_CVT_OFFSET, offset);

      float conv_scale = (task->input_scale * task->weights_scale) / task->output_scale;
      //fprintf(stderr, "conv_scale %f\n", conv_scale);
      uint32_t scale_bits = fui(conv_scale);
      /* Taken from https://github.com/pytorch/QNNPACK/blob/master/src/qnnpack/requantization.h#L130 */
      unsigned shift = 127 + 31 - 32 - (scale_bits >> 23) + 16;

      if (operation->truncate_bits > 0)
         shift--;

      unsigned scale = ((scale_bits >> 9) & 0x7fff) + 1;
      if (scale < 1 << 14)
         scale |= 1 << 14;

      EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE (scale));
      EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT (shift - 1));
   }

   EMIT(REG_DPU_EW_OP_VALUE_0, 0);
   EMIT(REG_DPU_EW_OP_VALUE_1, 0);
   EMIT(REG_DPU_EW_OP_VALUE_2, 0);
   EMIT(REG_DPU_EW_OP_VALUE_3, 0);
   EMIT(REG_DPU_EW_OP_VALUE_4, 0);
   EMIT(REG_DPU_EW_OP_VALUE_5, 0);
   EMIT(REG_DPU_EW_OP_VALUE_6, 0);
   EMIT(REG_DPU_EW_OP_VALUE_7, 0);
   EMIT(REG_DPU_SURFACE_ADD, DPU_SURFACE_ADD_SURF_ADD (task->surfaces_per_row));
   emit_raw(regs, DPU | 0x1, 0x40c4, 0);
   EMIT(REG_DPU_LUT_ACCESS_CFG, 0);
   EMIT(REG_DPU_LUT_ACCESS_DATA, 0);
   EMIT(REG_DPU_LUT_CFG, 0);
   EMIT(REG_DPU_LUT_INFO, 0);
   EMIT(REG_DPU_LUT_LE_START, 0);
   EMIT(REG_DPU_LUT_LE_END, 0);
   EMIT(REG_DPU_LUT_LO_START, 0);
   EMIT(REG_DPU_LUT_LO_END, 0);
   EMIT(REG_DPU_LUT_LE_SLOPE_SCALE, 0);
   EMIT(REG_DPU_LUT_LE_SLOPE_SHIFT, 0);
   EMIT(REG_DPU_LUT_LO_SLOPE_SCALE, 0);
   EMIT(REG_DPU_LUT_LO_SLOPE_SHIFT, 0);
   EMIT(REG_DPU_RDMA_RDMA_DATA_CUBE_WIDTH, DPU_RDMA_RDMA_DATA_CUBE_WIDTH_WIDTH (task->output_width - 1));
   EMIT(REG_DPU_RDMA_RDMA_DATA_CUBE_HEIGHT, DPU_RDMA_RDMA_DATA_CUBE_HEIGHT_HEIGHT (task->output_height - 1));
   EMIT(REG_DPU_RDMA_RDMA_DATA_CUBE_CHANNEL, DPU_RDMA_RDMA_DATA_CUBE_CHANNEL_CHANNEL (task->output_channels - 1));

   if (operation->add_tensor != -1) {
      EMIT(REG_DPU_RDMA_RDMA_SRC_BASE_ADDR, get_tensor(subgraph, operation->add_tensor)->phys_addr + task->output_offset);
   } else {
      EMIT(REG_DPU_RDMA_RDMA_SRC_BASE_ADDR, 0);
   }

   EMIT(REG_DPU_RDMA_RDMA_BRDMA_CFG, DPU_RDMA_RDMA_BRDMA_CFG_BRDMA_DATA_USE (1));
   EMIT(REG_DPU_RDMA_RDMA_BS_BASE_ADDR, rkt_resource(operation->biases)->phys_addr);
   EMIT(REG_DPU_RDMA_RDMA_NRDMA_CFG, 0);
   EMIT(REG_DPU_RDMA_RDMA_BN_BASE_ADDR, 0);

   unsigned ew_stride = MAX2(operation->output_width * operation->output_height, 12);

   if (operation->add_tensor != -1) {
      EMIT(REG_DPU_RDMA_RDMA_ERDMA_CFG, DPU_RDMA_RDMA_ERDMA_CFG_ERDMA_DATA_MODE(1) | DPU_RDMA_RDMA_ERDMA_CFG_ERDMA_DATA_SIZE(1));
      unsigned ew_base_offset = operation->output_width * operation->output_height * ATOMIC_K_SIZE;
      EMIT(REG_DPU_RDMA_RDMA_EW_BASE_ADDR, get_tensor(subgraph, operation->add_tensor)->phys_addr + task->output_offset + ew_base_offset);
      EMIT(REG_DPU_RDMA_RDMA_EW_SURF_STRIDE, DPU_RDMA_RDMA_EW_SURF_STRIDE_EW_SURF_STRIDE(ew_stride));
   } else {
      EMIT(REG_DPU_RDMA_RDMA_ERDMA_CFG, DPU_RDMA_RDMA_ERDMA_CFG_ERDMA_DISABLE (1));
      EMIT(REG_DPU_RDMA_RDMA_EW_BASE_ADDR, 0);
      EMIT(REG_DPU_RDMA_RDMA_EW_SURF_STRIDE, 0);
   }

   uint32_t rdma_feat_mode_cfg = 0x0;

   if (operation->add_tensor != -1) {
      rdma_feat_mode_cfg |= DPU_RDMA_RDMA_FEATURE_MODE_CFG_BURST_LEN (15) | DPU_RDMA_RDMA_FEATURE_MODE_CFG_COMB_USE(5);
   } else {
      rdma_feat_mode_cfg |= DPU_RDMA_RDMA_FEATURE_MODE_CFG_BURST_LEN (15) | DPU_RDMA_RDMA_FEATURE_MODE_CFG_MRDMA_DISABLE (1);
   }

   if (operation->depthwise)
      rdma_feat_mode_cfg |= DPU_RDMA_RDMA_FEATURE_MODE_CFG_CONV_MODE(3);

   EMIT(REG_DPU_RDMA_RDMA_FEATURE_MODE_CFG, rdma_feat_mode_cfg);
   EMIT(REG_DPU_RDMA_RDMA_SRC_DMA_CFG, 0);

   unsigned surf_notch = ew_stride + task->output_width * (operation->output_height - task->output_height);

   if (operation->input_width == 3) {
      surf_notch = 15;
   }

   if (operation->add_tensor != -1) {
      EMIT(REG_DPU_RDMA_RDMA_SURF_NOTCH, DPU_RDMA_RDMA_SURF_NOTCH_SURF_NOTCH_ADDR(surf_notch));
   } else {
      EMIT(REG_DPU_RDMA_RDMA_SURF_NOTCH, 0);
   }

   EMIT(REG_DPU_RDMA_RDMA_PAD_CFG, 0);
   EMIT(REG_DPU_RDMA_RDMA_WEIGHT, DPU_RDMA_RDMA_WEIGHT_E_WEIGHT (1) | DPU_RDMA_RDMA_WEIGHT_N_WEIGHT (1) | DPU_RDMA_RDMA_WEIGHT_B_WEIGHT (1) | DPU_RDMA_RDMA_WEIGHT_M_WEIGHT (1));

   if (operation->add_tensor != -1) {
      EMIT(REG_DPU_RDMA_RDMA_EW_SURF_NOTCH, DPU_RDMA_RDMA_EW_SURF_NOTCH_EW_SURF_NOTCH(surf_notch));
   } else {
      EMIT(REG_DPU_RDMA_RDMA_EW_SURF_NOTCH, 0x0);
   }

   if (num_tasks == 1)
      util_dynarray_append(regs, uint64_t, 0x0);
   else
      EMIT(REG_PC_BASE_ADDRESS, 0);

   EMIT(REG_PC_REGISTER_AMOUNTS, 0);

   /* TRM: before op_en, 64'h0041_xxxx_xxxx_xxxx must be set. */
   util_dynarray_append(regs, uint64_t, 0x0041000000000000);

   /* TRM: 64'h0081_0000_007f_0008 will set each block's op_en(CNA, CORE, ..., PPU_RDMA). */
   emit_raw(regs, 0x81, REG_PC_OPERATION_ENABLE, PC_OPERATION_ENABLE_RESERVED_0(14) | PC_OPERATION_ENABLE_OP_EN (1));
}

static void
fill_middle_regcmd(struct rkt_ml_subgraph *subgraph, const struct rkt_operation *operation, struct util_dynarray *regs, unsigned task_num)
{
   struct pipe_context *pcontext = subgraph->base.context;
   struct split_task *task = util_dynarray_element(&operation->tasks, struct split_task, task_num);
   unsigned scale = 21284;
   unsigned shift = 14;
   unsigned offset = 0xFFFFFF80; /* -128 */
   unsigned num_tasks = util_dynarray_num_elements(&operation->tasks, struct split_task);

   uint32_t con0 = CNA_CBUF_CON0_WEIGHT_BANK (task->weights_banks) |
                   CNA_CBUF_CON0_DATA_BANK (task->input_banks);
   if (task_num > 0 && operation->reuse_weights_cbuf)
      con0 |= CNA_CBUF_CON0_WEIGHT_REUSE(1);

   EMIT(REG_CNA_CBUF_CON0, con0);

   EMIT(REG_CNA_DCOMP_REGNUM, 0);
   EMIT(REG_CNA_DCOMP_CTRL, 0);

   uint32_t con1 = 0x0;
   if (task->input_channels_real == 1) {
      con1 |= CNA_CONV_CON1_NONALIGN_DMA (1) | CNA_CONV_CON1_GROUP_LINE_OFF (1) | CNA_CONV_CON1_ARGB_IN (8);
   }

   if (operation->depthwise)
      con1 |= CNA_CONV_CON1_CONV_MODE(3);

   EMIT(REG_CNA_CONV_CON1, con1);

   EMIT(REG_DPU_S_POINTER, DPU_S_POINTER_POINTER_PP_MODE (1) | DPU_S_POINTER_EXECUTER_PP_EN (1) | DPU_S_POINTER_POINTER_PP_EN (1));
   EMIT(REG_DPU_RDMA_RDMA_S_POINTER, DPU_RDMA_RDMA_S_POINTER_POINTER_PP_MODE (1) | DPU_RDMA_RDMA_S_POINTER_EXECUTER_PP_EN (1) | DPU_RDMA_RDMA_S_POINTER_POINTER_PP_EN (1));

   EMIT(REG_CNA_CBUF_CON0, con0);

   EMIT(REG_CNA_FEATURE_DATA_ADDR, get_tensor(subgraph, operation->input_index)->phys_addr + task->input_offset);

   if (task->output_channels_real == 32 && task->input_width == 16) {
      EMIT(REG_CNA_FC_DATA_SIZE0, CNA_FC_DATA_SIZE0_DMA_WIDTH (8) | CNA_FC_DATA_SIZE0_DMA_HEIGHT (task->input_height));
   } else {
      EMIT(REG_CNA_FC_DATA_SIZE0, CNA_FC_DATA_SIZE0_DMA_WIDTH (task->input_width) | CNA_FC_DATA_SIZE0_DMA_HEIGHT (task->input_height));
   }

   EMIT(REG_CNA_FC_DATA_SIZE1, CNA_FC_DATA_SIZE1_DMA_CHANNEL (task->input_channels));
   EMIT(REG_CNA_DCOMP_ADDR0, rkt_resource(operation->weights)->phys_addr);

   EMIT(REG_DPU_DST_BASE_ADDR, get_tensor(subgraph, operation->output_index)->phys_addr + task->output_offset);

   EMIT(REG_DPU_WDMA_SIZE_0, DPU_WDMA_SIZE_0_CHANNEL_WDMA (task->output_channels - 1));
   EMIT(REG_DPU_WDMA_SIZE_1, DPU_WDMA_SIZE_1_HEIGHT_WDMA (task->output_height - 1) | DPU_WDMA_SIZE_1_WIDTH_WDMA (task->output_width - 1));
   EMIT(REG_DPU_RDMA_RDMA_SRC_BASE_ADDR, 0);
   EMIT(REG_DPU_RDMA_RDMA_BS_BASE_ADDR, rkt_resource(operation->biases)->phys_addr);
   EMIT(REG_DPU_RDMA_RDMA_BN_BASE_ADDR, 0);
   EMIT(REG_DPU_RDMA_RDMA_EW_BASE_ADDR, 0);

   EMIT(REG_PC_BASE_ADDRESS, 0);
   EMIT(REG_PC_REGISTER_AMOUNTS, 0);

   /* TRM: before op_en, 64'h0041_xxxx_xxxx_xxxx must be set. */
   util_dynarray_append(regs, uint64_t, 0x0041000000000000);

   /* TRM: 64'h0081_0000_007f_0008 will set each block's op_en(CNA, CORE, ..., PPU_RDMA). */
   emit_raw(regs, 0x81, REG_PC_OPERATION_ENABLE, PC_OPERATION_ENABLE_RESERVED_0(14) | PC_OPERATION_ENABLE_OP_EN (1));
}

static void
fill_last_regcmd(struct rkt_ml_subgraph *subgraph, const struct rkt_operation *operation, struct util_dynarray *regs, unsigned task_num)
{
   struct pipe_context *pcontext = subgraph->base.context;
   struct split_task *task = util_dynarray_element(&operation->tasks, struct split_task, task_num);
   unsigned scale = 21284;
   unsigned shift = 14;
   unsigned offset = 0xFFFFFF80; /* -128 */
   unsigned num_tasks = util_dynarray_num_elements(&operation->tasks, struct split_task);

   uint32_t con0 = CNA_CBUF_CON0_WEIGHT_BANK (task->weights_banks) |
                   CNA_CBUF_CON0_DATA_BANK (task->input_banks);
   if (task_num > 0 && operation->reuse_weights_cbuf)
      con0 |= CNA_CBUF_CON0_WEIGHT_REUSE(1);

   EMIT(REG_CNA_CBUF_CON0, con0);

   EMIT(REG_CNA_DCOMP_REGNUM, 0);
   EMIT(REG_CNA_DCOMP_CTRL, 0);

   uint32_t con1 = 0x0;
   if (task->input_channels_real == 1) {
      con1 |= CNA_CONV_CON1_NONALIGN_DMA (1) | CNA_CONV_CON1_GROUP_LINE_OFF (1) | CNA_CONV_CON1_ARGB_IN (8);
   }

   if (operation->depthwise)
      con1 |= CNA_CONV_CON1_CONV_MODE(3);

   EMIT(REG_CNA_CONV_CON1, con1);

   EMIT(REG_DPU_S_POINTER, DPU_S_POINTER_POINTER_PP_MODE (1) | DPU_S_POINTER_EXECUTER_PP_EN (1) | DPU_S_POINTER_POINTER_PP_EN (1));
   EMIT(REG_DPU_RDMA_RDMA_S_POINTER, DPU_RDMA_RDMA_S_POINTER_POINTER_PP_MODE (1) | DPU_RDMA_RDMA_S_POINTER_EXECUTER_PP_EN (1) | DPU_RDMA_RDMA_S_POINTER_POINTER_PP_EN (1));

   EMIT(REG_CNA_DATA_SIZE0, CNA_DATA_SIZE0_DATAIN_WIDTH (task->input_width) |
                            CNA_DATA_SIZE0_DATAIN_HEIGHT (task->input_height));
   EMIT(REG_CNA_DATA_SIZE3, CNA_DATA_SIZE3_DATAOUT_ATOMICS (task->atomic_count));

   EMIT(REG_CNA_CBUF_CON0, con0);

   EMIT(REG_CNA_FEATURE_DATA_ADDR, get_tensor(subgraph, operation->input_index)->phys_addr + task->input_offset);

   if (task->output_channels_real == 32 && task->input_width == 16) {
      EMIT(REG_CNA_FC_DATA_SIZE0, CNA_FC_DATA_SIZE0_DMA_WIDTH (8) | CNA_FC_DATA_SIZE0_DMA_HEIGHT (task->input_height));
   } else {
      EMIT(REG_CNA_FC_DATA_SIZE0, CNA_FC_DATA_SIZE0_DMA_WIDTH (task->input_width) | CNA_FC_DATA_SIZE0_DMA_HEIGHT (task->input_height));
   }

   EMIT(REG_CNA_FC_DATA_SIZE1, CNA_FC_DATA_SIZE1_DMA_CHANNEL (task->input_channels));

   EMIT(REG_CNA_DCOMP_ADDR0, rkt_resource(operation->weights)->phys_addr);

   EMIT(REG_CORE_DATAOUT_SIZE_0, CORE_DATAOUT_SIZE_0_DATAOUT_HEIGHT (task->output_height - 1) | CORE_DATAOUT_SIZE_0_DATAOUT_WIDTH (task->output_width - 1));
   EMIT(REG_DPU_DST_BASE_ADDR, get_tensor(subgraph, operation->output_index)->phys_addr + task->output_offset);
   EMIT(REG_DPU_DATA_CUBE_HEIGHT, DPU_DATA_CUBE_HEIGHT_HEIGHT (task->output_height - 1));
   EMIT(REG_DPU_WDMA_SIZE_0, DPU_WDMA_SIZE_0_CHANNEL_WDMA (task->output_channels - 1));
   EMIT(REG_DPU_WDMA_SIZE_1, DPU_WDMA_SIZE_1_HEIGHT_WDMA (task->output_height - 1) | DPU_WDMA_SIZE_1_WIDTH_WDMA (task->output_width - 1));
   EMIT(REG_DPU_RDMA_RDMA_DATA_CUBE_HEIGHT, DPU_RDMA_RDMA_DATA_CUBE_HEIGHT_HEIGHT (task->output_height - 1));
   EMIT(REG_DPU_RDMA_RDMA_SRC_BASE_ADDR, 0);
   EMIT(REG_DPU_RDMA_RDMA_BS_BASE_ADDR, rkt_resource(operation->biases)->phys_addr);
   EMIT(REG_DPU_RDMA_RDMA_BN_BASE_ADDR, 0);
   EMIT(REG_DPU_RDMA_RDMA_EW_BASE_ADDR, 0);

   util_dynarray_append(regs, uint64_t, 0x0);
   EMIT(REG_PC_REGISTER_AMOUNTS, 0);

   /* TRM: before op_en, 64'h0041_xxxx_xxxx_xxxx must be set. */
   util_dynarray_append(regs, uint64_t, 0x0041000000000000);

   /* TRM: 64'h0081_0000_007f_0008 will set each block's op_en(CNA, CORE, ..., PPU_RDMA). */
   emit_raw(regs, 0x81, REG_PC_OPERATION_ENABLE, PC_OPERATION_ENABLE_RESERVED_0(14) | PC_OPERATION_ENABLE_OP_EN (1));
}

static void
fill_regcmd(struct rkt_ml_subgraph *subgraph, const struct rkt_operation *operation, struct util_dynarray *regs, unsigned task_num)
{
   unsigned num_tasks = util_dynarray_num_elements(&operation->tasks, struct split_task);

   /* TODO: There are some problems when using smaller regcmd buffers, for now set all registers always */
   fill_first_regcmd(subgraph, operation, regs, task_num);
   return;

   if (task_num < 2)
      fill_first_regcmd(subgraph, operation, regs, task_num);
   else if (task_num == num_tasks - 1)
      fill_last_regcmd(subgraph, operation, regs, task_num);
   else
      fill_middle_regcmd(subgraph, operation, regs, task_num);
}

static struct pipe_resource *
fill_weights(struct rkt_ml_subgraph *subgraph, const struct pipe_ml_operation *poperation)
{
   struct pipe_context *pcontext = subgraph->base.context;
   unsigned weights_width = poperation->conv.weight_tensor->dims[1];
   unsigned weights_height = poperation->conv.weight_tensor->dims[2];
   unsigned input_channels = poperation->input_tensors[0]->dims[3];
   unsigned input_channels_real = poperation->input_tensors[0]->dims[3];
   unsigned output_channels = poperation->output_tensors[0]->dims[3];
   unsigned output_channels_real = poperation->output_tensors[0]->dims[3];
   unsigned weights_size;
   uint8_t zero_point = poperation->conv.weight_tensor->zero_point;
   struct pipe_transfer *transfer_in,  *transfer_out;
   void *map = pipe_buffer_map(pcontext, poperation->conv.weight_tensor->resource, PIPE_MAP_READ, &transfer_in);
   uint8_t (*weights_in)[weights_width][weights_height][input_channels] = map;
   struct pipe_resource *rsc;
   uint8_t *weights_out;

   input_channels = MAX2(input_channels, FEATURE_ATOMIC_SIZE);

   output_channels = ALIGN(output_channels, 2);
   if (is_depthwise(poperation))
      output_channels = 1;

   weights_size = weights_width * weights_height * output_channels * ALIGN(input_channels, WEIGHT_ATOMIC_SIZE) * 2;

   rsc = pipe_buffer_create(pcontext->screen, 0, PIPE_USAGE_DEFAULT, weights_size);
   weights_out = pipe_buffer_map(pcontext, rsc, PIPE_MAP_WRITE, &transfer_out);

#if 0
   for (int oc = 0; oc < output_channels; oc++) {
      for (int x = 0; x < weights_width; x++) {
         for (int y = 0; y < weights_height; y++) {
            for (int ic = 0; ic < input_channels; ic++) {
               if (oc >= output_channels_real || ic >= input_channels_real)
                  fprintf(stderr, "NA ");
               else
                  fprintf(stderr, "%02x ", (uint8_t) (weights_in[oc][x][y][ic] - 0x80));
            }
            fprintf(stderr, "\n");
         }
      }
      fprintf(stderr, "\n");
   }
#endif

   unsigned input_channel_groups = WEIGHT_ATOMIC_SIZE;
   if (is_depthwise(poperation))
      input_channel_groups *= 2;

   unsigned input_channels_1 = DIV_ROUND_UP(input_channels, input_channel_groups);
   unsigned input_channels_2 = MIN2(input_channels, input_channel_groups);

   //fprintf(stderr, "output_channels %d input_channels %d output_channels_real %d\n", output_channels, input_channels, output_channels_real);
   //fprintf(stderr, "oc1 %d ic1 %d oc2 %d ic2 %d\n", DIV_ROUND_UP(output_channels, WEIGHT_ATOMIC_SIZE), input_channels_1, MIN2(output_channels, WEIGHT_ATOMIC_SIZE), input_channels_2);

   unsigned n = 0;
   for (int oc1 = 0; oc1 < DIV_ROUND_UP(output_channels, WEIGHT_ATOMIC_SIZE); oc1++) {
      for (int ic1 = 0; ic1 < input_channels_1; ic1++) {
         for (int x = 0; x < weights_width; x++) {
            for (int y = 0; y < weights_height; y++) {
               for (int oc2 = 0; oc2 < MIN2(output_channels, WEIGHT_ATOMIC_SIZE); oc2++) {
                  for (int ic2 = 0; ic2 < input_channels_2; ic2++) {
                     unsigned oc = oc1 * WEIGHT_ATOMIC_SIZE + oc2;
                     unsigned ic = ic1 * input_channel_groups + ic2;
                     if (output_channels_real > 2 && oc >= ALIGN(output_channels_real, 2))
                        continue;
                     //fprintf(stderr, "n %d x %d y %d oc %d ic %d %02x\n", n, x, y, oc, ic, (uint8_t)(weights_in[oc][ic][x][y] - 0x80));
                     if (oc >= output_channels_real)
                        weights_out[n++] = 0x0;
                     else if (ic >= input_channels_real) {
                        //fprintf(stderr, "ic2 %d ic %d input_channels_real %d input_channel_groups %d input_channels_1 %d input_channels_2 %d\n", ic2, ic, input_channels_real, input_channel_groups, input_channels_1, input_channels_2);
                        if (ic2 < 16 || (input_channels_real % 32) > 16)
                           weights_out[n++] = zero_point - 0x80; /* TODO: Why is the blob converting to signed? It should be unsigned. */
                     } else
                        weights_out[n++] = weights_in[oc][x][y][ic] - 0x80; /* TODO: Why is the blob converting to signed? It should be unsigned. */
                  }
               }
            }
         }
      }
   }

   if (1) {
      static int task = 0;
      dump_buffer(weights_out, "weights", 0, task++, 0, weights_size);
   }

   pipe_buffer_unmap(pcontext, transfer_out);

   pipe_buffer_unmap(pcontext, transfer_in);

   return rsc;
}

static int32_t calculate_bias_correction(struct rkt_ml_subgraph *subgraph, const struct pipe_ml_operation *poperation, unsigned oc, void *map)
{
   struct pipe_context *pcontext = subgraph->base.context;
   unsigned input_channels = poperation->input_tensors[0]->dims[3];
   unsigned input_zero_point = poperation->input_tensors[0]->zero_point;
   unsigned output_channels = poperation->output_tensors[0]->dims[3];
   unsigned weights_width = poperation->conv.weight_tensor->dims[1];
   unsigned weights_height = poperation->conv.weight_tensor->dims[2];
   unsigned weight_zero_point = poperation->conv.weight_tensor->zero_point;
   uint8_t (*weights)[weights_width][weights_height][input_channels] = map;

   int32_t correction = 0;
   if (is_depthwise(poperation)) {
      for (unsigned x = 0; x < weights_width; x++) {
         for (unsigned y = 0; y < weights_height; y++) {
            correction += (weights[0][x][y][oc] - weight_zero_point) * (input_zero_point - 0x80);
         }
      }
   } else {
      for (unsigned x = 0; x < weights_width; x++) {
         for (unsigned y = 0; y < weights_height; y++) {
            for (unsigned ic = 0; ic < input_channels; ic++) {
               correction += (weights[oc][x][y][ic] - weight_zero_point) * (input_zero_point - 0x80);
            }
         }
      }
   }

   return correction;
}

static struct pipe_resource *
fill_biases(struct rkt_ml_subgraph *subgraph, const struct pipe_ml_operation *poperation, unsigned *truncate_bits)
{
   struct pipe_context *pcontext = subgraph->base.context;
   unsigned input_channels = poperation->input_tensors[0]->dims[3];
   unsigned output_channels = poperation->output_tensors[0]->dims[3];
   unsigned weights_size = poperation->conv.weight_tensor->dims[1];
   struct pipe_transfer *transfer_in,  *transfer_out, *transfer_weights;
   int32_t *biases_in = pipe_buffer_map(pcontext, poperation->conv.bias_tensor->resource, PIPE_MAP_READ, &transfer_in);
   void *weights = pipe_buffer_map(pcontext, poperation->conv.weight_tensor->resource, PIPE_MAP_READ, &transfer_weights);
   struct pipe_resource *rsc;
   uint32_t *biases;

   rsc = pipe_buffer_create(pcontext->screen, 0, PIPE_USAGE_DEFAULT, output_channels * sizeof(uint32_t));
   biases = pipe_buffer_map(pcontext, rsc, PIPE_MAP_WRITE, &transfer_out);

   //fprintf(stderr, "weight_scale %x\n", fui(poperation->conv.weight_tensor->scale));
   /* TODO: Figure out when exactly we need to truncate */
   /* From http://nvdla.org/hw/v1/ias/unit_description.html#convolution-accumulator :
    *
    * The final result of accumulator in CACC is 48bits for INT16 and 34bits for
    * INT8. The bit width between CACC and SDP is 32. For precisions INT8 and INT16,
    * there is a round and saturation operation before sending the result to SDP.
    * The precision of rounding is configured by field CLIP_TRUNCATE in register
    * D_CLIP_CFG. For FP16, the value is just converted from FP48 to FP32.
    */
   if (fui(poperation->conv.weight_tensor->scale) == 0x3a88323f ||
       fui(poperation->conv.weight_tensor->scale) == 0x3c0060de ||
       fui(poperation->conv.weight_tensor->scale) == 0x3c06022d ||
       fui(poperation->conv.weight_tensor->scale) == 0x3c1642e3 ||
       fui(poperation->conv.weight_tensor->scale) == 0x3c1e3f51 ||
       fui(poperation->conv.weight_tensor->scale) == 0x3c5c8aa8 ||
       fui(poperation->conv.weight_tensor->scale) == 0x3c615e93 ||
       fui(poperation->conv.weight_tensor->scale) == 0x3c7326a2 ||
       fui(poperation->conv.weight_tensor->scale) == 0x3c783013 ||
       fui(poperation->conv.weight_tensor->scale) == 0x3d1748e6 ||
       fui(poperation->conv.weight_tensor->scale) == 0x3d282992 ||
       fui(poperation->conv.weight_tensor->scale) == 0x3d2e87ae ||
       fui(poperation->conv.weight_tensor->scale) == 0x3d77f5f6 ||
       fui(poperation->conv.weight_tensor->scale) == 0x3a9a5956 ||
       fui(poperation->conv.weight_tensor->scale) == 0x3caebc56)
      *truncate_bits = 1;
   else
      *truncate_bits = 0;

   int32_t max_bias = 0;
   int32_t max_corr = 0;
   unsigned max_num_bits = 0;
   bool retry = true;
   while(retry) {
      for (int oc = 0; oc < output_channels; oc++) {
         int32_t corr = calculate_bias_correction(subgraph, poperation, oc, weights);
         biases[oc] = (biases_in[oc] - corr) / (1 << *truncate_bits);

         uint64_t max_val = (biases_in[oc] - corr + 255 * 255 * weights_size * weights_size) / (1 << *truncate_bits);
         unsigned num_bits = ceil(log(abs(max_val)) / log(2)) + 1;
         max_bias = MAX2(max_bias, biases[oc]);
         max_corr = MAX2(max_corr, corr);
         max_num_bits = MAX2(max_num_bits, num_bits);

         /* TODO: This doesn't actually work, num_bits doesn't go above 19, and the blob sometimes truncates way below */
         if (num_bits > 32) {
            (*truncate_bits)++;
            retry = true;
         } else
            retry = false;
      }
   }

   if (1) {
      static int task = 0;
      dump_buffer((uint8_t*)biases, "biases", 0, task++, 0, output_channels * sizeof(uint32_t));
   }

   pipe_buffer_unmap(pcontext, transfer_out);

   pipe_buffer_unmap(pcontext, transfer_weights);

   pipe_buffer_unmap(pcontext, transfer_in);

   return rsc;
}

static void
compile_operation(struct rkt_ml_subgraph *subgraph, struct rkt_operation *operation)
{
   struct pipe_context *pcontext = subgraph->base.context;
   struct rkt_context *ctx = rkt_context(pcontext);
   unsigned regcfg_total_size = 0;
   struct util_dynarray *regcfgs;
   struct pipe_transfer *transfer = NULL;
   unsigned num_tasks = util_dynarray_num_elements(&operation->tasks, struct split_task);

   regcfgs = calloc(num_tasks, sizeof(struct util_dynarray));

   for (int i = 0; i < num_tasks; i++) {
      util_dynarray_init(&regcfgs[i], NULL);
      fill_regcmd(subgraph, operation, &regcfgs[i], i);

      unsigned size = util_dynarray_num_elements(&regcfgs[i], uint64_t) * sizeof(uint64_t);
      regcfg_total_size += ALIGN(size, 64);
   }

   operation->regcmd = pipe_buffer_create(pcontext->screen, 0, PIPE_USAGE_DEFAULT, regcfg_total_size);
   uint8_t *regcmd = pipe_buffer_map(pcontext, operation->regcmd, PIPE_MAP_WRITE, &transfer);

   unsigned regcmd_offset = 0;
   for (int i = 0; i < num_tasks; i++) {
      unsigned size = util_dynarray_num_elements(&regcfgs[i], uint64_t);
      struct split_task *task = util_dynarray_element(&operation->tasks, struct split_task, i);

      if (i < num_tasks - 1) {
         /* Patch next address and amount of regs to fetch, positions are relative to end */
         unsigned reg_count = util_dynarray_num_elements(&regcfgs[i], uint64_t);
         uint64_t *next_address_reg = util_dynarray_element(&regcfgs[i], uint64_t, reg_count - 4);
         uint64_t *reg_count_reg = util_dynarray_element(&regcfgs[i], uint64_t, reg_count - 3);

         uint64_t addr = rkt_resource(operation->regcmd)->phys_addr + regcmd_offset + ALIGN(size * sizeof(uint64_t), 64);
         *next_address_reg |= addr << 16;

         unsigned regs_to_fetch = util_dynarray_num_elements(&regcfgs[i + 1], uint64_t);
         regs_to_fetch -= 4;
         regs_to_fetch = ALIGN(regs_to_fetch / 2, 2);
         *reg_count_reg |= regs_to_fetch << 16;
      }

      memcpy(regcmd + regcmd_offset, util_dynarray_begin(&regcfgs[i]), size * sizeof(uint64_t));
      util_dynarray_fini(&regcfgs[i]);

      task->regcfg_amount = size;
      task->regcfg_addr = rkt_resource(operation->regcmd)->phys_addr + regcmd_offset;

      dump_buffer(regcmd, "regcmd", 0, i, regcmd_offset, (size + 4) * sizeof(uint64_t));

      regcmd_offset += ALIGN(size * sizeof(uint64_t), 64);
   }

   pipe_buffer_unmap(pcontext, transfer);

   for (int i = 0; i < num_tasks; i++) {
      util_dynarray_fini(&regcfgs[i]);
   }

   free(regcfgs);
}

static void
lower_convolution(struct rkt_ml_subgraph *subgraph, const struct pipe_ml_operation *poperation, struct rkt_operation *operation)
{
   struct pipe_context *pcontext = subgraph->base.context;

   util_dynarray_init(&operation->tasks, NULL);

   operation->depthwise = is_depthwise(poperation);
   operation->padding_same = poperation->conv.padding_same;
   operation->stride = poperation->conv.stride_x;

   operation->input_index = poperation->input_tensors[0]->index;
   operation->input_width = poperation->input_tensors[0]->dims[1];
   operation->input_height = poperation->input_tensors[0]->dims[2];
   operation->input_channels = poperation->input_tensors[0]->dims[3];
   operation->input_zero_point = poperation->input_tensors[0]->zero_point;
   operation->input_scale = poperation->input_tensors[0]->scale;

   operation->output_index = poperation->output_tensors[0]->index;
   operation->output_width = poperation->output_tensors[0]->dims[1];
   operation->output_height = poperation->output_tensors[0]->dims[2];
   operation->output_channels = poperation->output_tensors[0]->dims[3];
   operation->output_zero_point = poperation->output_tensors[0]->zero_point;
   operation->output_scale = poperation->output_tensors[0]->scale;

   operation->weights_width = poperation->conv.weight_tensor->dims[1];
   operation->weights_height = poperation->conv.weight_tensor->dims[2];
   operation->weights_zero_point = poperation->conv.weight_tensor->zero_point;
   operation->weights_scale = poperation->conv.weight_tensor->scale;

   operation->weights = fill_weights(subgraph, poperation);
   operation->biases = fill_biases(subgraph, poperation, &operation->truncate_bits);
}

static struct rkt_operation *
find_first_consumer(struct rkt_ml_subgraph *subgraph, unsigned tensor_index)
{
   util_dynarray_foreach(&subgraph->operations, struct rkt_operation, operation) {
      if (operation->input_index == tensor_index)
         return operation;
   }

   return NULL;
}

static struct rkt_operation *
find_producer(struct rkt_ml_subgraph *subgraph, unsigned tensor_index)
{
   util_dynarray_foreach(&subgraph->operations, struct rkt_operation, operation) {
      if (operation->output_index == tensor_index)
         return operation;
   }

   return NULL;
}

static unsigned
count_tensors(const struct pipe_ml_operation *poperations,
              unsigned count)
{
   unsigned tensor_count = 0;

   for (unsigned i = 0; i < count; i++) {
      const struct pipe_ml_operation *poperation = &poperations[i];
      tensor_count = MAX2(tensor_count, poperation->input_tensors[0]->index);
      tensor_count = MAX2(tensor_count, poperation->output_tensors[0]->index);
      switch (poperation->type) {
      case PIPE_ML_OPERATION_TYPE_CONVOLUTION:
         tensor_count = MAX2(tensor_count, poperation->conv.weight_tensor->index);
         tensor_count = MAX2(tensor_count, poperation->conv.bias_tensor->index);
         break;
      case PIPE_ML_OPERATION_TYPE_ADD:
         tensor_count = MAX2(tensor_count, poperation->input_tensors[1]->index);
         break;
      default:
         fprintf(stderr, "poperation->type %d\n", poperation->type);
         unreachable("Unsupported ML operation type");
      }
   }

   return tensor_count + 1;
}

struct pipe_ml_subgraph *
rkt_ml_subgraph_create(struct pipe_context *pcontext,
                       const struct pipe_ml_operation *poperations,
                       unsigned count)
{
   struct rkt_context *ctx = rkt_context(pcontext);
   struct rkt_ml_subgraph *subgraph;
   unsigned regcfg_amount;
   unsigned tensor_count;

   subgraph = calloc(1, sizeof(*subgraph));
   subgraph->base.context = pcontext;

   tensor_count = count_tensors(poperations, count);
   util_dynarray_init(&subgraph->tensors, NULL);
   util_dynarray_init(&subgraph->operations, NULL);
   if (!util_dynarray_resize(&subgraph->tensors, struct pipe_resource *, tensor_count))
      return NULL;
   memset(util_dynarray_begin(&subgraph->tensors), 0, subgraph->tensors.size);

   /* Lower */
   for(int i = 0; i < count; i++) {
      struct rkt_operation operation = {0};
      operation.add_tensor = -1;

      switch(poperations[i].type) {
         case PIPE_ML_OPERATION_TYPE_CONVOLUTION:
            lower_convolution(subgraph, &poperations[i], &operation);
            util_dynarray_append(&subgraph->operations, struct rkt_operation, operation);
            break;
         case PIPE_ML_OPERATION_TYPE_ADD: {
            /* Fuse tensor addition into convolution*/
            struct rkt_operation *input_op_1 = find_producer(subgraph, poperations[i].input_tensors[1]->index);
            struct rkt_operation *input_op_2 = find_producer(subgraph, poperations[i].input_tensors[0]->index);

            assert(input_op_1);
            assert(input_op_2);

            if (input_op_1 == NULL) {
               /* Graph input */
               input_op_2->add_tensor = poperations[i].input_tensors[1]->index;
            } else {
               input_op_1->addition_input = true;
               input_op_2->add_tensor = input_op_1->output_index;
            }

            input_op_2->output_index = poperations[i].output_tensors[0]->index;
            input_op_2->addition_offset = 0x80 - poperations[i].input_tensors[1]->zero_point;
            input_op_2->addition_scale = poperations[i].input_tensors[1]->scale;

            break;
	 }
      }
   }

   /* Create input tensors */
   util_dynarray_foreach(&subgraph->operations, struct rkt_operation, operation) {
      unsigned input_channels_1 = DIV_ROUND_UP(operation->input_channels, FEATURE_ATOMIC_SIZE) * 2;
      unsigned input_channels_2 = FEATURE_ATOMIC_SIZE;
      unsigned input_size = operation->input_width * operation->input_height * input_channels_1 * input_channels_2;

      create_tensor(subgraph, operation->input_index, input_size);
   }

   /* Create output tensors */
   util_dynarray_foreach(&subgraph->operations, struct rkt_operation, operation) {
      struct rkt_resource *res = get_tensor(subgraph, operation->output_index);
      if (res != NULL)
         continue;

      create_tensor(subgraph, operation->output_index, calc_raw_output_size(operation));
   }

   /* Compile */
   util_dynarray_foreach(&subgraph->operations, struct rkt_operation, operation) {
      split_tasks(subgraph, operation);
      compile_operation(subgraph, operation);
   }

   return &subgraph->base;
}

void
rkt_ml_subgraph_invoke(struct pipe_context *pcontext,
                       struct pipe_ml_subgraph *psubgraph,
                       unsigned inputs_count,
                       unsigned input_idxs[],
                       void *inputs[],
                       bool is_signed[])
{
   struct rkt_screen *screen = rkt_screen(pcontext->screen);
   struct rkt_ml_subgraph *subgraph = (struct rkt_ml_subgraph *)(psubgraph);
   int ret;

   trace_printk("Processing input\n");

   for (int i = 0; i < inputs_count; i++) {
      struct rkt_operation *operation = find_first_consumer(subgraph, input_idxs[i]);
      struct pipe_resource *input = &get_tensor(subgraph, input_idxs[i])->base;
      unsigned input_channels = operation->input_channels;
      unsigned output_channels = operation->output_channels;

      struct rkt_resource *input_tensor = get_tensor(subgraph, operation->input_index);
      if (output_channels == 1 && input_channels == 1 && !operation->addition_input && (operation->add_tensor == -1)) {
         pipe_buffer_copy(pcontext, &input_tensor->base, input, 0, 0, pipe_buffer_size(input));
      } else {
         unsigned input_width = operation->input_width;
         unsigned input_height = operation->input_height;
         unsigned zero_point = operation->input_zero_point;
         struct pipe_transfer *transfer_out;
         uint8_t (*input_in)[input_height][input_channels] = inputs[i];
         uint8_t *map = pipe_buffer_map(pcontext, &input_tensor->base, PIPE_MAP_WRITE, &transfer_out);

         trace_printk("Converting data\n");

         /*
          * From the NVDLA docs: "For int8, one element of data refers to an 8-bit signed integer."
          * But only when transposing do we seem to need to convert to signed.
          * The DMA unit seems to be able to convert from unsigned to signed though.
          */
         if (input_channels == 1) {
            unsigned n = 0;
            for (int x = 0; x < input_width; x++) {
               for (int y = 0; y < MAX2(input_height, FEATURE_ATOMIC_SIZE); y++) {
                  if (y < input_height)
                     map[n++] = input_in[x][y][0];
                  else
                     map[n++] = zero_point;
               }
            }
         } else {
            unsigned n = 0;
            for (int u = 0; u < DIV_ROUND_UP(input_channels, FEATURE_ATOMIC_SIZE); u++) {
               for (int x = 0; x < input_width; x++) {
                  for (int y = 0; y < input_height; y++) {
                     for (int c = 0; c < FEATURE_ATOMIC_SIZE; c++) {
                        unsigned input_channel = c + u * FEATURE_ATOMIC_SIZE;
                        if (input_channel < input_channels)
                           map[n++] = input_in[x][y][input_channel] - 0x80;
                        else
                           map[n++] = zero_point - 0x80;
                     }
                  }
               }
            }
         }

         dump_buffer(map, "input", 0, 0, 0, get_tensor(subgraph, input_idxs[i])->bo_size);

         trace_printk("Converted data\n");

         pipe_buffer_unmap(pcontext, transfer_out);
      }
   }
   trace_printk("Processed input\n");

   trace_printk("Submitting graph\n");

   #define MAX_TASKS 16

   struct util_dynarray jobs;
   util_dynarray_init(&jobs, NULL);

   util_dynarray_foreach(&subgraph->operations, struct rkt_operation, operation) {
      unsigned num_inputs = operation->add_tensor != -1 ? 2 : 1;
      uint32_t *in_bo_handles = calloc(num_inputs, sizeof(uint32_t));
      uint32_t *out_bo_handles = malloc(sizeof(uint32_t));

      in_bo_handles[0] = get_tensor(subgraph, operation->input_index)->handle;

      if (operation->add_tensor != -1)
         in_bo_handles[1] = get_tensor(subgraph, operation->add_tensor)->handle;

      out_bo_handles[0] = get_tensor(subgraph, operation->output_index)->handle;

      if (operation->reuse_weights_cbuf) {
         /* Submit all tasks to the same core, so weights can be reused */
         unsigned num_tasks = util_dynarray_num_elements(&operation->tasks, struct split_task);
         struct drm_rocket_task *tasks = calloc(num_tasks, sizeof(*tasks));
         unsigned task_count = 0;
         util_dynarray_foreach(&operation->tasks, struct split_task, task) {
            tasks[task_count].regcmd = task->regcfg_addr;
            tasks[task_count].regcmd_count = task->regcfg_amount;
            task_count++;
         }
         struct drm_rocket_job job = {0};
         job.in_bo_handles = (uint64_t)(uintptr_t)in_bo_handles;
         job.in_bo_handle_count = num_inputs;
         job.out_bo_handles = (uint64_t)(uintptr_t)out_bo_handles;
         job.out_bo_handle_count = 1;
         job.tasks = (uint64_t)tasks;
         job.task_count = task_count;
         util_dynarray_append(&jobs, struct drm_rocket_job, job);
      } else {
         /* Spread tasks among cores, for parallelism */
         util_dynarray_foreach(&operation->tasks, struct split_task, task) {
            struct drm_rocket_task *ktask = calloc(1, sizeof(*ktask));
            ktask->regcmd = task->regcfg_addr;
            ktask->regcmd_count = task->regcfg_amount;

            struct drm_rocket_job job = {0};
            job.in_bo_handles = (uint64_t)(uintptr_t)in_bo_handles;
            job.in_bo_handle_count = num_inputs;
            job.out_bo_handles = (uint64_t)(uintptr_t)out_bo_handles;
            job.out_bo_handle_count = 1;
            job.tasks = (uint64_t)ktask;
            job.task_count = 1;
            util_dynarray_append(&jobs, struct drm_rocket_job, job);
         }
      }
   }

   struct drm_rocket_submit submit = {
      .jobs = (uint64_t)util_dynarray_begin(&jobs),
      .job_count = util_dynarray_num_elements(&jobs, struct drm_rocket_job),
   };

   ret = drmIoctl(screen->fd, DRM_IOCTL_ROCKET_SUBMIT, &submit);
   assert(ret == 0);

   /* TODO: Free all the stuff */
   util_dynarray_fini(&jobs);

   trace_printk("Submitted graph\n");
}

void rkt_ml_subgraph_read_outputs(struct pipe_context *pcontext,
                                  struct pipe_ml_subgraph *psubgraph,
                                  unsigned outputs_count, unsigned output_idxs[],
                                  void *outputs[], bool is_signed[])
{
   struct rkt_ml_subgraph *subgraph = (struct rkt_ml_subgraph *)(psubgraph);

   trace_printk("Processing output\n");

   for (int i = 0; i < outputs_count; i++) {

      struct rkt_operation *operation = find_producer(subgraph, output_idxs[i]);
      struct rkt_resource *output_tensor = get_tensor(subgraph, output_idxs[i]);
      struct pipe_transfer *transfer = NULL;
      uint8_t *raw_output;
      uint8_t (*output_in)[operation->output_height][operation->output_width][FEATURE_ATOMIC_SIZE];
      uint8_t (*output_out)[operation->output_width][operation->output_channels];

      trace_printk("Before pipe_buffer_map\n");
      raw_output = pipe_buffer_map(pcontext, &output_tensor->base, PIPE_MAP_READ, &transfer);
      trace_printk("After pipe_buffer_map\n");
#if 1
      trace_printk("Converting data\n");

      output_in = (void *)raw_output;
      output_out = (void *)outputs[i];

      dump_buffer(raw_output, "output", 0, 0, 0, output_tensor->bo_size);

      unsigned n = 0;
      for (int oc = 0; oc < operation->output_channels; oc++) {
         for (int x = 0; x < operation->output_width; x++) {
            for (int y = 0; y < operation->output_height; y++) {
               unsigned c = oc % FEATURE_ATOMIC_SIZE;
               unsigned g = oc / FEATURE_ATOMIC_SIZE;
               output_out[y][x][oc] = output_in[g][y][x][c] + 0x80;
            }
         }
      }

      trace_printk("Converted data\n");

#endif

      pipe_buffer_unmap(pcontext, transfer);
   }

   trace_printk("Processed output\n");
}

static void
free_operation(struct rkt_operation *operation)
{
   util_dynarray_fini(&operation->tasks);
   pipe_resource_reference(&operation->regcmd, NULL);
   pipe_resource_reference(&operation->weights, NULL);
   pipe_resource_reference(&operation->biases, NULL);

   free(operation);
}

void
rkt_ml_subgraph_destroy(struct pipe_context *context, struct pipe_ml_subgraph *psubgraph)
{
   struct rkt_ml_subgraph *subgraph = (struct rkt_ml_subgraph *)(psubgraph);

   //free_operation(subgraph->operation);
}
