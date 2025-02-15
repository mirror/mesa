/* Copyright © 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "libintel_shaders.h"

/* Memcpy data using multiple lanes. */
void genX(libanv_memcpy)(global void *dst_base,
                         global void *src_base,
                         uint num_dwords,
                         uint dword_offset)
{
   global void *dst = dst_base + 4 * dword_offset;
   global void *src = src_base + 4 * dword_offset;

   if (dword_offset + 4 <= num_dwords) {
      *(global uint4 *)(dst) = *(global uint4 *)(src);
   } else if (dword_offset + 3 <= num_dwords) {
      *(global uint3 *)(dst) = *(global uint3 *)(src);
   } else if (dword_offset + 2 <= num_dwords) {
      *(global uint2 *)(dst) = *(global uint2 *)(src);
   } else if (dword_offset + 1 <= num_dwords) {
      *(global uint *)(dst) = *(global uint *)(src);
   }
}

/* Copy size from src_ptr to dst_ptr for using a single lane with size
 * multiple of 4.
 */
void genX(copy_data)(global void *dst_base,
                     global void *src_base,
                     uint32_t size)
{
   for (uint32_t offset = 0; offset < size; offset += 16) {
      global void *dst = dst_base + offset;
      global void *src = src_base + offset;
      if (offset + 16 <= size) {
         /* printf("dst=%p src=%p\n", dst, src); */
         /* uint4 v = *(global uint4 *)(src); */
         /* printf("src: 0x%08x, 0x%08x, 0x%08x, 0x%08x\n", */
         /*        v[0], v[1], v[2], v[3]); */
         *(global uint4 *)(dst) = *(global uint4 *)(src);
      } else if (offset + 12 <= size) {
         /* uint3 v = *(global uint3 *)(src); */
         /* printf("src: 0x%08x, 0x%08x, 0x%08x\n", */
         /*        v[0], v[1], v[2]); */
         *(global uint3 *)(dst) = *(global uint3 *)(src);
      } else if (offset + 8 <= size) {
         /* uint2 v = *(global uint2 *)(src); */
         /* printf("src: 0x%08x, 0x%08x\n", */
         /*        v[0], v[1]); */
         *(global uint2 *)(dst) = *(global uint2 *)(src);
      } else if (offset + 4 <= size) {
         *(global uint *)(dst) = *(global uint *)(src);
      }
   }
}

/* memset data into dst_ptr */
void genX(set_data)(global void *dst_base,
                    uint32_t data,
                    uint32_t size)
{
   for (uint32_t offset = 0; offset < size; offset += 16) {
      global void *dst = dst_base + offset;
      if (offset + 16 <= size) {
         *(global uint4 *)(dst) = (uint4)data;
      } else if (offset + 12 <= size) {
         *(global uint3 *)(dst) = (uint3)data;
      } else if (offset + 8 <= size) {
         *(global uint2 *)(dst) = (uint2)data;
      } else if (offset + 4 <= size) {
         *(global uint *)(dst) = (uint)data;
      }
   }
}
