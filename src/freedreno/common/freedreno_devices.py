#
# Copyright © 2021 Google, Inc.
#
# SPDX-License-Identifier: MIT

from mako.template import Template
import sys
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('-p', '--import-path', required=True)
args = parser.parse_args()
sys.path.insert(0, args.import_path)

import adreno_gpu, a2xx_gpu, a3xx_gpu, a4xx_gpu, a5xx_gpu, a6xx_gpu, a7xx_gpu

class State(object):
    def __init__(self):
        # List of unique device-info structs, multiple different GPU ids
        # can map to a single info struct in cases where the differences
        # are not sw visible, or the only differences are parameters
        # queried from the kernel (like GMEM size)
        self.gpu_infos = []

        # Table mapping GPU id to device-info struct
        self.gpus = {}

    def info_index(self, gpu_info):
        i = 0
        for info in self.gpu_infos:
            if gpu_info == info:
                return i
            i += 1
        raise Error("invalid info")

s = State()

configs = []
configs.extend(a2xx_gpu.configs)
configs.extend(a3xx_gpu.configs)
configs.extend(a4xx_gpu.configs)
configs.extend(a5xx_gpu.configs)
configs.extend(a6xx_gpu.configs)
configs.extend(a7xx_gpu.configs)

for config in configs:
    s.gpu_infos.append(config.info)
    for id in config.ids:
        s.gpus[id] = config.info

template = """\
/* Copyright © 2021 Google, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "freedreno_dev_info.h"
#include "util/u_debug.h"
#include "util/log.h"

#include <stdlib.h>

/* Map python to C: */
#define True true
#define False false

%for info in s.gpu_infos:
static const struct fd_dev_info __info${s.info_index(info)} = ${str(info)};
%endfor

static const struct fd_dev_rec fd_dev_recs[] = {
%for id, info in s.gpus.items():
   { {${id.gpu_id}, ${hex(id.chip_id)}}, "${id.name}", &__info${s.info_index(info)} },
%endfor
};

void
fd_dev_info_apply_dbg_options(struct fd_dev_info *info)
{
    const char *env = debug_get_option("FD_DEV_FEATURES", NULL);
    if (!env || !*env)
        return;

    char *features = strdup(env);
    char *feature, *feature_end;
    feature = strtok_r(features, ":", &feature_end);
    while (feature != NULL) {
        char *name, *name_end;
        name = strtok_r(feature, "=", &name_end);

        if (!name) {
            mesa_loge("Invalid feature \\"%s\\" in FD_DEV_FEATURES", feature);
            exit(1);
        }

        char *value = strtok_r(NULL, "=", &name_end);

        feature = strtok_r(NULL, ":", &feature_end);

%for (prop, gen), val in unique_props.items():
  <%
    if isinstance(val, bool):
        parse_value = "debug_parse_bool_option"
    else:
        parse_value = "debug_parse_num_option"
  %>
        if (strcmp(name, "${prop}") == 0) {
            info->${gen}.${prop} = ${parse_value}(value, info->${gen}.${prop});
            continue;
        }
%endfor

        mesa_loge("Invalid feature \\"%s\\" in FD_DEV_FEATURES", name);
        exit(1);
    }

    free(features);
}
"""

print(Template(template).render(s=s, unique_props=adreno_gpu.A6XXProps.unique_props))

