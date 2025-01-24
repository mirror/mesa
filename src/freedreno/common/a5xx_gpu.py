#
# Copyright © 2021 Google, Inc.
#
# SPDX-License-Identifier: MIT

from adreno_gpu import *

configs = [
    GPUConfig([
        GPUId(505),
        GPUId(506),
        GPUId(508),
        GPUId(509),
    ], GPUInfo(
        CHIP.A5XX,
        gmem_align_w = 64,  gmem_align_h = 32,
        tile_align_w = 64,  tile_align_h = 32,
        tile_max_w   = 1024, # max_bitfield_val(7, 0, 5)
        tile_max_h   = max_bitfield_val(16, 9, 5),
        num_vsc_pipes = 16,
        cs_shared_mem_size = 32 * 1024,
        num_sp_cores = 1,
        wave_granularity = 2,
        fibers_per_sp = 64 * 16, # Lowest number that didn't fault on spillall fs-varying-array-mat4-col-row-rd.
        highest_bank_bit = 14,
        threadsize_base = 32,
    )),

    GPUConfig([
        GPUId(510),
        GPUId(512),
    ], GPUInfo(
        CHIP.A5XX,
        gmem_align_w = 64,  gmem_align_h = 32,
        tile_align_w = 64,  tile_align_h = 32,
        tile_max_w   = 1024, # max_bitfield_val(7, 0, 5)
        tile_max_h   = max_bitfield_val(16, 9, 5),
        num_vsc_pipes = 16,
        cs_shared_mem_size = 32 * 1024,
        num_sp_cores = 2,
        wave_granularity = 2,
        fibers_per_sp = 64 * 16, # Lowest number that didn't fault on spillall fs-varying-array-mat4-col-row-rd.
        highest_bank_bit = 14,
        threadsize_base = 32,
    )),

    GPUConfig([
        GPUId(530),
        GPUId(540),
    ], GPUInfo(
        CHIP.A5XX,
        gmem_align_w = 64,  gmem_align_h = 32,
        tile_align_w = 64,  tile_align_h = 32,
        tile_max_w   = 1024, # max_bitfield_val(7, 0, 5)
        tile_max_h   = max_bitfield_val(16, 9, 5),
        num_vsc_pipes = 16,
        cs_shared_mem_size = 32 * 1024,
        num_sp_cores = 4,
        wave_granularity = 2,
        fibers_per_sp = 64 * 16, # Lowest number that didn't fault on spillall fs-varying-array-mat4-col-row-rd.
        highest_bank_bit = 15,
        threadsize_base = 32,
    )),
]
