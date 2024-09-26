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

#include "autoclif_dump.h"
#include "common/v3d_debug.h"
#include "drm-uapi/v3d_drm.h"

#include "v3d_autoclif.h"

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

static uint32_t autoclif_sequence = 0;

static inline char *
get_output_name(const char *capture_type)
{
        char *output_name;

        asprintf(&output_name, "record.%u.%.4u.%s.clif",
                 (uint32_t)getpid(), ++autoclif_sequence, capture_type);

        return output_name;
}

void autoclif_cl_dump(uint32_t qpu_count,
                      struct drm_v3d_submit_cl *submit,
                      autoclif_mem_read mem_read,
                      void *p)
{
        struct v3d_autoclif *va = v3d_autoclif_new(qpu_count, mem_read, p);
        v3d_autoclif_record_bin(va,
                                submit->bcl_start, submit->bcl_end,
                                submit->qma, submit->qms, submit->qts);
        v3d_autoclif_record_render(va,
                                   submit->rcl_start, submit->rcl_end,
                                   submit->qma);
        char *outname = get_output_name("cl");
        v3d_autoclif_write(va, outname);

        free(outname);
        v3d_autoclif_free(va);
}

void autoclif_csd_dump(uint32_t qpu_count,
                       struct drm_v3d_submit_csd *submit,
                       autoclif_mem_read mem_read,
                       void *p)
{
        struct v3d_autoclif *va = v3d_autoclif_new(qpu_count, mem_read, p);
        v3d_autoclif_record_csd(va, submit->cfg);

        char *outname = get_output_name("csd");
        v3d_autoclif_write(va, outname);

        free(outname);
        v3d_autoclif_free(va);
}

void autoclif_tfu_dump(uint32_t qpu_count,
                       struct drm_v3d_submit_tfu *submit,
                       autoclif_mem_read mem_read,
                       void *p)
{
        struct v3d_autoclif *va = v3d_autoclif_new(qpu_count, mem_read, p);
        v3d_autoclif_record_tfu(va,
                                submit->iia, submit->iis, submit->ica,
                                submit->iua, submit->ioa,
                                /* This will be ignored in V3D < 7.1 */
                                submit->v71.ioc,
                                submit->ios, submit->icfg,
                                submit->coef[0], submit->coef[1],
                                submit->coef[2], submit->coef[3]);

        char *outname = get_output_name("tfu");
        v3d_autoclif_write(va, outname);

        free(outname);
        v3d_autoclif_free(va);
}
