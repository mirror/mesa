/*
 * Copyright 2014, 2015 Red Hat.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef VIRGL_gdi_WINSYS_H
#define VIRGL_gdi_WINSYS_H

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdint.h>
#include "virgl_gdi_public.h"
#include "util/u_thread.h"
#include "pipe/p_state.h"
#include "util/list.h"
#include "virgl/virgl_winsys.h"
#include "virgl_resource_cache.h"

#include "virtio/wddm/viogpu_wddm_driver.h"


struct pipe_fence_handle;
struct hash_table;

struct virgl_hw_res {
   struct pipe_reference reference;
   enum pipe_texture_target target;
   uint32_t res_handle;
   HANDLE hResource;
   D3DKMT_HANDLE hAllocation;
   int num_cs_references;
   uint32_t size;
   void *ptr;

   struct virgl_resource_cache_entry cache_entry;
   uint32_t bind;
   uint32_t flags;

   /* false when the resource is known to be typed */
   bool maybe_untyped;

   /* true when the resource is imported or exported */
   int shared;

   /* false when the resource is known to be idle */
   int maybe_busy;
   uint32_t blob_mem;
};

struct virgl_gdi_winsys
{
   struct virgl_winsys base;

   struct gdikmt_device *device;
   VIOGPU_ADAPTERINFO adapterInfo;

   mtx_t coreMtx;
   struct virgl_cmd_buf *coreCtx;

   struct virgl_resource_cache cache;
   mtx_t cacheMtx;
};

struct virgl_gdi_fence {
   struct pipe_reference reference;
   bool external;
   HANDLE handle;
};

struct virgl_gdi_cmd_buf {
   struct virgl_cmd_buf base;
   
   struct virgl_winsys *ws;
   struct gdikmt_context *ctx;

   int in_fence_fd;

   int alloc_count;
   int max_alloc;
   struct virgl_hw_res **res_bo;

   UINT driver_length;
};

static inline struct virgl_gdi_winsys *
virgl_gdi_winsys(struct virgl_winsys *iws)
{
   return (struct virgl_gdi_winsys *)iws;
}

static inline struct virgl_gdi_fence *
virgl_gdi_fence(struct pipe_fence_handle *f)
{
   return (struct virgl_gdi_fence *)f;
}

static inline struct virgl_gdi_cmd_buf *
virgl_gdi_cmd_buf(struct virgl_cmd_buf *cbuf)
{
   return (struct virgl_gdi_cmd_buf *)cbuf;
}

#endif
