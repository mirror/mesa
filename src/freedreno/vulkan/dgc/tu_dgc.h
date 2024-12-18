/*
 * Copyright © 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TU_DGC_H
#define TU_DGC_H

#ifdef VULKAN

#define TYPE(type, align)                                                     \
   layout(buffer_reference, buffer_reference_align = align, scalar) buffer type##_ref \
   {                                                                          \
      type value;                                                             \
   };

#define REF(type)  type##_ref
#define DEREF(var) var.value

TYPE(uint32_t, 4)
TYPE(uint64_t, 4)
TYPE(uvec3, 4)
TYPE(uvec4, 4)

#else

#include <stdint.h>
#define TYPE(type, align)
#define REF(type) uint64_t

#endif

/* A DGC layout consists of a collection of template buffers, that represent
 * command streams, draw parameters, etc. and a set of patchpoints. The
 * preprocess buffer contains an array of "maxSequenceCount" buffers for each
 * template buffer with the final data for that sequence, plus the trampoline
 * IB. The preprocess shader takes the template buffers and a sequence of
 * patchpoints for each destination buffer and fills out the final buffers for
 * each sequence.
 *
 * The size of an IB is limited, so we have to support multiple IBs, each with
 * its own trampoline, which means that the layout of the main command stream
 * buffer (which the first trampoline points to) is a bit more complicated. We
 * have to calculate how many sequences can fit in an IB and split it up into
 * IBs, each of which ends in a trampoline pointing to the next IB:
 *
 * T_0 ... IB_0 T_1 -> IB_1 ... T_N -> IB_N
 *
 * Trampolines other than the first one (which is called from the main
 * command stream) are at the end of the previous IB.
 */

/* Replace "size" dwords at "dst_offset" with the data at
 * "indirectAddress + sequence_id * indirectStride + src_offset"
 */
#define TU_DGC_PATCHPOINT_SRC_DIRECT 0

/* Replace "size" dwords at "dst_offset" with the pipeline data at
 * "src_offset".
 */
#define TU_DGC_PATCHPOINT_PIPELINE_DIRECT 1

/* Replace 2 dwords at "dst_offset" with the address of
 * "indirectAddress + sequence_id * indirectStride + src_offset." Additionally
 * OR (mask << shift), in order to construct a UBO descriptor.
 */
#define TU_DGC_PATCHPOINT_SRC_INDIRECT 2

/* Replace 2 dwords at "dst_offset" with the address of the destination buffer
 * "dst_buffer" for this sequence plus "src_offset." Additionally OR (mask <<
 * shift), in order to construct a UBO descriptor.
 */
#define TU_DGC_PATCHPOINT_DST_INDIRECT 3

/* OR the given dword at "dst_offset" with the pipeline data shifted by
 * "shift".
 */
#define TU_DGC_PATCHPOINT_PIPELINE_FIELD 4

/* Set the given dword to the sequence index */
#define TU_DGC_PATCHPOINT_SEQUENCE_INDEX 5

/* Read the VkBindIndexBufferIndirectCommandEXT from src_offset and set:
 * - index base
 * - max index (from index size and index type)
 */
#define TU_DGC_PATCHPOINT_INDEX_VULKAN 7

/* Read the D3D12_INDEX_BUFFER_VIEW from src_offset and set:
 * - draw initiator: patch in the index type
 * - index base
 * - max index (from index size and index type)
 */
#define TU_DGC_PATCHPOINT_INDEX_DX 8

/* Read the VkBindIndexBufferIndirectCommandEXT from src_offset, read a dword
 * from "src_buffer", and patch in the parsed index size shifted by "shift".
 */
#define TU_DGC_PATCHPOINT_DRAW_INITIATOR_VULKAN 9

/* Read the D3D12_INDEX_BUFFER_VIEW from src_offset, read a dword from
 * "src_buffer" pipeline, and patch in the parsed index size shifted by
 * "shift".
 */
#define TU_DGC_PATCHPOINT_DRAW_INITIATOR_DX 10

/* Replace with the max_draw_count argument */
#define TU_DGC_PATCHPOINT_MAX_DRAW_COUNT 11

/* Similar to a DIRECT_SRC patchpoint, but with a hardcoded size of 3, and set
 * the last dword (the size) to 0 if the first two (the address) is 0 to make
 * sure that robustness works correctly.
 */
#define TU_DGC_PATCHPOINT_VBO 12

struct tu_dgc_patchpoint {
   uint32_t src_offset;
   uint16_t src_buffer;
   uint16_t dst_offset;
   uint16_t size;
   uint16_t type;
   uint16_t shift;
   uint16_t mask;
};

/* For draws:
 * - Command buffer
 * - Push constants draw state
 * - VBO draw state
 * - VBO stride draw state
 * For dispatches:
 * - Command buffer
 * - Push constants IB (to be consistent with draws)
 * - Driver params UBO
 */
#define TU_DGC_MAX_BUFFERS 4
#define TU_DGC_BUFFER_MAX_SIZE 2048
#define TU_DGC_MAX_PATCHPOINTS 512
#define TU_DGC_PIPELINE_SIZE 512

struct tu_dgc_args {
   REF(uint32_t) sequence_count_addr; /* sequenceCountAddress in the API */
   uint64_t trampoline_addr;
   uint64_t dst_buffer_addr[TU_DGC_MAX_BUFFERS];
   uint64_t src_indirect_addr; /* indirectAddress in the API */
   uint32_t src_indirect_stride; /* indirectStride in the API */
   uint32_t max_sequence_count; /* maxSequenceCount in the API */
   uint32_t max_draw_count; /* maxDrawCount in the API */
   uint32_t ib_sequence_offset;
   uint32_t sequences_per_ib;
   uint32_t src_pipeline_offset;
   uint32_t buffer_count;
   uint32_t main_buffer;
   uint32_t buffer_stride[TU_DGC_MAX_BUFFERS]; /* in dwords */
   uint32_t patchpoint_count[TU_DGC_MAX_BUFFERS];
};

#endif
