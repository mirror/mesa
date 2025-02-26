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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winternl.h>

#include "gdikmt/gdikmt.h"
#include "pipe/p_state.h"
#include "util/format/u_formats.h"
#include "util/simple_mtx.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "frontend/winsys_handle.h"

#include "virgl/virgl_context.h"
#include "virgl/virgl_public.h"
#include "virgl/virgl_resource.h"
#include "virgl/virgl_screen.h"

#include "virgl/virgl_winsys.h"
#include "virtio-gpu/virgl_hw.h"
#include "wddm/viogpu_wddm_driver.h"
#include "virgl_gdi_public.h"
#include "virgl_gdi_winsys.h"

#include <d3dkmthk.h>
#include <debugapi.h>

#define VIRGL_DRM_CAPSET_VIRGL  1
#define VIRGL_DRM_CAPSET_VIRGL2 2

/* Gets a pointer to the virgl_hw_res containing the pointed to cache entry. */
#define cache_entry_container_res(ptr)                                         \
   (struct virgl_hw_res *)((char *)ptr -                                       \
                           offsetof(struct virgl_hw_res, cache_entry))

static inline boolean
can_cache_resource(uint32_t bind)
{
   if (bind & VIRGL_BIND_SHARED)
      return false;

   return bind == VIRGL_BIND_CONSTANT_BUFFER ||
          bind == VIRGL_BIND_INDEX_BUFFER || bind == VIRGL_BIND_VERTEX_BUFFER ||
          bind == VIRGL_BIND_CUSTOM || bind == VIRGL_BIND_STAGING ||
          bind == VIRGL_BIND_DEPTH_STENCIL ||
          bind == VIRGL_BIND_RENDER_TARGET || bind == 0;
}

static inline unsigned virgl_to_pipe_bind(unsigned pbind)
{
   unsigned outbind = 0;
   if (pbind & VIRGL_BIND_DEPTH_STENCIL)
      outbind |= PIPE_BIND_DEPTH_STENCIL;
   if (pbind & VIRGL_BIND_RENDER_TARGET)
      outbind |= PIPE_BIND_RENDER_TARGET;
   if (pbind & VIRGL_BIND_SAMPLER_VIEW)
      outbind |= PIPE_BIND_SAMPLER_VIEW;
   if (pbind & VIRGL_BIND_VERTEX_BUFFER)
      outbind |= PIPE_BIND_VERTEX_BUFFER;
   if (pbind & VIRGL_BIND_INDEX_BUFFER)
      outbind |= PIPE_BIND_INDEX_BUFFER;
   if (pbind & VIRGL_BIND_CONSTANT_BUFFER)
      outbind |= PIPE_BIND_CONSTANT_BUFFER;
   if (pbind & VIRGL_BIND_DISPLAY_TARGET)
      outbind |= PIPE_BIND_DISPLAY_TARGET;
   if (pbind & VIRGL_BIND_STREAM_OUTPUT)
      outbind |= PIPE_BIND_STREAM_OUTPUT;
   if (pbind & VIRGL_BIND_CURSOR)
      outbind |= PIPE_BIND_CURSOR;
   if (pbind & VIRGL_BIND_CUSTOM)
      outbind |= PIPE_BIND_CUSTOM;
   if (pbind & VIRGL_BIND_SCANOUT)
      outbind |= PIPE_BIND_SCANOUT;
   if (pbind & VIRGL_BIND_SHARED)
      outbind |= PIPE_BIND_SHARED;
   if (pbind & VIRGL_BIND_SHADER_BUFFER)
      outbind |= PIPE_BIND_SHADER_BUFFER;
   if (pbind & VIRGL_BIND_QUERY_BUFFER)
      outbind |= PIPE_BIND_QUERY_BUFFER;
   if (pbind & VIRGL_BIND_COMMAND_ARGS)
      outbind |= PIPE_BIND_COMMAND_ARGS_BUFFER;

   return outbind;
}

static inline unsigned virgl_to_pipe_flags(unsigned pflags)
{
   unsigned out_flags = 0;
   if (pflags & VIRGL_RESOURCE_FLAG_MAP_PERSISTENT)
      out_flags |= PIPE_RESOURCE_FLAG_MAP_PERSISTENT;

   if (pflags & VIRGL_RESOURCE_FLAG_MAP_COHERENT)
      out_flags |= PIPE_RESOURCE_FLAG_MAP_COHERENT;

   return out_flags;
}

static void
virgl_hw_res_destroy(struct virgl_gdi_winsys *qdws, struct virgl_hw_res *res)
{
   if (pipe_is_referenced(&res->reference)) {
      return;
   }

   if (!p_atomic_read(&res->shared)) {
      NTSTATUS Status = qdws->device->destroyAllocation(
         qdws->device, res->hResource, res->hAllocation);
      if (!NT_SUCCESS(Status)) {
         _debug_printf("Failed to destroy allocation with status code: %lx\n",
                       Status);
      }
   }

   FREE(res);
}

static boolean
virgl_gdi_resource_is_busy(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   if (!p_atomic_read(&res->maybe_busy) && !p_atomic_read(&res->shared))
      return false;

   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(vws);

   VIOGPU_ESCAPE resid_escape;
   resid_escape.Type = VIOGPU_RES_BUSY;
   resid_escape.DataLength = sizeof(VIOGPU_RES_BUSY_REQ);
   resid_escape.ResourceBusy.ResHandle = res->hAllocation;
   resid_escape.ResourceBusy.Wait = FALSE;

   NTSTATUS Status =
      qdws->device->escape(qdws->device, &resid_escape, sizeof(resid_escape));

   if (!NT_SUCCESS(Status)) {
      _debug_printf(
         "Failed to check if allocation is busy with status code: %lx\n",
         Status);
   }

   p_atomic_set(&res->maybe_busy, resid_escape.ResourceBusy.IsBusy);

   return resid_escape.ResourceBusy.IsBusy;
}

static void
virgl_gdi_resource_reference(struct virgl_winsys *qws,
                             struct virgl_hw_res **dres,
                             struct virgl_hw_res *sres)
{
   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);
   struct virgl_hw_res *old = *dres;

   if (pipe_reference(&(*dres)->reference, &sres->reference)) {
      if (!can_cache_resource(old->bind) || p_atomic_read(&old->shared)) {
         virgl_hw_res_destroy(qdws, old);
      } else {
         mtx_lock(&qdws->cacheMtx);
         virgl_resource_cache_add(&qdws->cache, &old->cache_entry);
         mtx_unlock(&qdws->cacheMtx);
      }
   }
   *dres = sres;
}

static struct virgl_hw_res *
virgl_gdi_winsys_resource_create(struct virgl_winsys *qws,
                                 enum pipe_texture_target target,
                                 uint32_t format, uint32_t bind, uint32_t width,
                                 uint32_t height, uint32_t depth,
                                 uint32_t array_size, uint32_t last_level,
                                 uint32_t nr_samples, uint32_t flags,
                                 uint32_t size)
{
   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);

   VIOGPU_CREATE_ALLOCATION_EXCHANGE AllocExchange;
   VIOGPU_CREATE_RESOURCE_EXCHANGE ResExchange;
   memset(&AllocExchange, 0, sizeof(AllocExchange));
   memset(&ResExchange, 0, sizeof(ResExchange));

   AllocExchange.ResourceOptions.target = target;
   AllocExchange.ResourceOptions.format = pipe_to_virgl_format(format);
   AllocExchange.ResourceOptions.bind = bind;
   AllocExchange.ResourceOptions.width = width;
   AllocExchange.ResourceOptions.height = height;
   AllocExchange.ResourceOptions.depth = depth;
   AllocExchange.ResourceOptions.array_size = array_size;
   AllocExchange.ResourceOptions.last_level = last_level;
   AllocExchange.ResourceOptions.nr_samples = nr_samples;
   AllocExchange.ResourceOptions.flags = flags;
   AllocExchange.Size = size;

   struct gdikmt_createallocation CreateAllocation;
   D3DDDI_ALLOCATIONINFO AllocationInfo;
   memset(&CreateAllocation, 0, sizeof(CreateAllocation));
   memset(&AllocationInfo, 0, sizeof(AllocationInfo));

   CreateAllocation.NumAllocations = 1;
   CreateAllocation.pAllocationInfo = &AllocationInfo;

   CreateAllocation.pPrivateDriverData = &ResExchange;
   CreateAllocation.PrivateDriverDataSize = sizeof(ResExchange);

   AllocationInfo.hAllocation = 0;
   AllocationInfo.pPrivateDriverData = &AllocExchange;
   AllocationInfo.PrivateDriverDataSize = sizeof(AllocExchange);

   NTSTATUS status =
      qdws->device->createAllocation(qdws->device, &CreateAllocation);
   if (!NT_SUCCESS(status)) {
      _debug_printf(
         "Failed to create resource(D3DKMTCreateAllocation) with status code: %lx\n",
         status);
      return NULL;
   }

   VIOGPU_ESCAPE resid_escape;
   resid_escape.Type = VIOGPU_RES_INFO;
   resid_escape.DataLength = sizeof(VIOGPU_RES_INFO_REQ);
   resid_escape.ResourceInfo.ResHandle = AllocationInfo.hAllocation;

   status =
      qdws->device->escape(qdws->device, &resid_escape, sizeof(resid_escape));
   if (!NT_SUCCESS(status)) {
      _debug_printf(
         "Failed to get resource id(D3DKMTEscape@VIOGPU_RESID_REQ) with status code: %lx\n",
         status);
      return NULL;
   }

   struct virgl_hw_res *res;
   struct virgl_resource_params params = {.size = size,
                                          .bind = bind,
                                          .format = format,
                                          .flags = 0,
                                          .nr_samples = nr_samples,
                                          .width = width,
                                          .height = height,
                                          .depth = depth,
                                          .array_size = array_size,
                                          .last_level = last_level,
                                          .target = target};

   res = CALLOC_STRUCT(virgl_hw_res);
   if (!res)
      return NULL;

   res->bind = bind;

   res->res_handle = resid_escape.ResourceInfo.Id;
   res->hResource = CreateAllocation.hResource;
   res->hAllocation = AllocationInfo.hAllocation;
   res->size = size;
   res->target = target;
   res->maybe_untyped = false;
   res->ptr = NULL;
   pipe_reference_init(&res->reference, 1);
   p_atomic_set(&res->shared, (bind & VIRGL_BIND_SHARED) != 0);
   p_atomic_set(&res->num_cs_references, 0);

   /* A newly created resource is considered busy by the kernel until the
    * command is retired.  But for our purposes, we can consider it idle
    * unless it is used for fencing.
    */
   p_atomic_set(&res->maybe_busy, false);

   virgl_resource_cache_entry_init(&res->cache_entry, params);

   return res;
}

static struct virgl_hw_res *
virgl_gdi_winsys_resource_cache_create(
   struct virgl_winsys *qws, enum pipe_texture_target target,
   const void *map_front_private, uint32_t format, uint32_t bind,
   uint32_t width, uint32_t height, uint32_t depth, uint32_t array_size,
   uint32_t last_level, uint32_t nr_samples, uint32_t flags, uint32_t size)
{
   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);
   struct virgl_hw_res *res;
   struct virgl_resource_cache_entry *entry;
   struct virgl_resource_params params = {.size = size,
                                          .bind = bind,
                                          .format = format,
                                          .flags = flags,
                                          .nr_samples = nr_samples,
                                          .width = width,
                                          .height = height,
                                          .depth = depth,
                                          .array_size = array_size,
                                          .last_level = last_level,
                                          .target = target};

   if (can_cache_resource(bind)) {
      mtx_lock(&qdws->cacheMtx);

      entry = virgl_resource_cache_remove_compatible(&qdws->cache, params);
      if (entry) {
         res = cache_entry_container_res(entry);
         mtx_unlock(&qdws->cacheMtx);
         pipe_reference_init(&res->reference, 1);
         return res;
      }

      mtx_unlock(&qdws->cacheMtx);
   }

   res = virgl_gdi_winsys_resource_create(qws, target, format, bind, width,
                                          height, depth, array_size, last_level,
                                          nr_samples, flags, size);
   return res;
}

static uint32_t
virgl_gdi_winsys_resource_get_storage_size(struct virgl_winsys *qws,
                                           struct virgl_hw_res *res)
{
   // This is of course not a real size, but this function
   // is called only for imported resources, to determine
   // whether we should use staging path, and due to
   // D3DKMTLock implications we have to use staging path
   // on any imported resource
   return 0;
}

static struct virgl_hw_res *
virgl_gdi_winsys_resource_create_handle(struct virgl_winsys *qws,
                                        struct winsys_handle *whandle,
                                        struct pipe_resource *templ,
                                        uint32_t *plane, uint32_t *stride,
                                        uint32_t *plane_offset,
                                        uint64_t *modifier, uint32_t *blob_mem)
{
   if (whandle->type != WINSYS_HANDLE_TYPE_WIN32_HANDLE)
      return NULL;

   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);
   struct virgl_hw_res *res = NULL;

   struct gdikmt_openallocation openAllocation;
   openAllocation.hGlobalHandle = (D3DKMT_HANDLE)whandle->handle;

   NTSTATUS Status =
      qdws->device->queryAllocation(qdws->device, &openAllocation);
   if (!NT_SUCCESS(Status)) {
      return NULL;
   }

   D3DDDI_OPENALLOCATIONINFO *openAllocationInfo =
      CALLOC(sizeof(D3DDDI_OPENALLOCATIONINFO), openAllocation.NumAllocations);

   openAllocation.pOpenAllocation = openAllocationInfo;
   openAllocation.pPrivateDriverData =
      malloc(openAllocation.PrivateDriverDataSize);
   openAllocation.pTotalBuffer = malloc(openAllocation.TotalBufferSize);

   Status = qdws->device->openAllocation(qdws->device, &openAllocation);

   if (!NT_SUCCESS(Status))
      goto free_buffers;

   const VIOGPU_CREATE_ALLOCATION_EXCHANGE *AllocExchange =
      openAllocationInfo[0].pPrivateDriverData;

   templ->target = AllocExchange->ResourceOptions.target;
   templ->format = virgl_to_pipe_format(AllocExchange->ResourceOptions.format);
   templ->bind = virgl_to_pipe_bind(AllocExchange->ResourceOptions.bind);
   templ->width0 = AllocExchange->ResourceOptions.width;
   templ->height0 = AllocExchange->ResourceOptions.height;
   templ->depth0 = AllocExchange->ResourceOptions.depth;
   templ->array_size = AllocExchange->ResourceOptions.array_size;
   templ->last_level = AllocExchange->ResourceOptions.last_level;
   templ->nr_samples = AllocExchange->ResourceOptions.nr_samples;
   templ->flags = virgl_to_pipe_flags(AllocExchange->ResourceOptions.flags);

   res = CALLOC_STRUCT(virgl_hw_res);
   if (!res)
      goto free_buffers;

   *plane = whandle->plane;
   *stride = whandle->stride;
   *plane_offset = whandle->offset;
   *modifier = whandle->modifier;

   res->hAllocation = openAllocationInfo[0].hAllocation;

   VIOGPU_ESCAPE resid_escape;
   resid_escape.Type = VIOGPU_RES_INFO;
   resid_escape.DataLength = sizeof(VIOGPU_RES_INFO_REQ);
   resid_escape.ResourceInfo.ResHandle = res->hAllocation;

   Status =
      qdws->device->escape(qdws->device, &resid_escape, sizeof(resid_escape));
   if (!NT_SUCCESS(Status)) {
      _debug_printf(
         "Failed to get resource id(D3DKMTEscape@VIOGPU_RESID_REQ) with status code: %lx\n",
         Status);

      free(res);
      res = NULL;
      goto free_buffers;
   }
   res->res_handle = resid_escape.ResourceInfo.Id;
   res->blob_mem = 0;
   *blob_mem = 0;

   res->size = AllocExchange->Size;
   res->maybe_untyped = false;
   pipe_reference_init(&res->reference, 1);
   p_atomic_set(&res->shared, true);
   res->num_cs_references = 0;

free_buffers:
   free(openAllocation.pOpenAllocation);
   free(openAllocation.pPrivateDriverData);
   free(openAllocation.pTotalBuffer);

   return res;
}

static struct pipe_fence_handle *
virgl_gdi_fence_create(struct virgl_winsys *vws, HANDLE handle, bool external)
{
   struct virgl_gdi_fence *fence;

   fence = CALLOC_STRUCT(virgl_gdi_fence);
   if (!fence) {
      CloseHandle(handle);
      return NULL;
   }

   if (external) {
      DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                      &fence->handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
   } else {
      fence->handle = handle;
   }

   fence->external = external;

   pipe_reference_init(&fence->reference, 1);

   return (struct pipe_fence_handle *)fence;
}

static bool
virgl_gdi_fence_wait(struct virgl_winsys *vws, struct pipe_fence_handle *_fence,
                     uint64_t timeout)
{
   struct virgl_gdi_fence *fence = virgl_gdi_fence(_fence);
   DWORD timeout_ms;

   if (timeout > INFINITE) {
      timeout = INFINITE;
   } else {
      timeout_ms = timeout / 1000000;
      /* round up */
      if (timeout_ms * 1000000 < timeout)
         timeout_ms++;
   }

   return WaitForSingleObject(fence->handle, timeout) == STATUS_WAIT_0;
}

static void
virgl_gdi_fence_reference(struct virgl_winsys *vws,
                          struct pipe_fence_handle **dst,
                          struct pipe_fence_handle *src)
{
   struct virgl_gdi_fence *dfence = virgl_gdi_fence(*dst);
   struct virgl_gdi_fence *sfence = virgl_gdi_fence(src);

   if (pipe_reference(&dfence->reference, &sfence->reference)) {
      CloseHandle(dfence->handle);
      FREE(dfence);
   }

   *dst = src;
}

static boolean virgl_gdi_winsys_resource_get_handle(struct virgl_winsys *qws,
                                                    struct virgl_hw_res *res,
                                                    uint32_t stride,
                                                    struct winsys_handle
                                                    *whandle)
 {
   if (!res)
       return FALSE;

   if (whandle->type == WINSYS_HANDLE_TYPE_WIN32_HANDLE) {
      if(!res->shared) return FALSE;

      /* TODO: Implement exporting handles for resources.
               Exporting shared handles only useful for non directx runtimes
               But currently there is no way to request handle export from opengl 
      */
      whandle->handle = 0;
   } else if(whandle->type == WINSYS_HANDLE_TYPE_D3DKMT_ALLOC) {
      whandle->handle = (HANDLE)res->hAllocation;
   }
   
   whandle->stride = stride;
   return TRUE;
}

static void *
virgl_gdi_resource_map(struct virgl_winsys *qws, struct virgl_hw_res *res)
{
   if (res->ptr)
      return res->ptr;

   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);

   D3DDDICB_LOCKFLAGS flags;
   flags.Value = 0;
   flags.IgnoreSync = true;

   NTSTATUS Status = qdws->device->lockAllocation(
      qdws->device, res->hAllocation, flags, &res->ptr);
   if (!NT_SUCCESS(Status)) {
      _debug_printf(
         "Failed to map allocation(D3DKMTLock) with status code: %lx\n",
         Status);
      return NULL;
   }

   return res->ptr;
}

static void
virgl_gdi_resource_wait(struct virgl_winsys *qws, struct virgl_hw_res *res)
{

   if (!p_atomic_read(&res->maybe_busy) && !p_atomic_read(&res->shared))
      return;

   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);

   VIOGPU_ESCAPE resid_escape;
   resid_escape.Type = VIOGPU_RES_BUSY;
   resid_escape.DataLength = sizeof(VIOGPU_RES_BUSY_REQ);
   resid_escape.ResourceBusy.ResHandle = res->hAllocation;
   resid_escape.ResourceBusy.Wait = TRUE;

   NTSTATUS Status =
      qdws->device->escape(qdws->device, &resid_escape, sizeof(resid_escape));

   if (!NT_SUCCESS(Status)) {
      _debug_printf(
         "Failed to check if allocation is busy with status code: %lx\n",
         Status);
   }

   p_atomic_set(&res->maybe_busy, false);

   return;
}

static void
virgl_gdi_emit_res(struct virgl_winsys *qws, struct virgl_cmd_buf *_cbuf,
                   struct virgl_hw_res *res, boolean write_buf)
{
   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);
   struct virgl_gdi_cmd_buf *cbuf = virgl_gdi_cmd_buf(_cbuf);
   boolean already_in_list = false;
   for (int i = 0; i < cbuf->alloc_count; i++) {
      if (cbuf->ctx->pAllocationList[i].hAllocation == res->hAllocation) {
         already_in_list = true;
         break;
      }
   }

   if (write_buf)
      cbuf->base.buf[cbuf->base.cdw++] = res->res_handle;
   if (!already_in_list) {
      assert(cbuf->alloc_count <= cbuf->max_alloc);

      memset(&cbuf->ctx->pAllocationList[cbuf->alloc_count], 0,
             sizeof(D3DDDI_ALLOCATIONLIST));
      cbuf->ctx->pAllocationList[cbuf->alloc_count].hAllocation =
         res->hAllocation;

      memset(&cbuf->ctx->pPatchLocationList[cbuf->alloc_count], 0,
             sizeof(D3DDDI_PATCHLOCATIONLIST));
      cbuf->ctx->pPatchLocationList[cbuf->alloc_count].AllocationIndex =
         cbuf->alloc_count;

      cbuf->res_bo[cbuf->alloc_count] = NULL;
      virgl_gdi_resource_reference(&qdws->base,
                                   &cbuf->res_bo[cbuf->alloc_count], res);

      p_atomic_inc(&res->num_cs_references);

      cbuf->alloc_count++;
   }
}

static boolean
virgl_gdi_res_is_ref(struct virgl_winsys *qws, struct virgl_cmd_buf *_cbuf,
                     struct virgl_hw_res *res)
{
   if (!p_atomic_read(&res->num_cs_references))
      return FALSE;

   return TRUE;
}

static struct virgl_cmd_buf *
virgl_gdi_cmd_buf_create(struct virgl_winsys *qws, uint32_t size)
{
   struct virgl_gdi_cmd_buf *cbuf;
   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);

   cbuf = CALLOC_STRUCT(virgl_gdi_cmd_buf);
   if (!cbuf)
      return NULL;

   cbuf->ws = qws;
   NTSTATUS Status = qdws->device->createContext(qdws->device, &cbuf->ctx);

   if (!NT_SUCCESS(Status)) {
      _debug_printf(
         "Failed to create D3DKMTCreateContext with status code: %lx\n",
         Status);
      FREE(cbuf);
      return NULL;
   }

   // Resize buffer to suit our needs
   memset(cbuf->ctx->pCommandBuffer, 0,
          sizeof(VIOGPU_COMMAND_HDR)); // Create nop header
   struct gdikmt_render render;
   memset(&render, 0, sizeof(render));
   render.CommandLength = sizeof(VIOGPU_COMMAND_HDR);
   render.ResizeCommandBuffer = true;
   render.ResizeAllocationList = true;
   render.ResizePatchLocationList = true;

   render.NewCommandBufferSize = size * 4 + 0x100;
   render.NewAllocationListSize = 1024;
   render.NewPatchLocationListSize = 1024;
   Status = cbuf->ctx->render(cbuf->ctx, &render);
   if (!NT_SUCCESS(Status)) {
      _debug_printf(
         "Failed to resize cmdbuf(D3DKMTRender) with status code: %lx\n",
         Status);
      FREE(cbuf);
      return NULL;
   }

   cbuf->driver_length = 0;

   cbuf->max_alloc = render.NewAllocationListSize;
   cbuf->res_bo =
      CALLOC(render.NewAllocationListSize, sizeof(struct virgl_hw_res *));

   cbuf->base.buf =
      (uint8_t *)cbuf->ctx->pCommandBuffer + sizeof(VIOGPU_COMMAND_HDR);
   return &cbuf->base;
}

static void
virgl_gdi_cmd_buf_destroy(struct virgl_cmd_buf *_cbuf)
{
   struct virgl_gdi_cmd_buf *cbuf = virgl_gdi_cmd_buf(_cbuf);

   for (int i = 0; i < cbuf->alloc_count; i++) {
      p_atomic_dec(&cbuf->res_bo[i]->num_cs_references);
      virgl_gdi_resource_reference(cbuf->ws, &cbuf->res_bo[i], NULL);
   }

   cbuf->ctx->destroy(cbuf->ctx);
   FREE(cbuf);
}

static int
virgl_gdi_winsys_submit_cmd(struct virgl_winsys *qws,
                            struct virgl_cmd_buf *_cbuf,
                            struct pipe_fence_handle **fence)
{
   struct virgl_gdi_cmd_buf *cbuf = virgl_gdi_cmd_buf(_cbuf);

   struct gdikmt_render render;
   memset(&render, 0, sizeof(render));

   if (cbuf->driver_length == 0) {
      VIOGPU_COMMAND_HDR *cmdHdr = cbuf->ctx->pCommandBuffer;
      cmdHdr->type = VIOGPU_CMD_SUBMIT;
      cmdHdr->size = cbuf->base.cdw * 4;
      render.CommandLength = sizeof(VIOGPU_COMMAND_HDR) + cbuf->base.cdw * 4;
   } else {
      render.CommandLength = cbuf->driver_length;
   }

   render.AllocationCount = cbuf->alloc_count;
   render.PatchLocationCount = cbuf->alloc_count;

   if (fence) {
      HANDLE event = CreateEventA(NULL, TRUE, FALSE, NULL);
      if (event) {
         *fence = virgl_gdi_fence_create(qws, event, false);
         render.CompletionEvent = event;
      }
   }

   NTSTATUS Status = cbuf->ctx->render(cbuf->ctx, &render);
   if (!NT_SUCCESS(Status)) {
      _debug_printf(
         "Failed to submit cmdbuf(D3DKMTRender) with status code: %lx\n",
         Status);
      return -1;
   }

   cbuf->base.buf =
      (uint8_t *)cbuf->ctx->pCommandBuffer + sizeof(VIOGPU_COMMAND_HDR);
   cbuf->base.cdw = 0;

   for (int i = 0; i < cbuf->alloc_count; i++) {
      p_atomic_set(&cbuf->res_bo[i]->maybe_busy, true);

      p_atomic_dec(&cbuf->res_bo[i]->num_cs_references);
      virgl_gdi_resource_reference(cbuf->ws, &cbuf->res_bo[i], NULL);
   }

   cbuf->alloc_count = 0;
   return 0;
}

static int
virgl_bo_transfer_put(struct virgl_winsys *vws, struct virgl_hw_res *res,
                      const struct pipe_box *box, uint32_t stride,
                      uint32_t layer_stride, uint32_t buf_offset,
                      uint32_t level)
{
   struct virgl_gdi_winsys *vdws = virgl_gdi_winsys(vws);
   mtx_lock(&vdws->coreMtx);

   uint8_t *pCommandBuffer =
      virgl_gdi_cmd_buf(vdws->coreCtx)->ctx->pCommandBuffer;

   VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)pCommandBuffer;
   cmdHdr->type = VIOGPU_CMD_TRANSFER_TO_HOST;
   cmdHdr->size = sizeof(VIOGPU_TRANSFER_CMD);

   VIOGPU_TRANSFER_CMD *transferCmd =
      (VIOGPU_TRANSFER_CMD *)(pCommandBuffer + sizeof(VIOGPU_COMMAND_HDR));
   transferCmd->res_id = res->res_handle;

   transferCmd->box.x = box->x;
   transferCmd->box.y = box->y;
   transferCmd->box.z = box->z;
   transferCmd->box.width = box->width;
   transferCmd->box.height = box->height;
   transferCmd->box.depth = box->depth;

   transferCmd->stride = stride;
   transferCmd->layer_stride = layer_stride;
   transferCmd->offset = buf_offset;
   transferCmd->level = level;

   virgl_gdi_emit_res(vws, vdws->coreCtx, res, false);

   virgl_gdi_cmd_buf(vdws->coreCtx)->driver_length =
      sizeof(VIOGPU_COMMAND_HDR) + sizeof(VIOGPU_TRANSFER_CMD);

   int result = virgl_gdi_winsys_submit_cmd(vws, vdws->coreCtx, NULL);

   mtx_unlock(&vdws->coreMtx);
   return result;
}

static int
virgl_bo_transfer_get(struct virgl_winsys *vws, struct virgl_hw_res *res,
                      const struct pipe_box *box, uint32_t stride,
                      uint32_t layer_stride, uint32_t buf_offset,
                      uint32_t level)
{
   struct virgl_gdi_winsys *vdws = virgl_gdi_winsys(vws);
   mtx_lock(&vdws->coreMtx);

   uint8_t *pCommandBuffer =
      virgl_gdi_cmd_buf(vdws->coreCtx)->ctx->pCommandBuffer;

   VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)pCommandBuffer;
   cmdHdr->type = VIOGPU_CMD_TRANSFER_FROM_HOST;
   cmdHdr->size = sizeof(VIOGPU_TRANSFER_CMD);

   VIOGPU_TRANSFER_CMD *transferCmd =
      (VIOGPU_TRANSFER_CMD *)(pCommandBuffer + sizeof(VIOGPU_COMMAND_HDR));
   transferCmd->res_id = res->res_handle;

   transferCmd->box.x = box->x;
   transferCmd->box.y = box->y;
   transferCmd->box.z = box->z;
   transferCmd->box.width = box->width;
   transferCmd->box.height = box->height;
   transferCmd->box.depth = box->depth;

   transferCmd->stride = stride;
   transferCmd->layer_stride = layer_stride;
   transferCmd->offset = buf_offset;
   transferCmd->level = level;

   virgl_gdi_emit_res(vws, vdws->coreCtx, res, false);

   virgl_gdi_cmd_buf(vdws->coreCtx)->driver_length =
      sizeof(VIOGPU_COMMAND_HDR) + sizeof(VIOGPU_TRANSFER_CMD);

   int result = virgl_gdi_winsys_submit_cmd(vws, vdws->coreCtx, NULL);

   mtx_unlock(&vdws->coreMtx);
   return result;
}


static void
virgl_gdi_winsys_resource_set_type(struct virgl_winsys *qws,
                                   struct virgl_hw_res *res,
                                   uint32_t format, uint32_t bind,
                                   uint32_t width, uint32_t height,
                                   uint32_t usage, uint64_t modifier,
                                   uint32_t plane_count,
                                   const uint32_t *plane_strides,
                                   const uint32_t *plane_offsets)
{
   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);
   struct virgl_cmd_buf *cbuf = qdws->coreCtx;
   
   if (!res->maybe_untyped) {
      return;
   }
   mtx_lock(&qdws->coreMtx);
   res->maybe_untyped = false;

   assert(plane_count && plane_count <= VIRGL_MAX_PLANE_COUNT);

   cbuf->buf[0] = VIRGL_CMD0(VIRGL_CCMD_PIPE_RESOURCE_SET_TYPE, 0,
   VIRGL_PIPE_RES_SET_TYPE_SIZE(plane_count));
   cbuf->buf[VIRGL_PIPE_RES_SET_TYPE_RES_HANDLE] = res->res_handle,
   cbuf->buf[VIRGL_PIPE_RES_SET_TYPE_FORMAT] = format;
   cbuf->buf[VIRGL_PIPE_RES_SET_TYPE_BIND] = bind;
   cbuf->buf[VIRGL_PIPE_RES_SET_TYPE_WIDTH] = width;
   cbuf->buf[VIRGL_PIPE_RES_SET_TYPE_HEIGHT] = height;
   cbuf->buf[VIRGL_PIPE_RES_SET_TYPE_USAGE] = usage;
   cbuf->buf[VIRGL_PIPE_RES_SET_TYPE_MODIFIER_LO] = (uint32_t)modifier;
   cbuf->buf[VIRGL_PIPE_RES_SET_TYPE_MODIFIER_HI] = (uint32_t)(modifier >> 32);
   for (uint32_t i = 0; i < plane_count; i++) {
      cbuf->buf[VIRGL_PIPE_RES_SET_TYPE_PLANE_STRIDE(i)] = plane_strides[i];
      cbuf->buf[VIRGL_PIPE_RES_SET_TYPE_PLANE_OFFSET(i)] = plane_offsets[i];
   }

   cbuf->cdw = VIRGL_PIPE_RES_SET_TYPE_SIZE(plane_count);

   virgl_gdi_emit_res(qws, cbuf, res, false);
   virgl_gdi_winsys_submit_cmd(qws, cbuf, NULL);

   mtx_unlock(&qdws->coreMtx);
}


static int
virgl_gdi_get_caps(struct virgl_winsys *vws, struct virgl_drm_caps *caps)
{
   struct virgl_gdi_winsys *vdws = virgl_gdi_winsys(vws);
   NTSTATUS ret;
   VIOGPU_ESCAPE args;

   virgl_ws_fill_new_caps_defaults(caps);

   memset(&args, 0, sizeof(args));
   args.Type = VIOGPU_GET_CAPS;
   args.DataLength = sizeof(VIOGPU_CAPSET_REQ);

   args.Capset.CapsetId = 2;
   args.Capset.Size = sizeof(union virgl_caps);
   args.Capset.Capset = (UCHAR *)&caps->caps;

   ret = vdws->device->escape(vdws->device, &args, sizeof(args));
   if (!NT_SUCCESS(ret)) {
      /* Fallback to v1 */
      args.Capset.CapsetId = 2;
      args.Capset.Size = sizeof(struct virgl_caps_v1);
      ret = vdws->device->escape(vdws->device, &args, sizeof(args));
      if (!NT_SUCCESS(ret))
         return -1;
   }
   return 0;
}

static bool
virgl_gdi_resource_cache_entry_is_busy(struct virgl_resource_cache_entry *entry,
                                       void *user_data)
{
   struct virgl_gdi_winsys *qdws = user_data;
   struct virgl_hw_res *res = cache_entry_container_res(entry);

   return virgl_gdi_resource_is_busy(&qdws->base, res);
}

static void
virgl_gdi_resource_cache_entry_release(struct virgl_resource_cache_entry *entry,
                                       void *user_data)
{
   struct virgl_gdi_winsys *qdws = user_data;
   struct virgl_hw_res *res = cache_entry_container_res(entry);

   virgl_hw_res_destroy(qdws, res);
}

static void
virgl_gdi_winsys_flush_frontbuffer(struct virgl_winsys *qws,
                                   struct virgl_cmd_buf *_cmdbuf,
                                   struct virgl_hw_res *res, unsigned level,
                                   unsigned layer, void *winsys_drawable_handle,
                                   struct pipe_box *sub_box)
{
   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);
   struct virgl_gdi_cmd_buf *cmdbuf = virgl_gdi_cmd_buf(_cmdbuf);

   qdws->device->present(cmdbuf->ctx, res->hAllocation, winsys_drawable_handle,
                         sub_box);
}

static void
virgl_gdi_winsys_destroy(struct virgl_winsys *qws)
{
   struct virgl_gdi_winsys *qdws = virgl_gdi_winsys(qws);

   virgl_resource_cache_flush(&qdws->cache);

   virgl_gdi_cmd_buf_destroy(qdws->coreCtx);
   qdws->device->destroy(qdws->device);

   mtx_destroy(&qdws->coreMtx);
   FREE(qdws);
}

static struct virgl_winsys *
virgl_gdi_winsys_create(struct gdikmt_device *device)
{
   static const unsigned CACHE_TIMEOUT_USEC = 1000000;

   NTSTATUS Status;

   struct virgl_gdi_winsys *qdws = CALLOC_STRUCT(virgl_gdi_winsys);
   if (!qdws)
      return NULL;
   qdws->device = device;

   Status = qdws->device->queryAdapterInfo(
      qdws->device, KMTQAITYPE_UMDRIVERPRIVATE, &qdws->adapterInfo,
      sizeof(qdws->adapterInfo));

   if (!NT_SUCCESS(Status)) {
      _debug_printf(
         "Failed to request adapter info(D3DKMTQueryAdapterInfo) with status code: %lx\n",
         Status);
      FREE(qdws);
      return NULL;
   }

   if (qdws->adapterInfo.IamVioGPU != VIOGPU_IAM ||
       !qdws->adapterInfo.Flags.Supports3d) {
      _debug_printf(
         "Invalid adapter info, either driver is not viogpu or it doesn't support 3d\n");
      FREE(qdws);
      return NULL;
   }

   uint64_t supports_capset_virgl =
      ((1 << VIRGL_DRM_CAPSET_VIRGL) & qdws->adapterInfo.SupportedCapsetIDs);
   uint64_t supports_capset_virgl2 =
      ((1 << VIRGL_DRM_CAPSET_VIRGL2) & qdws->adapterInfo.SupportedCapsetIDs);

   if (!supports_capset_virgl && !supports_capset_virgl2) {
      _debug_printf("No virgl contexts available on host");
      FREE(qdws);
      return NULL;
   }

   VIOGPU_ESCAPE ctxInitEscape;
   ctxInitEscape.Type = VIOGPU_CTX_INIT;
   ctxInitEscape.DataLength = sizeof(VIOGPU_CTX_INIT_REQ);
   ctxInitEscape.CtxInit.CapsetID = (supports_capset_virgl2)
                                       ? VIRGL_DRM_CAPSET_VIRGL2
                                       : VIRGL_DRM_CAPSET_VIRGL;

   Status =
      qdws->device->escape(qdws->device, &ctxInitEscape, sizeof(ctxInitEscape));

   if (!NT_SUCCESS(Status)) {
      _debug_printf("Failed to initialize context with status code: %lx\n",
                    Status);
   }

   // Create "core ctx" used for transfers/set_type
   qdws->coreCtx = virgl_gdi_cmd_buf_create(&qdws->base, 1024);
   if (!qdws->coreCtx) {
      _debug_printf("Failed to create core context\n");
      FREE(qdws);
      return NULL;
   }
   mtx_init(&qdws->coreMtx, mtx_plain);

   mtx_init(&qdws->cacheMtx, mtx_plain);
   virgl_resource_cache_init(&qdws->cache, CACHE_TIMEOUT_USEC,
                             virgl_gdi_resource_cache_entry_is_busy,
                             virgl_gdi_resource_cache_entry_release, qdws);

   qdws->base.destroy = virgl_gdi_winsys_destroy;

   qdws->base.get_caps = virgl_gdi_get_caps;

   qdws->base.resource_create = virgl_gdi_winsys_resource_cache_create;
   qdws->base.resource_reference = virgl_gdi_resource_reference;
   qdws->base.resource_create_from_handle =
      virgl_gdi_winsys_resource_create_handle;
   qdws->base.resource_set_type = virgl_gdi_winsys_resource_set_type;
   qdws->base.resource_get_handle = virgl_gdi_winsys_resource_get_handle;
   qdws->base.resource_get_storage_size =
      virgl_gdi_winsys_resource_get_storage_size;
   qdws->base.resource_map = virgl_gdi_resource_map;
   qdws->base.resource_wait = virgl_gdi_resource_wait;
   qdws->base.resource_is_busy = virgl_gdi_resource_is_busy;

   qdws->base.transfer_put = virgl_bo_transfer_put;
   qdws->base.transfer_get = virgl_bo_transfer_get;

   qdws->base.cmd_buf_create = virgl_gdi_cmd_buf_create;
   qdws->base.cmd_buf_destroy = virgl_gdi_cmd_buf_destroy;
   qdws->base.submit_cmd = virgl_gdi_winsys_submit_cmd;

   qdws->base.emit_res = virgl_gdi_emit_res;
   qdws->base.res_is_referenced = virgl_gdi_res_is_ref;

   qdws->base.fence_wait = virgl_gdi_fence_wait;
   qdws->base.fence_reference = virgl_gdi_fence_reference;

   qdws->base.flush_frontbuffer = virgl_gdi_winsys_flush_frontbuffer;

   qdws->base.supports_fences = 0;
   qdws->base.supports_encoded_transfers = 1;
   qdws->base.supports_coherent = 0;

   return &qdws->base;
}

static simple_mtx_t virgl_screen_mutex = SIMPLE_MTX_INITIALIZER;

static void
virgl_gdi_screen_destroy(struct pipe_screen *pscreen)
{
   struct virgl_screen *screen = virgl_screen(pscreen);
   boolean destroy;

   simple_mtx_lock(&virgl_screen_mutex);
   destroy = --screen->refcnt == 0;
   simple_mtx_unlock(&virgl_screen_mutex);

   if (destroy) {
      pscreen->destroy = screen->winsys_priv;
      pscreen->destroy(pscreen);
   }
}

struct pipe_screen *
virgl_gdi_screen_create(struct gdikmt_device *device)
{
   struct pipe_screen *pscreen = NULL;

   simple_mtx_lock(&virgl_screen_mutex);
   struct virgl_winsys *vws;
   vws = virgl_gdi_winsys_create(device);
   if (!vws) {
      // close(dup_fd);
      goto unlock;
   }

   pscreen = virgl_create_screen(vws, NULL);
   if (pscreen) {
      // _mesa_hash_table_insert(fd_tab, intptr_to_pointer(dup_fd), pscreen);

      /* Bit of a hack, to avoid circular linkage dependency,
       * ie. pipe driver having to call in to winsys, we
       * override the pipe drivers screen->destroy():
       */
      virgl_screen(pscreen)->winsys_priv = pscreen->destroy;
      pscreen->destroy = virgl_gdi_screen_destroy;
   }

unlock:
   simple_mtx_unlock(&virgl_screen_mutex);
   return pscreen;
}
