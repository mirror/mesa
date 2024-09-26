/*
 * Copyright © 2024 Raspberry Pi Ltd
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

#ifndef AUTOCLIF_DUMP_H
#define AUTOCLIF_DUMP_H

#include <stddef.h>
#include <stdint.h>

struct drm_v3d_submit_cl;
struct drm_v3d_submit_csd;
struct drm_v3d_submit_tfu;

typedef void (*autoclif_mem_read)(void *dst, uint64_t src_addr, size_t size, void *p);

void autoclif_cl_dump(uint32_t qpu_count,
                      struct drm_v3d_submit_cl *submit,
                      autoclif_mem_read mem_read,
                      void *p);

void autoclif_csd_dump(uint32_t qpu_count,
                       struct drm_v3d_submit_csd *submit,
                       autoclif_mem_read mem_read,
                       void *p);

void autoclif_tfu_dump(uint32_t qpu_count,
                       struct drm_v3d_submit_tfu *submit,
                       autoclif_mem_read mem_read,
                       void *p);

#endif
