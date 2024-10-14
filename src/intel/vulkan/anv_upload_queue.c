/* Copyright © 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "anv_private.h"

static VkResult
anv_device_upload_garbage_collect_locked(struct anv_device *device,
                                         bool wait_completion)
{
   struct anv_device_upload *upload = &device->upload;

   uint64_t last_value;
   if (!wait_completion) {
      VkResult result =
         vk_sync_get_value(&device->vk, upload->timeline, &last_value);
      if (result != VK_SUCCESS)
         return result;

      /* Valgrind doesn't know that drmSyncobjQuery writes to 'last_value' on
       * success.
       */
      VG(VALGRIND_MAKE_MEM_DEFINED(&last_value, sizeof(last_value)));
   } else {
      last_value = upload->timeline_val;
   }

   list_for_each_entry_safe(struct anv_async_submit, submit,
                            &upload->in_flight_uploads, link) {
      if (submit->signal.signal_value <= last_value) {
         list_del(&submit->link);
         anv_async_submit_destroy(submit);
         continue;
      }

      if (!wait_completion)
         break;

      VkResult result = vk_sync_wait(
         &device->vk,
         submit->signal.sync,
         submit->signal.signal_value,
         VK_SYNC_WAIT_COMPLETE,
         os_time_get_absolute_timeout(OS_TIMEOUT_INFINITE));
      if (result == VK_SUCCESS) {
         list_del(&submit->link);
         anv_async_submit_destroy(submit);
         continue;
      }

      /* If the wait failed but the caller wanted completion, return the
       * error.
       */
      return result;
   }

   return VK_SUCCESS;
}

static VkResult
anv_device_upload_flush_locked(struct anv_device *device,
                               uint64_t *timeline_val_out)
{
   struct anv_async_submit *submit = device->upload.submit;
   VkResult result;

   /* Not work has been queued */
   if (submit == NULL) {
      *timeline_val_out = device->upload.timeline_val;
      return VK_SUCCESS;
   }

   device->upload.submit = NULL;

   anv_genX(device->info, emit_memcpy_end)(
      &device->upload.memcpy_state);

   if (anv_batch_has_error(&submit->batch)) {
      result = submit->batch.status;
      anv_async_submit_destroy(submit);
      return result;
   }

   anv_async_submit_set_signal(submit,
                               device->upload.timeline,
                               ++device->upload.timeline_val);

   result = device->kmd_backend->queue_exec_async(submit,
                                                  0, NULL,
                                                  0, NULL);
   if (result != VK_SUCCESS) {
      anv_async_submit_destroy(submit);
      return result;
   }

   /* If u_trace is active, flush the data into the utrace thead and it'll do
    * the free on completion, otherwise add it to the in-flight queue to be
    * garbage collected later.
    */
   if (u_trace_should_process(&device->ds.trace_context)) {
      intel_ds_queue_flush_data(&submit->queue->ds,
                                &submit->ds.trace,
                                &submit->ds, device->vk.current_frame,
                                true);
   } else {
      list_addtail(&submit->link, &device->upload.in_flight_uploads);
   }

   *timeline_val_out = device->upload.timeline_val;

   return VK_SUCCESS;
}

VkResult
anv_device_upload_flush(struct anv_device *device,
                        uint64_t *timeline_val_out)
{
   if (!device->physical->use_shader_upload) {
      *timeline_val_out = 0;
      return VK_SUCCESS;
   }

   VkResult result;

   simple_mtx_lock(&device->upload.mutex);
   result = anv_device_upload_garbage_collect_locked(device, false);
   if (result == VK_SUCCESS)
      result = anv_device_upload_flush_locked(device, timeline_val_out);
   simple_mtx_unlock(&device->upload.mutex);

   return result;
}

VkResult
anv_device_upload_data(struct anv_device *device,
                       struct anv_address dst_addr,
                       const void *data,
                       size_t size)
{
   assert(device->physical->use_shader_upload);

   VkResult result = VK_SUCCESS;
   struct anv_async_submit *submit;

   simple_mtx_lock(&device->upload.mutex);

   anv_device_upload_garbage_collect_locked(device, false);

   if (device->upload.submit == NULL) {
      result = anv_async_submit_create(&device->internal_queue,
                                       &device->batch_bo_pool,
                                       false, false, &submit);
      if (result != VK_SUCCESS)
         goto unlock;
   } else {
      /* Append to the existing batch */
      submit = device->upload.submit;
   }

   if (device->upload.submit == NULL) {
      anv_genX(device->info, emit_memcpy_init)(
         &device->upload.memcpy_state, device, NULL, &submit->batch,
         &submit->dynamic_state_stream, &submit->general_state_stream);
   }

   assert(size % 4 == 0);

   while (size > 0) {
      uint32_t cp_size = MIN2(size, submit->general_state_stream.block_size);
      struct anv_state src_state = anv_state_stream_alloc(
         &submit->general_state_stream, align64(cp_size, 64), 64);
      struct anv_address src_addr =
         anv_state_pool_state_address(&device->general_state_pool,
                                      src_state);

      memcpy(src_state.map, data, cp_size);

      anv_genX(device->info, emit_memcpy)(&device->upload.memcpy_state,
                                          dst_addr, src_addr, cp_size);

      data += cp_size;
      dst_addr = anv_address_add(dst_addr, cp_size);
      size -= cp_size;
   }

   if (anv_batch_has_error(&submit->batch))
      goto free_submit;

   device->upload.submit = submit;

   simple_mtx_unlock(&device->upload.mutex);

   return VK_SUCCESS;

 free_submit:
   anv_async_submit_destroy(submit);
 unlock:
   simple_mtx_unlock(&device->upload.mutex);

   return result;
}

VkResult
anv_device_upload_init(struct anv_device *device)
{
   if (!device->physical->use_shader_upload)
      return VK_SUCCESS;

   VkResult result = vk_sync_create(&device->vk,
                                    &device->physical->sync_syncobj_type,
                                    VK_SYNC_IS_TIMELINE, 0 /* initial_value */,
                                    &device->upload.timeline);
   if (result != VK_SUCCESS)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   simple_mtx_init(&device->upload.mutex, mtx_plain);

   list_inithead(&device->upload.in_flight_uploads);

   return VK_SUCCESS;
}

void
anv_device_upload_finish(struct anv_device *device)
{
   if (!device->physical->use_shader_upload)
      return;

   anv_device_upload_garbage_collect_locked(device, true);

   if (device->upload.submit)
      anv_async_submit_destroy(device->upload.submit);

   vk_sync_destroy(&device->vk, device->upload.timeline);

   simple_mtx_destroy(&device->upload.mutex);
}
