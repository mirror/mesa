#
# Copyright © 2021 Google, Inc.
#
# SPDX-License-Identifier: MIT

from adreno_gpu import *

configs = [
    GPUConfig([
        GPUId(405),
        GPUId(420),
        GPUId(430),
    ], GPUInfo(
        CHIP.A4XX,
        gmem_align_w = 32,  gmem_align_h = 32,
        tile_align_w = 32,  tile_align_h = 32,
        tile_max_w   = 1024, # max_bitfield_val(4, 0, 5)
        tile_max_h   = max_bitfield_val(9, 5, 5),
        num_vsc_pipes = 8,
        cs_shared_mem_size = 32 * 1024,
        num_sp_cores = 0, # TODO
        wave_granularity = 2,
        fibers_per_sp = 0, # TODO
        threadsize_base = 32, # TODO: Confirm this
    )),
]
