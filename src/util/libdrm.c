/*
 * Copyright © 2023 Google, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <errno.h>
#include <stdbool.h>
#include <time.h>

#include "drm-uapi/drm.h"
#include "util/libdrm.h"

static bool
has_syncobj_deadline(int fd)
{
   static int has_deadline = -1;

   if (has_deadline != -1)
      return has_deadline;

   /*
    * Do a dummy wait with no handles to probe whether kernel supports the
    * new _WAIT_DEADLINE flag
    */
   struct drm_syncobj_wait args = {
         .count_handles = 0,
         .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE,
   };
   int ret;

   ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args);

   has_deadline = !ret;

   return has_deadline;
}

int
drm_syncobj_wait(int fd, uint32_t *handles, unsigned num_handles,
                 int64_t timeout_nsec, unsigned flags,
                 uint32_t *first_signaled,
                 int64_t deadline_nsec)
{
   struct drm_syncobj_wait args = {
         .handles = (uintptr_t)handles,
         .timeout_nsec = timeout_nsec,
         .count_handles = num_handles,
         .flags = flags,
   };
   int ret;

   if (deadline_nsec && has_syncobj_deadline(fd)) {
      args.flags |= DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
      args.deadline_nsec = deadline_nsec;
   }

   ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args);
   if (ret < 0)
       return -errno;

   if (first_signaled)
       *first_signaled = args.first_signaled;

   return ret;
}

int
drm_syncobj_timeline_wait(int fd, uint32_t *handles, uint64_t *points,
                          unsigned num_handles,
                          int64_t timeout_nsec, unsigned flags,
                          uint32_t *first_signaled,
                          int64_t deadline_nsec)
{
   struct drm_syncobj_timeline_wait args = {
         .handles = (uintptr_t)handles,
         .points = (uintptr_t)points,
         .timeout_nsec = timeout_nsec,
         .count_handles = num_handles,
         .flags = flags,
   };
   int ret;

   if (deadline_nsec && has_syncobj_deadline(fd)) {
      args.flags |= DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
      args.deadline_nsec = deadline_nsec;
   }

   ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args);
   if (ret < 0)
       return -errno;

   if (first_signaled)
       *first_signaled = args.first_signaled;
   return ret;
}
