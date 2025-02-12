#
# Copyright © 2021 Google, Inc.
#
# SPDX-License-Identifier: MIT

from enum import Enum
from collections import namedtuple

def max_bitfield_val(high, low, shift):
    return ((1 << (high - low)) - 1) << shift

class CHIP(Enum):
    A2XX = 2
    A3XX = 3
    A4XX = 4
    A5XX = 5
    A6XX = 6
    A7XX = 7

class CCUColorCacheFraction(Enum):
    FULL = 0
    HALF = 1
    QUARTER = 2
    EIGHTH = 3


class GPUId(object):
    def __init__(self, gpu_id = None, chip_id = None, name=None):
        if chip_id is None:
            assert(gpu_id is not None)
            val = gpu_id
            core = int(val / 100)
            val -= (core * 100)
            major = int(val / 10)
            val -= (major * 10)
            minor = val
            chip_id = (core << 24) | (major << 16) | (minor << 8) | 0xff
        self.chip_id = chip_id
        if gpu_id is None:
            gpu_id = 0
        self.gpu_id = gpu_id
        if name is None:
            assert(gpu_id != 0)
            name = "FD%d" % gpu_id
        self.name = name

class Struct(object):
    """A helper class that stringifies itself to a 'C' struct initializer
    """
    def __str__(self):
        s = "{"
        for name, value in vars(self).items():
            s += "." + name + "=" + str(value) + ","
        return s + "}"

class GPUInfo(Struct):
    """Base class for any generation of adreno, consists of GMEM layout
       related parameters

       Note that tile_max_h is normally only constrained by corresponding
       bitfield size/shift (ie. VSC_BIN_SIZE, or similar), but tile_max_h
       tends to have lower limits, in which case a comment will describe
       the bitfield size/shift
    """
    def __init__(self, chip, gmem_align_w, gmem_align_h,
                 tile_align_w, tile_align_h,
                 tile_max_w, tile_max_h, num_vsc_pipes,
                 cs_shared_mem_size, num_sp_cores, wave_granularity, fibers_per_sp,
                 highest_bank_bit = 0, ubwc_swizzle = 0x7, macrotile_mode = 0,
                 threadsize_base = 64, max_waves = 16):
        self.chip          = chip.value
        self.gmem_align_w  = gmem_align_w
        self.gmem_align_h  = gmem_align_h
        self.tile_align_w  = tile_align_w
        self.tile_align_h  = tile_align_h
        self.tile_max_w    = tile_max_w
        self.tile_max_h    = tile_max_h
        self.num_vsc_pipes = num_vsc_pipes
        self.cs_shared_mem_size = cs_shared_mem_size
        self.num_sp_cores  = num_sp_cores
        self.wave_granularity = wave_granularity
        self.fibers_per_sp = fibers_per_sp
        self.threadsize_base = threadsize_base
        self.max_waves     = max_waves
        self.highest_bank_bit = highest_bank_bit
        self.ubwc_swizzle = ubwc_swizzle
        self.macrotile_mode = macrotile_mode

class A6xxGPUInfo(GPUInfo):
    """The a6xx generation has a lot more parameters, and is broken down
       into distinct sub-generations.  The template parameter avoids
       duplication of parameters that are unique to the sub-generation.
    """
    def __init__(self, chip, template, num_ccu,
                 tile_align_w, tile_align_h, num_vsc_pipes,
                 cs_shared_mem_size, wave_granularity, fibers_per_sp,
                 magic_regs, raw_magic_regs = None, highest_bank_bit = 15,
                 ubwc_swizzle = 0x6, macrotile_mode = 1,
                 threadsize_base = 64, max_waves = 16):
        if chip == CHIP.A6XX:
            tile_max_w   = 1024 # max_bitfield_val(5, 0, 5)
            tile_max_h   = max_bitfield_val(14, 8, 4) # 1008
        else:
            tile_max_w   = 1728
            tile_max_h   = 1728

        super().__init__(chip, gmem_align_w = 16, gmem_align_h = 4,
                         tile_align_w = tile_align_w,
                         tile_align_h = tile_align_h,
                         tile_max_w   = tile_max_w,
                         tile_max_h   = tile_max_h,
                         num_vsc_pipes = num_vsc_pipes,
                         cs_shared_mem_size = cs_shared_mem_size,
                         num_sp_cores = num_ccu, # The # of SP cores seems to always match # of CCU
                         wave_granularity   = wave_granularity,
                         fibers_per_sp      = fibers_per_sp,
                         highest_bank_bit = highest_bank_bit,
                         ubwc_swizzle = ubwc_swizzle,
                         macrotile_mode = macrotile_mode,
                         threadsize_base    = threadsize_base,
                         max_waves    = max_waves)

        self.num_ccu = num_ccu

        self.a6xx = Struct()
        self.a7xx = Struct()

        self.a6xx.magic = Struct()

        for name, val in magic_regs.items():
            setattr(self.a6xx.magic, name, val)

        if raw_magic_regs:
            self.a6xx.magic_raw = [[int(r[0]), r[1]] for r in raw_magic_regs]

        templates = template if isinstance(template, list) else [template]
        for template in templates:
            template.apply_props(self)


    def __str__(self):
     return super(A6xxGPUInfo, self).__str__().replace('[', '{').replace("]", "}")

class A6XXProps(dict):
    unique_props = dict()
    def apply_gen_props(self, gen, gpu_info):
        for name, val in self.items():
            setattr(getattr(gpu_info, gen), name, val)
            A6XXProps.unique_props[(name, gen)] = val

    def apply_props(self, gpu_info):
        self.apply_gen_props("a6xx", gpu_info)


class A7XXProps(A6XXProps):
    def apply_props(self, gpu_info):
        self.apply_gen_props("a7xx", gpu_info)

GPUConfig = namedtuple('GPUConfig', ['ids', 'info'])
