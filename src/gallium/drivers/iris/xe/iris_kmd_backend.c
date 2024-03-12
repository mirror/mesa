/*
 * Copyright © 2023 Intel Corporation
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
#include "iris_kmd_backend.h"

#include <sys/mman.h>

#include "common/intel_debug_identifier.h"
#include "common/intel_gem.h"
#include "dev/intel_debug.h"
#include "iris/iris_bufmgr.h"
#include "iris/iris_batch.h"
#include "iris/iris_context.h"

#include "drm-uapi/xe_drm.h"

#define FILE_DEBUG_FLAG DEBUG_BUFMGR

static uint32_t
xe_gem_create(struct iris_bufmgr *bufmgr,
              const struct intel_memory_class_instance **regions,
              uint16_t regions_count, uint64_t size,
              enum iris_heap heap_flags, unsigned alloc_flags)
{
   /* Xe still don't have support for protected content */
   if (alloc_flags & BO_ALLOC_PROTECTED)
      return -EINVAL;

   uint32_t vm_id = iris_bufmgr_get_global_vm_id(bufmgr);
   vm_id = alloc_flags & BO_ALLOC_SHARED ? 0 : vm_id;

   uint32_t flags = 0;
   /* TODO: we might need to consider scanout for shared buffers too as we
    * do not know what the process this is shared with will do with it
    */
   if (alloc_flags & BO_ALLOC_SCANOUT)
      flags |= DRM_XE_GEM_CREATE_FLAG_SCANOUT;
   if (!intel_vram_all_mappable(iris_bufmgr_get_device_info(bufmgr)) &&
       (heap_flags == IRIS_HEAP_DEVICE_LOCAL_PREFERRED ||
        heap_flags == IRIS_HEAP_DEVICE_LOCAL_CPU_VISIBLE_SMALL_BAR))
      flags |= DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;

   struct drm_xe_gem_create gem_create = {
     .vm_id = vm_id,
     .size = align64(size, iris_bufmgr_get_device_info(bufmgr)->mem_alignment),
     .flags = flags,
   };
   for (uint16_t i = 0; i < regions_count; i++)
      gem_create.placement |= BITFIELD_BIT(regions[i]->instance);

   const struct intel_device_info *devinfo = iris_bufmgr_get_device_info(bufmgr);
   const struct intel_device_info_pat_entry *pat_entry;
   pat_entry = iris_heap_to_pat_entry(devinfo, heap_flags);
   switch (pat_entry->mmap) {
   case INTEL_DEVICE_INFO_MMAP_MODE_WC:
      gem_create.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
      break;
   case INTEL_DEVICE_INFO_MMAP_MODE_WB:
      gem_create.cpu_caching = DRM_XE_GEM_CPU_CACHING_WB;
      break;
   default:
      unreachable("missing");
      gem_create.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
   }

   if (intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_XE_GEM_CREATE,
                   &gem_create))
      return 0;

   return gem_create.handle;
}

static void *
xe_gem_mmap(struct iris_bufmgr *bufmgr, struct iris_bo *bo)
{
   struct drm_xe_gem_mmap_offset args = {
      .handle = bo->gem_handle,
   };
   if (intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_XE_GEM_MMAP_OFFSET, &args))
      return NULL;

   void *map = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    iris_bufmgr_get_fd(bufmgr), args.offset);
   return map != MAP_FAILED ? map : NULL;
}

static inline void
bind_op_set(struct iris_bo *bo, uint32_t op, struct drm_xe_vm_bind_op *bind_op)
{
   struct iris_bufmgr *bufmgr = bo->bufmgr;
   const struct intel_device_info *devinfo = iris_bufmgr_get_device_info(bufmgr);
   uint32_t handle = op == DRM_XE_VM_BIND_OP_UNMAP ? 0 : bo->gem_handle;
   uint64_t range, obj_offset = 0;
   uint32_t flags = 0;

   if (iris_bo_is_imported(bo))
      range = bo->size;
   else
      range = align64(bo->size, devinfo->mem_alignment);

   if (bo->real.userptr) {
      handle = 0;
      obj_offset = (uintptr_t)bo->real.map;
      if (op == DRM_XE_VM_BIND_OP_MAP)
         op = DRM_XE_VM_BIND_OP_MAP_USERPTR;
   }

   if (bo->real.capture)
      flags |= DRM_XE_VM_BIND_FLAG_DUMPABLE;

   bind_op->obj = handle;
   bind_op->obj_offset = obj_offset;
   bind_op->range = range;
   bind_op->addr = intel_48b_address(bo->address);
   bind_op->op = op;
   bind_op->pat_index = iris_heap_to_pat_entry(devinfo, bo->real.heap)->index;
   bind_op->flags = flags;
}

static inline int
xe_gem_vm_bind_op(struct iris_bo *bo, uint32_t op)
{
   struct iris_bufmgr *bufmgr = bo->bufmgr;
   struct intel_bind_timeline *bind_timeline = iris_bufmgr_get_bind_timeline(bufmgr);
   struct drm_xe_sync xe_sync = {
      .handle = intel_bind_timeline_get_syncobj(bind_timeline),
      .type = DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ,
      .flags = DRM_XE_SYNC_FLAG_SIGNAL,
   };
   int ret, fd = iris_bufmgr_get_fd(bufmgr);
   struct drm_xe_vm_bind args = {
      .vm_id = iris_bufmgr_get_global_vm_id(bufmgr),
      .num_syncs = 1,
      .syncs = (uintptr_t)&xe_sync,
      .num_binds = 1,
   };

   bind_op_set(bo, op, &args.bind);
   xe_sync.timeline_value = intel_bind_timeline_bind_begin(bind_timeline);
   ret = intel_ioctl(fd, DRM_IOCTL_XE_VM_BIND, &args);
   intel_bind_timeline_bind_end(bind_timeline);

   if (ret)
      DBG("vm_bind_op: DRM_IOCTL_XE_VM_BIND failed(%i)", ret);

   return ret;
}

static bool
xe_gem_vm_bind(struct iris_bo *bo)
{
   return xe_gem_vm_bind_op(bo, DRM_XE_VM_BIND_OP_MAP) == 0;
}

static bool
xe_gem_vm_unbind(struct iris_bo *bo)
{
   return xe_gem_vm_bind_op(bo, DRM_XE_VM_BIND_OP_UNMAP) == 0;
}

static bool
xe_bo_madvise(struct iris_bo *bo, enum iris_madvice state)
{
   /* Only applicable if VM was created with DRM_XE_VM_CREATE_FAULT_MODE but
    * that is not compatible with DRM_XE_VM_CREATE_SCRATCH_PAGE
    *
    * So returning as retained.
    */
   return true;
}

static int
xe_bo_set_caching(struct iris_bo *bo, bool cached)
{
   /* Xe don't have caching UAPI so this function should never be called */
   assert(0);
   return -1;
}

static enum pipe_reset_status
xe_batch_check_for_reset(struct iris_batch *batch)
{
   enum pipe_reset_status status = PIPE_NO_RESET;
   struct drm_xe_exec_queue_get_property exec_queue_get_property = {
      .exec_queue_id = batch->xe.exec_queue_id,
      .property = DRM_XE_EXEC_QUEUE_GET_PROPERTY_BAN,
   };
   int ret = intel_ioctl(iris_bufmgr_get_fd(batch->screen->bufmgr),
                         DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY,
                         &exec_queue_get_property);

   if (ret || exec_queue_get_property.value)
      status = PIPE_GUILTY_CONTEXT_RESET;

   return status;
}

static uint32_t
xe_batch_submit_external_bo_count(struct iris_batch *batch)
{
   uint32_t count = 0;

   for (int i = 0; i < batch->exec_count; i++) {
      if (iris_bo_is_external(batch->exec_bos[i]))
         count++;
   }

   return count;
}

struct iris_implicit_sync {
   struct iris_implicit_sync_entry {
      struct iris_bo *bo;
      struct iris_syncobj *iris_syncobj;
   } *entries;
   uint32_t entry_count;

   struct iris_syncobj *batch_signal_syncobj;
};

static bool
iris_implicit_sync_add_bo(struct iris_batch *batch,
                          struct iris_implicit_sync *sync,
                          struct iris_bo *bo)
{
   struct iris_syncobj *syncobj = iris_bo_export_sync_state(bo);

   if (!syncobj)
      return false;

   sync->entries[sync->entry_count].bo = bo;
   sync->entries[sync->entry_count].iris_syncobj = syncobj;
   sync->entry_count++;

   iris_batch_add_syncobj(batch, syncobj, IRIS_BATCH_FENCE_WAIT);

   return true;
}

/* Cleans up the state of 'sync'. */
static void
iris_implicit_sync_finish(struct iris_batch *batch,
                          struct iris_implicit_sync *sync)
{
   struct iris_bufmgr *bufmgr = batch->screen->bufmgr;

   for (int i = 0; i < sync->entry_count; i++)
      iris_syncobj_reference(bufmgr, &sync->entries[i].iris_syncobj, NULL);

   free(sync->entries);
   sync->entry_count = 0;
}

/* Import implicit synchronization data from the batch bos that require
 * implicit synchronization int our batch buffer so the batch will wait for
 * these bos to be idle before starting.
 */
static int
iris_implicit_sync_import(struct iris_batch *batch,
                          struct iris_implicit_sync *sync)
{
   uint32_t len = xe_batch_submit_external_bo_count(batch);

   if (!len)
      return 0;

   sync->entries = malloc(sizeof(*sync->entries) * len);
   if (!sync->entries)
      return -ENOMEM;

   for (int i = 0; i < batch->exec_count; i++) {
      struct iris_bo *bo = batch->exec_bos[i];

      if (!iris_bo_is_real(bo) || !iris_bo_is_external(bo)) {
         assert(iris_get_backing_bo(bo)->real.prime_fd == -1);
         continue;
      }

      if (bo->real.prime_fd == -1) {
         fprintf(stderr, "Bo(%s/%i %sported) with prime_fd unset in iris_implicit_sync_import()\n",
                 bo->name, bo->gem_handle, bo->real.imported ? "im" : "ex");
         continue;
      }

      if (!iris_implicit_sync_add_bo(batch, sync, bo)) {
         iris_implicit_sync_finish(batch, sync);
         return -1;
      }
   }

   return 0;
}

/* Export implicit synchronization data from our batch buffer into the bos
 * that require implicit synchronization so other clients relying on it can do
 * implicit synchronization with these bos, which will wait for the batch
 * buffer we just submitted to signal its syncobj.
 */
static bool
iris_implicit_sync_export(struct iris_batch *batch,
                          struct iris_implicit_sync *sync)
{
   int sync_file_fd;

   if (!iris_batch_syncobj_to_sync_file_fd(batch, &sync_file_fd))
      return false;

   for (int i = 0; i < sync->entry_count; i++)
      iris_bo_import_sync_state(sync->entries[i].bo, sync_file_fd);

   close(sync_file_fd);

   return true;
}

static int
low_memory_mode_bind_ops(struct iris_batch *batch, struct drm_xe_vm_bind *bind_args)
{
   struct iris_bufmgr *bufmgr = batch->screen->bufmgr;
   const struct intel_device_info *devinfo = iris_bufmgr_get_device_info(bufmgr);
   struct drm_xe_vm_bind_op *bind_ops;
   uint8_t *array_gem_handle_done;
   int ret = 0;

   bind_ops = calloc(batch->exec_count + 1, sizeof(*bind_ops));
   if (!bind_ops) {
      ret = -ENOMEM;
      goto error_out_of_mem_bind_ops;
   }

   /* more or less copied from i915 but why not use a hashmap? */
   array_gem_handle_done = calloc(batch->max_gem_handle + 1, sizeof(uint8_t));
   if (!array_gem_handle_done) {
      ret = -ENOMEM;
      goto error_out_of_mem_array_gem_handle_done;
   }

   /* Unbind the whole VM */
   bind_ops[bind_args->num_binds].addr = 0x0;
   bind_ops[bind_args->num_binds].range = devinfo->gtt_size;
   bind_ops[bind_args->num_binds].op = DRM_XE_VM_BIND_OP_UNMAP;
   bind_args->num_binds++;

   /* Bind VMAs referenced by iris_batch */
   for (uint32_t i = 0; i < batch->exec_count; i++) {
      struct iris_bo *bo = iris_get_backing_bo(batch->exec_bos[i]);
      assert(bo->gem_handle != 0);

      if (array_gem_handle_done[bo->gem_handle])
         continue;

      bind_op_set(bo, DRM_XE_VM_BIND_OP_MAP, &bind_ops[bind_args->num_binds]);
      bind_args->num_binds++;
      array_gem_handle_done[bo->gem_handle] = 1;
   }

   free(array_gem_handle_done);
   bind_args->vector_of_binds = (uintptr_t)bind_ops;

   return ret;

error_out_of_mem_array_gem_handle_done:
   free(bind_ops);
error_out_of_mem_bind_ops:
   return ret;
}

static int
low_memory_mode_bind_syncs(struct iris_batch *batch, struct drm_xe_vm_bind *bind_args, struct iris_syncobj ***ret_exec_syncobjs)
{
   struct iris_bufmgr *bufmgr = batch->screen->bufmgr;
   struct intel_bind_timeline *bind_timeline = iris_bufmgr_get_bind_timeline(bufmgr);
   struct iris_syncobj **exec_syncobjs;
   struct drm_xe_sync *bind_syncs;
   uint32_t batches_count = 0;
   int ret = 0;

   list_for_each_entry_safe(struct iris_context, ice, iris_bufmgr_get_context_list(bufmgr), list_node) {
      batches_count += IRIS_BATCH_COUNT;
   }

   exec_syncobjs = malloc(sizeof(struct iris_syncobj *) * batches_count);
   if (!exec_syncobjs) {
      ret = -ENOMEM;
      goto error_out_of_mem_array_exec_syncobjs;
   }

   for (uint32_t i = 0; i < batches_count; i++) {
      exec_syncobjs[i] = iris_create_syncobj(bufmgr);
      if (!exec_syncobjs[i]) {
         ret = -ENOMEM;
         goto error_out_of_mem_syncobj;
      }
   }

   bind_syncs = calloc(batches_count + 1, sizeof(*bind_syncs));
   if (!bind_syncs) {
      ret = -ENOMEM;
      goto error_out_of_mem_bind_syncs;
   }

   /* get a signal of the completion of the last XE_EXEC of every EXEC_QUEUE
    * in this bufmgr
    */
   list_for_each_entry_safe(struct iris_context, ice, iris_bufmgr_get_context_list(bufmgr), list_node) {
      for (uint32_t i = 0; i < IRIS_BATCH_COUNT; i++) {
         struct drm_xe_sync exec_sync = {
            .type = DRM_XE_SYNC_TYPE_SYNCOBJ,
            .flags = DRM_XE_SYNC_FLAG_SIGNAL,
            .handle = exec_syncobjs[bind_args->num_syncs]->handle,
         };
         struct drm_xe_exec exec = {
            .exec_queue_id = ice->batches[i].xe.exec_queue_id,
            .num_syncs = 1,
            .syncs = (uintptr_t)&exec_sync,
         };

         /* Using the special exec.num_batch_buffer == 0 handling to get a syncobj
          * signaled when the last DRM_IOCTL_XE_EXEC is completed.
          *
          * Synchronized by xe_batch_submit()->simple_mtx_lock(bo_deps_lock)
          */
         ret = intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_XE_EXEC, &exec);
         if (ret) {
            /* if batch was banned just skip it */
            if (errno == ECANCELED) {
               ret = 0;
               continue;
            }

            ret = -errno;
            goto error_xe_exec;
         }

         /* makes binds wait for last queued DRM_IOCTL_XE_EXEC */
         bind_syncs[bind_args->num_syncs].handle = exec_syncobjs[i]->handle;
         bind_syncs[bind_args->num_syncs].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
         bind_args->num_syncs++;
      }
   }

   /* Regular bind_timeline signal, so real DRM_IOCTL_XE_EXEC waits for this
    * bind.
    * It is important that is the last one so the timeline value is set in the
    * right bind_syncs by the caller.
    */
   bind_syncs[bind_args->num_syncs].handle = intel_bind_timeline_get_syncobj(bind_timeline);
   bind_syncs[bind_args->num_syncs].type = DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ;
   bind_syncs[bind_args->num_syncs].flags = DRM_XE_SYNC_FLAG_SIGNAL;
   bind_args->num_syncs++;

   bind_args->syncs = (uintptr_t)bind_syncs;
   *ret_exec_syncobjs = exec_syncobjs;

   return ret;

error_xe_exec:
   free(bind_syncs);
error_out_of_mem_bind_syncs:
   for (uint32_t i = 0; i < batches_count; i++) {
      if (exec_syncobjs[i])
         iris_syncobj_destroy(bufmgr, exec_syncobjs[i]);
   }
error_out_of_mem_syncobj:
   free(exec_syncobjs);
error_out_of_mem_array_exec_syncobjs:
   return ret;
}

/*
 * In low memory mode before doing a real DRM_IOCTL_XE_EXEC it first unbind
 * all VMAs and then bind only the VMAs needed by iris_batch.
 * This allow us to execute a iris_batch even if it was allocated in current
 * VM more memory than GPU can store.
 */
static int
xe_batch_submit_prepare_low_memory_mode(struct iris_batch *batch)
{
   struct iris_bufmgr *bufmgr = batch->screen->bufmgr;
   struct intel_bind_timeline *bind_timeline = iris_bufmgr_get_bind_timeline(bufmgr);
   simple_mtx_t *context_list_lock = iris_bufmgr_get_context_list_lock(bufmgr);
   struct drm_xe_vm_bind bind_args = {
      .vm_id = iris_bufmgr_get_global_vm_id(bufmgr),
   };
   struct drm_xe_sync *bind_syncs;
   struct drm_xe_vm_bind_op *bind_ops;
   struct iris_syncobj **exec_syncobjs;
   int ret = 0;

   simple_mtx_lock(context_list_lock);

   /* unbind the whole VM and bind bos referenced by batch */
   ret = low_memory_mode_bind_ops(batch, &bind_args);
   if (ret)
      goto error_bind_ops;

   bind_ops = (struct drm_xe_vm_bind_op *)bind_args.vector_of_binds;
   /* get a signal of the completion of the last XE_EXEC of every EXEC_QUEUE
    * in this bufmgr + regular bind_timeline signal, so real DRM_IOCTL_XE_EXEC
    * waits for this bind. */
   ret = low_memory_mode_bind_syncs(batch, &bind_args, &exec_syncobjs);
   if (ret)
      goto error_bind_syncs;

   bind_syncs = (struct drm_xe_sync *)bind_args.syncs;
   bind_syncs[bind_args.num_syncs - 1].timeline_value = intel_bind_timeline_bind_begin(bind_timeline);
   ret = intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_XE_VM_BIND, &bind_args);
   intel_bind_timeline_bind_end(bind_timeline);

   if (ret)
      ret = -errno;

   for (uint32_t i = 0; i < (bind_args.num_syncs - 1); i++) {
      iris_syncobj_destroy(bufmgr, exec_syncobjs[i]);
   }
   free(exec_syncobjs);
error_bind_syncs:
   free(bind_ops);
error_bind_ops:
   simple_mtx_unlock(context_list_lock);
   return ret;
}

static int
xe_batch_submit(struct iris_batch *batch)
{
   struct iris_bufmgr *bufmgr = batch->screen->bufmgr;
   struct intel_bind_timeline *bind_timeline = iris_bufmgr_get_bind_timeline(bufmgr);
   simple_mtx_t *bo_deps_lock = iris_bufmgr_get_bo_deps_lock(bufmgr);
   struct iris_implicit_sync implicit_sync = {};
   struct drm_xe_sync *syncs = NULL;
   unsigned long sync_len;
   int ret, i;

   iris_bo_unmap(batch->bo);

   /* The decode operation may map and wait on the batch buffer, which could
    * in theory try to grab bo_deps_lock. Let's keep it safe and decode
    * outside the lock.
    */
   if (INTEL_DEBUG(DEBUG_BATCH) &&
       intel_debug_batch_in_range(batch->ice->frame))
      iris_batch_decode_batch(batch);

   simple_mtx_lock(bo_deps_lock);

   if (iris_bufmgr_get_low_memory_mode(bufmgr) &&
       !batch->screen->devinfo->no_hw) {
      ret = xe_batch_submit_prepare_low_memory_mode(batch);
      if (ret)
         goto error_prepare;
   }

   iris_batch_update_syncobjs(batch);

   ret = iris_implicit_sync_import(batch, &implicit_sync);
   if (ret)
      goto error_implicit_sync_import;

   sync_len = iris_batch_num_fences(batch) + 1 /* vm bind sync */;
   syncs = calloc(sync_len, sizeof(*syncs));
   if (!syncs) {
      ret = -ENOMEM;
      goto error_no_sync_mem;
   }

   i = 0;
   util_dynarray_foreach(&batch->exec_fences, struct iris_batch_fence, fence) {
      if (fence->flags & IRIS_BATCH_FENCE_SIGNAL)
         syncs[i].flags = DRM_XE_SYNC_FLAG_SIGNAL;

      syncs[i].handle = fence->handle;
      syncs[i].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
      i++;
   }

   syncs[i].handle = intel_bind_timeline_get_syncobj(bind_timeline);
   syncs[i].type = DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ;
   syncs[i].timeline_value = intel_bind_timeline_get_last_point(bind_timeline);

   if ((INTEL_DEBUG(DEBUG_BATCH) &&
        intel_debug_batch_in_range(batch->ice->frame)) ||
       INTEL_DEBUG(DEBUG_SUBMIT)) {
      iris_dump_fence_list(batch);
      iris_dump_bo_list(batch);
   }

   struct drm_xe_exec exec = {
      .exec_queue_id = batch->xe.exec_queue_id,
      .num_batch_buffer = 1,
      .address = batch->exec_bos[0]->address,
      .syncs = (uintptr_t)syncs,
      .num_syncs = sync_len,
   };
   if (!batch->screen->devinfo->no_hw) {
       ret = intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_XE_EXEC, &exec);
       if (ret && (errno == ENOMEM || errno == ENOSPC)) {
          /* if not in low memory mode, activate it and try to submit again */
          if (iris_bufmgr_get_low_memory_mode(bufmgr) == false) {
             iris_bufmgr_enable_low_memory_mode(bufmgr);
             if (xe_batch_submit_prepare_low_memory_mode(batch) == 0)
                ret = intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_XE_EXEC, &exec);
          }
       }
   }

   if (ret) {
      ret = -errno;
      goto error_exec;
   }

   if (!iris_implicit_sync_export(batch, &implicit_sync))
      ret = -1;

error_exec:
   iris_implicit_sync_finish(batch, &implicit_sync);

   simple_mtx_unlock(bo_deps_lock);

   free(syncs);

   for (int i = 0; i < batch->exec_count; i++) {
      struct iris_bo *bo = batch->exec_bos[i];

      bo->idle = false;
      bo->index = -1;

      iris_get_backing_bo(bo)->idle = false;

      iris_bo_unreference(bo);
   }

   return ret;

error_no_sync_mem:
   iris_implicit_sync_finish(batch, &implicit_sync);
error_implicit_sync_import:
error_prepare:
   simple_mtx_unlock(bo_deps_lock);
   return ret;
}

static int
xe_gem_close(struct iris_bufmgr *bufmgr, struct iris_bo *bo)
{
   if (bo->real.userptr)
      return 0;

   struct drm_gem_close close = {
      .handle = bo->gem_handle,
   };
   return intel_ioctl(iris_bufmgr_get_fd(bufmgr), DRM_IOCTL_GEM_CLOSE, &close);
}

static uint32_t
xe_gem_create_userptr(struct iris_bufmgr *bufmgr, void *ptr, uint64_t size)
{
   /* We return UINT32_MAX, because Xe doesn't create handles for userptrs but
    * it needs a gem_handle different than 0 so iris_bo_is_real() returns true
    * for userptr bos.
    * UINT32_MAX handle here will not conflict with an actual gem handle with
    * same id as userptr bos are not put to slab or bo cache.
    */
   return UINT32_MAX;
}

const struct iris_kmd_backend *xe_get_backend(void)
{
   static const struct iris_kmd_backend xe_backend = {
      .gem_create = xe_gem_create,
      .gem_create_userptr = xe_gem_create_userptr,
      .gem_close = xe_gem_close,
      .gem_mmap = xe_gem_mmap,
      .gem_vm_bind = xe_gem_vm_bind,
      .gem_vm_unbind = xe_gem_vm_unbind,
      .bo_madvise = xe_bo_madvise,
      .bo_set_caching = xe_bo_set_caching,
      .batch_check_for_reset = xe_batch_check_for_reset,
      .batch_submit = xe_batch_submit,
   };
   return &xe_backend;
}
