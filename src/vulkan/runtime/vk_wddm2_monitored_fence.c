/*
 * Copyright © 2021 Intel Corporation
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

#include "vk_wddm2_monitored_fence.h"

#include <inttypes.h>

#include "util/os_time.h"
#include "util/u_atomic.h"

#ifndef _WIN32
#include <poll.h>
#include <sys/eventfd.h>
#endif

/* Windows headers conflict with Wayland and XLib headers */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "vk_device.h"
#include "vk_log.h"
#include "vk_queue.h"
#include "vk_util.h"

/* Windows headers need to be included dead last because they have lots of
 * #defines which may mess with other included headers.
 */
#include "wsl/winadapter.h"
#include "d3dkmthk.h"

static struct vk_wddm2_monitored_fence *
to_wddm2_monitored_fence(struct vk_sync *sync)
{
   assert(vk_sync_type_is_wddm2_monitored_fence(sync->type));
   return container_of(sync, struct vk_wddm2_monitored_fence, base);
}

static VkResult
NTSTATUS_to_VkResult(struct vk_device *device,
                     NTSTATUS status)
{
   switch (status) {
   case STATUS_SUCCESS:
      return VK_SUCCESS;
   case STATUS_NO_MEMORY:
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   case STATUS_DEVICE_REMOVED:
      return vk_device_set_lost(device, "Received STATUS_DEVICE_REMOVED");
   default:
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "Unknown NTSTATUS: 0x%x", status);
   }
}

static VkResult
vk_wddm2_monitored_fence_init(struct vk_device *device,
                              struct vk_sync *sync,
                              uint64_t initial_value)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);
   NTSTATUS status;

   D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create = {
      .hDevice = device->wddm2_handle,
      .Info = {
         .Type = D3DDDI_MONITORED_FENCE,
         .Flags = {
            .Shared = (sync->flags & VK_SYNC_IS_SHARED) != 0,
            .NtSecuritySharing = (sync->flags & VK_SYNC_IS_SHARED) != 0,
            /* This gets us 64-bit fences */
            .NoGPUAccess = true,
         },
         .MonitoredFence = {
            .InitialFenceValue = initial_value,
         },
      }
   };
   status = D3DKMTCreateSynchronizationObject2(&create);
   if (unlikely(!NT_SUCCESS(status)))
      return NTSTATUS_to_VkResult(device, status);

   fence->handle = create.hSyncObject;
#ifdef _WIN32
   fence->shared_handle = create.Info.SharedHandle;
#endif
   fence->value_map = create.Info.MonitoredFence.FenceValueCPUVirtualAddress;

   return VK_SUCCESS;
}

static void
vk_wddm2_monitored_fence_finish(struct vk_device *device,
                                struct vk_sync *sync)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);

#ifdef _WIN32
   if (fence->shared_handle) {
      ASSERTED BOOL err = CloseHandle(fence->shared_handle);
      assert(!err);
   }
#endif

   const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {
      .hSyncObject = fence->handle,
   };
   ASSERTED NTSTATUS status = D3DKMTDestroySynchronizationObject(&destroy);
   assert(NT_SUCCESS(status));
}

static VkResult
vk_wddm2_monitored_fence_signal(struct vk_device *device,
                                struct vk_sync *sync,
                                uint64_t value)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);

   assert(value > p_atomic_read(fence->value_map));

   const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU signal = {
      .hDevice = device->wddm2_handle,
      .ObjectCount = 1,
      .ObjectHandleArray = &fence->handle,
      .FenceValueArray = &value,
   };
   NTSTATUS status = D3DKMTSignalSynchronizationObjectFromCpu(&signal);
   if (unlikely(!NT_SUCCESS(status))) {
      vk_wddm2_monitored_fence_finish(device, sync);
      return NTSTATUS_to_VkResult(device, status);
   }

   return VK_SUCCESS;
}

static VkResult
vk_wddm2_monitored_fence_get_value(struct vk_device *device,
                                   struct vk_sync *sync,
                                   uint64_t *value)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);
   *value = p_atomic_read(fence->value_map);
   return VK_SUCCESS;
}

static VkResult
create_async_event(struct vk_device *device, HANDLE *event_out)
{
#ifdef _WIN32
   HANDLE event = CreateEvent(NULL, true, false, NULL);
   if (event == NULL)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "CreateEvent failed");
#else
   int event = eventfd(0, EFD_CLOEXEC);
   if (event < 0)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "eventfd failed: %m");
#endif

   *event_out = (void *)(intptr_t)event;

   return VK_SUCCESS;
}

static VkResult
wait_async_event(struct vk_device *device, HANDLE event,
                 uint64_t rel_timeout_ns)
{
   /* Both poll() and WaitForSingleObject() take a relative timeout in
    * milliseconds as a 32-bit number.  For poll(), it's signed.
    */
   uint64_t rel_timeout_ms = DIV_ROUND_UP(rel_timeout_ns, 1000 * 1000);
   if (rel_timeout_ms > INT32_MAX)
      rel_timeout_ms = INT32_MAX;

#ifdef _WIN32
   DWORD ret = WaitForSingleObject(async_event, rel_timeout_ms);
   switch (ret) {
   case WAIT_TIMEOUT:
      return VK_TIMEOUT;
   case WAIT_OBJECT_0:
      return VK_SUCCESS;
   default:
      return vk_error(device, VK_ERROR_UNKNOWN,
                      "WaitForSingleObject failed with 0x%x", ret);
   }
#else
   struct pollfd event_poll = {
      .fd = (intptr_t)event,
      .events = POLLIN,
   };
   int ret = poll(&event_poll, 1, rel_timeout_ms);
   if (ret < 0 && (errno == EINTR || errno == EAGAIN)) {
      /* Treat this as an early timeout.  The caller loops anyway */
      return VK_TIMEOUT;
   } else if (ret < 0) {
      return vk_errorf(device, VK_ERROR_UNKNOWN, "poll failed: %m");
   } else if (ret > 0) {
      assert(event_poll.revents & POLLIN);
      return VK_SUCCESS;
   } else {
      /* No events */
      return VK_TIMEOUT;
   }
#endif
}

static void
close_async_event(HANDLE event)
{
#ifdef _WIN32
   CloseHandle(event);
#else
   close((intptr_t)event);
#endif
}

static VkResult
vk_wddm2_monitored_fence_wait_many(struct vk_device *device,
                                   uint32_t wait_count,
                                   const struct vk_sync_wait *waits,
                                   enum vk_sync_wait_flags wait_flags,
                                   uint64_t abs_timeout_ns)
{
   VkResult result;
   NTSTATUS status;

   /* Quick poll all the fences ourselves.  We may not have to call into the
    * kernel at all.
    */
   uint32_t ready = 0;
   for (uint32_t i = 0; i < wait_count; i++) {
      struct vk_wddm2_monitored_fence *fence =
         to_wddm2_monitored_fence(waits[i].sync);

      if (p_atomic_read(fence->value_map) >= waits[i].wait_value)
         ready++;
   }
   if (ready == wait_count || ((wait_flags & VK_SYNC_WAIT_ANY) && ready > 0))
      return VK_SUCCESS;

   if (abs_timeout_ns == 0)
      return VK_TIMEOUT;

   uint64_t now_ns = os_time_get_nano();
   if (abs_timeout_ns <= now_ns)
      return VK_TIMEOUT;

   const bool use_event = (abs_timeout_ns >= INT64_MAX);

   HANDLE async_event = 0;
   if (use_event) {
      result = create_async_event(device, &async_event);
      if (unlikely(result != VK_SUCCESS))
         return result;
   }

   STACK_ARRAY(D3DKMT_HANDLE, handles, wait_count);
   STACK_ARRAY(uint64_t, wait_values, wait_count);

   for (uint64_t i = 0; i < wait_count; i++) {
      handles[i] = to_wddm2_monitored_fence(waits[i].sync)->handle,
      wait_values[i] = waits[i].wait_value;
   }

   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = device->wddm2_handle,
      .ObjectCount = wait_count,
      .ObjectHandleArray = handles,
      .FenceValueArray = wait_values,
      .Flags = {
         .WaitAny = (wait_flags & VK_SYNC_WAIT_ANY) != 0,
      },
      .hAsyncEvent = async_event,
   };
   status = D3DKMTWaitForSynchronizationObjectFromCpu(&wait);

   STACK_ARRAY_FINISH(handles);
   STACK_ARRAY_FINISH(wait_values);

   if (unlikely(!NT_SUCCESS(status))) {
      result = NTSTATUS_to_VkResult(device, status);
      goto fail_close_event;
   }

   if (use_event) {
      /* We loop here for a couple reasons:
       *
       *  1. Windows WaitForSingleObject has a maximum timeout of 49.7 days
       *     and poll() has a maximum timeout of 24.8 days (UINT_MAX and
       *     INT_MAX in milliseconds, respectively).
       *
       *  2. At least poll() can return early due to an interrupt.
       *
       *  3. They're both in milliseconds and we're not 100% sure about OS
       *     rounding so it's safer to do our own check.
       */
      do {
         /* We already know this won't overflow because of the
          * `abs_timeout_ns <= now_ns` case above.
          */
         uint64_t rel_timeout_ns = abs_timeout_ns - now_ns;

         result = wait_async_event(device, async_event, rel_timeout_ns);
         if (unlikely(result != VK_SUCCESS))
            goto fail_close_event;

         now_ns = os_time_get_nano();
      } while (abs_timeout_ns <= now_ns);
   }

fail_close_event:
   if (use_event)
      close_async_event(async_event);

   return result;
}

#ifdef _WIN32
static VkResult
vk_wddm2_monitored_fence_import_opaque_win32_handle(struct vk_device *device,
                                                    struct vk_sync *sync,
                                                    HANDLE handle)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);

   D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 open = {
      .hNtHandle = handle,
      .hDevice = device->wddm2_handle,
      .Flags = {
         .Shared = shared,
         .NtSecuritySharing = shared,
         /* This gets us 64-bit fences */
         .NoGPUAccess = true,
      },
   };
   status = D3DKMTOpenSynchronizationObjectFromNtHandle2(&open);
   if (unlikely(!NT_SUCCESS(status)))
      return NTSTATUS_to_VkResult(device, status);

   vk_wddm2_monitored_fence_finish(device, sync);

   fence->handle = open.hSyncObject;
   fence->shared_handle = handle;
   fence->value_map = open.MonitoredFence.FenceValueCPUVirtualAddress;

   return VK_SUCCESS;
}

static VkResult
vk_wddm2_monitored_fence_export_opaque_win32_handle(struct vk_device *device,
                                                    struct vk_sync *sync,
                                                    HANDLE *handle)
{
   struct vk_wddm2_monitored_fence *fence = to_wddm2_monitored_fence(sync);

   HANDLE process = GetCurrentProcess();
   BOOL err = DuplicateHandle(process, fence->shared_handle,
                              process, handle, 0, false,
                              DUPLICATE_SAME_ACCESS);
   if (err)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "DuplicateHandle failed");

   return VK_SUCCESS;
}
#endif

const struct vk_sync_type vk_wddm2_monitored_fence_type = {
   .size = sizeof(struct vk_wddm2_monitored_fence),
   .features = VK_SYNC_FEATURE_TIMELINE |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_ANY |
               VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL,
   .init = vk_wddm2_monitored_fence_init,
   .finish = vk_wddm2_monitored_fence_finish,
   .signal = vk_wddm2_monitored_fence_signal,
   .get_value = vk_wddm2_monitored_fence_get_value,
   .wait_many = vk_wddm2_monitored_fence_wait_many,
};

VkResult
vk_wddm2_check_device_status(struct vk_device *device)
{
   NTSTATUS status;

   D3DKMT_GETDEVICESTATE get_state = {
      .hDevice = device->wddm2_handle,
      .StateType = D3DKMT_DEVICESTATE_EXECUTION,
   };
   status = D3DKMTGetDeviceState(&get_state);
   if (unlikely(!NT_SUCCESS(status))) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "D3DKMTGetDeviceState failed");
   }

   switch (get_state.ExecutionState) {
   case D3DKMT_DEVICEEXECUTION_ACTIVE:
      return VK_SUCCESS;
   case D3DKMT_DEVICEEXECUTION_RESET:
      return vk_device_set_lost(device, "Device was reset");
   case D3DKMT_DEVICEEXECUTION_HUNG:
      return vk_device_set_lost(device, "Device is hung");
   case D3DKMT_DEVICEEXECUTION_STOPPED:
      return vk_device_set_lost(device, "Device is stopped");
   case D3DKMT_DEVICEEXECUTION_ERROR_OUTOFMEMORY:
      return vk_device_set_lost(device, "Device ran out of memory");
   case D3DKMT_DEVICEEXECUTION_ERROR_DMAFAULT:
      return vk_device_set_lost(device, "Device DMA fault");
   case D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT:
      get_state.StateType = D3DKMT_DEVICESTATE_PAGE_FAULT;
      status = D3DKMTGetDeviceState(&get_state);
      if (unlikely(!NT_SUCCESS(status))) {
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "D3DKMTGetDeviceState failed");
      }
      D3DKMT_DEVICEPAGEFAULT_STATE fault = get_state.PageFaultState;

      if (fault.FaultedVirtualAddress) {
         return vk_device_set_lost(device, "Device page fault at 0x%"PRIx64,
                                   fault.FaultedVirtualAddress);
      }

      return vk_device_set_lost(device, "Unknown device page fault");
   default:
      return vk_device_set_lost(device, "Unknown device error");
   }
}

VkResult
vk_wddm2_monitored_fence_gpu_wait_many(struct vk_queue *queue,
                                       uint32_t context_handle,
                                       uint32_t wait_count,
                                       const struct vk_sync_wait *waits)
{
   if (wait_count == 0)
      return VK_SUCCESS;

   STACK_ARRAY(D3DKMT_HANDLE, handles, wait_count);
   STACK_ARRAY(uint64_t, wait_values, wait_count);

   for (uint32_t i = 0; i < wait_count; i++) {
      handles[i] = to_wddm2_monitored_fence(waits[i].sync)->handle;
      wait_values[i] = waits[i].wait_value;
   }

   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU gpu_wait = {
      .hContext = context_handle,
      .ObjectCount = wait_count,
      .ObjectHandleArray = handles,
      .MonitoredFenceValueArray = wait_values,
   };
   NTSTATUS status = D3DKMTWaitForSynchronizationObjectFromGpu(&gpu_wait);

   STACK_ARRAY_FINISH(handles);
   STACK_ARRAY_FINISH(wait_values);

   if (unlikely(!NT_SUCCESS(status))) {
      return vk_queue_set_lost(queue,
         "D3DKMTWaitForSynchronizationObjectFromGpu failed");
   }

   return VK_SUCCESS;
}

VkResult
vk_wddm2_monitored_fence_gpu_signal_many(struct vk_queue *queue,
                                         uint32_t context_handle,
                                         uint32_t signal_count,
                                         const struct vk_sync_signal *signals)
{
   if (signal_count == 0)
      return VK_SUCCESS;

   STACK_ARRAY(D3DKMT_HANDLE, handles, signal_count);
   STACK_ARRAY(uint64_t, signal_values, signal_count);

   for (uint32_t i = 0; i < signal_count; i++) {
      struct vk_wddm2_monitored_fence *fence =
         to_wddm2_monitored_fence(signals[i].sync);

      assert(signals[i].signal_value > p_atomic_read(fence->value_map));

      handles[i] = fence->handle;
      signal_values[i] = signals[i].signal_value;
   }

   const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU gpu_signal = {
      .hContext = context_handle,
      .ObjectCount = signal_count,
      .ObjectHandleArray = handles,
      .MonitoredFenceValueArray = signal_values,
   };
   NTSTATUS status = D3DKMTSignalSynchronizationObjectFromGpu(&gpu_signal);

   STACK_ARRAY_FINISH(handles);
   STACK_ARRAY_FINISH(signal_values);

   if (unlikely(!NT_SUCCESS(status))) {
      return vk_queue_set_lost(queue,
         "D3DKMTSignalSynchronizationObjectFromGpu failed");
   }

   return VK_SUCCESS;
}
