#
# Copyright © 2021 Google, Inc.
#
# SPDX-License-Identifier: MIT

from adreno_gpu import *

# a2xx is really two sub-generations, a20x and a22x, but we don't currently
# capture that in the device-info tables
configs = [
    GPUConfig([
        GPUId(200),
        GPUId(201),
        GPUId(205),
        GPUId(220),
    ], GPUInfo(
        CHIP.A2XX,
        gmem_align_w = 32,  gmem_align_h = 32,
        tile_align_w = 32,  tile_align_h = 32,
        tile_max_w   = 512,
        tile_max_h   = ~0, # TODO
        num_vsc_pipes = 8,
        cs_shared_mem_size = 0,
        num_sp_cores = 0, # TODO
        wave_granularity = 2,
        fibers_per_sp = 0, # TODO
        threadsize_base = 8, # TODO: Confirm this
    )),
]
