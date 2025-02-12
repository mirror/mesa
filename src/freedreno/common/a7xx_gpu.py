#
# Copyright © 2021 Google, Inc.
#
# SPDX-License-Identifier: MIT

from a6xx import *
from adreno_gpu import *

# Based on a6xx_base + a6xx_gen4
a7xx_base = A6XXProps(
        has_gmem_fast_clear = True,
        has_hw_multiview = True,
        has_fs_tex_prefetch = True,
        has_sampler_minmax = True,

        supports_double_threadsize = True,

        sysmem_per_ccu_depth_cache_size = 256 * 1024,
        sysmem_per_ccu_color_cache_size = 64 * 1024,
        gmem_ccu_color_cache_fraction = CCUColorCacheFraction.EIGHTH.value,

        prim_alloc_threshold = 0x7,
        vs_max_inputs_count = 32,
        max_sets = 8,

        reg_size_vec4 = 96,
        # Blob limits it to 128 but we hang with 128
        instr_cache_size = 127,
        supports_multiview_mask = True,
        has_z24uint_s8uint = True,
        tess_use_shared = True,
        storage_16bit = True,
        has_tex_filter_cubic = True,
        has_separate_chroma_filter = True,
        has_sample_locations = True,
        has_lpac = True,
        has_getfiberid = True,
        has_dp2acc = True,
        has_dp4acc = True,
        enable_lrz_fast_clear = True,
        has_lrz_dir_tracking = True,
        has_lrz_feedback = True,
        has_per_view_viewport = True,
        line_width_min = 1.0,
        line_width_max = 127.5,
        has_scalar_alu = True,
        has_coherent_ubwc_flag_caches = True,
        has_isam_v = True,
        has_ssbo_imm_offsets = True,
        has_early_preamble = True,
        has_attachment_shading_rate = True,
        has_ubwc_linear_mipmap_fallback = True,
        prede_nop_quirk = True,
        predtf_nop_quirk = True,
        has_sad = True,
    )

a7xx_gen1 = A7XXProps(
        supports_ibo_ubwc = True,
        fs_must_have_non_zero_constlen_quirk = True,
        enable_tp_ubwc_flag_hint = True,
        reading_shading_rate_requires_smask_quirk = True,
    )

a7xx_gen2 = A7XXProps(
        stsc_duplication_quirk = True,
        has_event_write_sample_count = True,
        ubwc_unorm_snorm_int_compatible = True,
        supports_ibo_ubwc = True,
        fs_must_have_non_zero_constlen_quirk = True,
        # Most devices with a740 have blob v6xx which doesn't have
        # this hint set. Match them for better compatibility by default.
        enable_tp_ubwc_flag_hint = False,
        has_64b_ssbo_atomics = True,
        has_primitive_shading_rate = True,
        reading_shading_rate_requires_smask_quirk = True,
        has_ray_intersection = True,
    )

a7xx_gen3 = A7XXProps(
        has_event_write_sample_count = True,
        load_inline_uniforms_via_preamble_ldgk = True,
        load_shader_consts_via_preamble = True,
        has_gmem_vpc_attr_buf = True,
        sysmem_vpc_attr_buf_size = 0x20000,
        gmem_vpc_attr_buf_size = 0xc000,
        ubwc_unorm_snorm_int_compatible = True,
        supports_ibo_ubwc = True,
        has_generic_clear = True,
        r8g8_faulty_fast_clear_quirk = True,
        gs_vpc_adjacency_quirk = True,
        storage_8bit = True,
        ubwc_all_formats_compatible = True,
        has_compliant_dp4acc = True,
        ubwc_coherency_quirk = True,
        has_persistent_counter = True,
        has_64b_ssbo_atomics = True,
        has_primitive_shading_rate = True,
        has_ray_intersection = True,
        has_sw_fuse = True,
        has_rt_workaround = True,
        has_alias_rt=True,
    )

a730_magic_regs = dict(
        TPL1_DBG_ECO_CNTL = 0x1000000,
        GRAS_DBG_ECO_CNTL = 0x800,
        SP_CHICKEN_BITS = 0x1440,
        UCHE_CLIENT_PF = 0x00000084,
        PC_MODE_CNTL = 0x0000003f, # 0x00001f1f in some tests
        SP_DBG_ECO_CNTL = 0x10000000,
        RB_DBG_ECO_CNTL = 0x00000000,
        RB_DBG_ECO_CNTL_blit = 0x00000000,  # is it even needed?
        RB_UNKNOWN_8E01 = 0x0,
        VPC_DBG_ECO_CNTL = 0x02000000,
        UCHE_UNKNOWN_0E12 = 0x3200000,

        RB_UNKNOWN_8E06 = 0x02080000,
    )

a730_raw_magic_regs = [
        [A6XXRegs.REG_A6XX_UCHE_CACHE_WAYS, 0x00840004],
        [A6XXRegs.REG_A6XX_TPL1_DBG_ECO_CNTL1, 0x00040724],

        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE08, 0x00002400],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE09, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE0A, 0x00000000],
        [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E10, 0x00000000],
        [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E11, 0x00000040],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6C, 0x00008000],
        [A6XXRegs.REG_A6XX_PC_DBG_ECO_CNTL, 0x20080000],
        [A6XXRegs.REG_A7XX_PC_UNKNOWN_9E24, 0x21fc7f00],
        [A6XXRegs.REG_A7XX_VFD_UNKNOWN_A600, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE06, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6A, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6B, 0x00000080],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE73, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB02, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB01, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB22, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_B310, 0x00000000],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8120, 0x09510840],
        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8121, 0x00000a62],

        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2+1, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4+1, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6+1, 0x00000000],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_80A7, 0x00000000],

        [A6XXRegs.REG_A7XX_RB_UNKNOWN_8E79,   0x00000000],
        [A6XXRegs.REG_A7XX_RB_UNKNOWN_8899,   0x00000000],
        [A6XXRegs.REG_A7XX_RB_UNKNOWN_88F5,   0x00000000],
    ]

a740_magic_regs = dict(
        # PC_POWER_CNTL = 7,
        TPL1_DBG_ECO_CNTL = 0x11100000,
        GRAS_DBG_ECO_CNTL = 0x00004800,
        SP_CHICKEN_BITS = 0x10001400,
        UCHE_CLIENT_PF = 0x00000084,
        # Blob uses 0x1f or 0x1f1f, however these values cause vertices
        # corruption in some tests.
        PC_MODE_CNTL = 0x0000003f,
        SP_DBG_ECO_CNTL = 0x10000000,
        RB_DBG_ECO_CNTL = 0x00000000,
        RB_DBG_ECO_CNTL_blit = 0x00000000,  # is it even needed?
        # HLSQ_DBG_ECO_CNTL = 0x0,
        RB_UNKNOWN_8E01 = 0x0,
        VPC_DBG_ECO_CNTL = 0x02000000,
        UCHE_UNKNOWN_0E12 = 0x00000000,

        RB_UNKNOWN_8E06 = 0x02080000,
    )

a740_raw_magic_regs = [
        [A6XXRegs.REG_A6XX_UCHE_CACHE_WAYS, 0x00040004],
        [A6XXRegs.REG_A6XX_TPL1_DBG_ECO_CNTL1, 0x00040724],

        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE08, 0x00000400],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE09, 0x00430800],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE0A, 0x00000000],
        [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E10, 0x00000000],
        [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E11, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6C, 0x00000000],
        [A6XXRegs.REG_A6XX_PC_DBG_ECO_CNTL, 0x00100000],
        [A6XXRegs.REG_A7XX_PC_UNKNOWN_9E24, 0x21585600],
        [A6XXRegs.REG_A7XX_VFD_UNKNOWN_A600, 0x00008000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE06, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6A, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6B, 0x00000080],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE73, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB02, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB01, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB22, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_B310, 0x00000000],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8120, 0x09510840],
        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8121, 0x00000a62],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8009, 0x00000000],
        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800A, 0x00000000],
        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800B, 0x00000000],
        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800C, 0x00000000],

        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2+1, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4+1, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6+1, 0x00000000],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_80A7, 0x00000000],

        [A6XXRegs.REG_A7XX_RB_UNKNOWN_8E79,   0x00000000],
        [A6XXRegs.REG_A7XX_RB_UNKNOWN_8899,   0x00000000],
        [A6XXRegs.REG_A7XX_RB_UNKNOWN_88F5,   0x00000000],
        [A6XXRegs.REG_A7XX_RB_UNKNOWN_8C34,   0x00000000],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8008, 0x00000000],
    ]

configs = [
    GPUConfig([
        # These are named as Adreno730v3 or Adreno725v1.
        GPUId(chip_id=0x07030002, name="FD725"),
        GPUId(chip_id=0xffff07030002, name="FD725"),
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen1, A7XXProps(cmdbuf_start_a725_quirk = True)],
        num_ccu = 4,
        tile_align_w = 64,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = a730_magic_regs,
        raw_magic_regs = a730_raw_magic_regs,
    )),

    GPUConfig([
        GPUId(chip_id=0x07030001, name="FD730"), # KGSL, no speedbin data
        GPUId(chip_id=0xffff07030001, name="FD730"), # Default no-speedbin fallback
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen1],
        num_ccu = 4,
        tile_align_w = 64,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = a730_magic_regs,
        raw_magic_regs = a730_raw_magic_regs,
    )),

    GPUConfig([
        GPUId(chip_id=0x43030B00, name="FD735")
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen2, A7XXProps(enable_tp_ubwc_flag_hint = True)],
        num_ccu = 3,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        magic_regs = dict(
            TPL1_DBG_ECO_CNTL = 0x11100000,
            GRAS_DBG_ECO_CNTL = 0x00004800,
            SP_CHICKEN_BITS = 0x10001400,
            UCHE_CLIENT_PF = 0x00000084,
            PC_MODE_CNTL = 0x0000001f,
            SP_DBG_ECO_CNTL = 0x10000000,
            RB_DBG_ECO_CNTL = 0x00000001,
            RB_DBG_ECO_CNTL_blit = 0x00000001,  # is it even needed?
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000000,

            RB_UNKNOWN_8E06 = 0x02080000,
        ),
        raw_magic_regs = [
            [A6XXRegs.REG_A6XX_UCHE_CACHE_WAYS, 0x00000000],
            [A6XXRegs.REG_A6XX_TPL1_DBG_ECO_CNTL1, 0x00040724],

            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE08, 0x00000400],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE09, 0x00430800],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE0A, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E10, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E11, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6C, 0x00000000],
            [A6XXRegs.REG_A6XX_PC_DBG_ECO_CNTL, 0x00100000],
            [A6XXRegs.REG_A7XX_PC_UNKNOWN_9E24, 0x01585600],
            [A6XXRegs.REG_A7XX_VFD_UNKNOWN_A600, 0x00008000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE06, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6A, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6B, 0x00000080],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE73, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB02, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB01, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB22, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_B310, 0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8120, 0x09510840],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8121, 0x00000a62],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8009, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800A, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800B, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800C, 0x00000000],

            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6+1, 0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_80A7, 0x00000000],

            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8E79,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8899,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_88F5,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8C34,   0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8008, 0x00000000],
        ],
    )),

    GPUConfig([
        GPUId(740), # Deprecated, used for dev kernels.
        GPUId(chip_id=0x43050a01, name="FD740"), # KGSL, no speedbin data
        GPUId(chip_id=0xffff43050a01, name="FD740"), # Default no-speedbin fallback
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen2],
        num_ccu = 6,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = a740_magic_regs,
        raw_magic_regs = a740_raw_magic_regs,
    )),

    GPUConfig([
        GPUId(chip_id=0xffff43050c01, name="Adreno X1-85"),
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen2, A7XXProps(compute_constlen_quirk = True)],
        num_ccu = 6,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = a740_magic_regs,
        raw_magic_regs = a740_raw_magic_regs,
    )),

# Values from blob v676.0
    GPUConfig([
        GPUId(chip_id=0x43050a00, name="FDA32"), # Adreno A32 (G3x Gen 2)
        GPUId(chip_id=0xffff43050a00, name="FDA32"),
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen2, A7XXProps(cmdbuf_start_a725_quirk = True)],
        num_ccu = 6,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        magic_regs = a740_magic_regs,
        raw_magic_regs = [
            [A6XXRegs.REG_A6XX_UCHE_CACHE_WAYS, 0x00040004],
            [A6XXRegs.REG_A6XX_TPL1_DBG_ECO_CNTL1, 0x00000700],

            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE08, 0x00000400],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE09, 0x00430820],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE0A, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E10, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E11, 0x00000080],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6C, 0x00000000],
            [A6XXRegs.REG_A6XX_PC_DBG_ECO_CNTL, 0x00100000],
            [A6XXRegs.REG_A7XX_PC_UNKNOWN_9E24, 0x21585600],
            [A6XXRegs.REG_A7XX_VFD_UNKNOWN_A600, 0x00008000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE06, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6A, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6B, 0x00000080],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE73, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB02, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB01, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB22, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_B310, 0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8120, 0x09510840],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8121, 0x00000a62],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8009, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800A, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800B, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800C, 0x00000000],

            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6+1, 0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_80A7, 0x00000000],

            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8E79,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8899,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_88F5,   0x00000000],
        ],
    )),

    GPUConfig([
        GPUId(chip_id=0x43050b00, name="FD740v3"), # Quest 3
        GPUId(chip_id=0xffff43050b00, name="FD740v3"),
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen2, A7XXProps(enable_tp_ubwc_flag_hint = True)],
        num_ccu = 6,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        magic_regs = dict(
            # PC_POWER_CNTL = 7,
            TPL1_DBG_ECO_CNTL = 0x11100000,
            GRAS_DBG_ECO_CNTL = 0x00004800,
            SP_CHICKEN_BITS = 0x10001400,
            UCHE_CLIENT_PF = 0x00000084,
            # Blob uses 0x1f or 0x1f1f, however these values cause vertices
            # corruption in some tests.
            PC_MODE_CNTL = 0x0000003f,
            SP_DBG_ECO_CNTL = 0x10000000,
            RB_DBG_ECO_CNTL = 0x00000001,
            RB_DBG_ECO_CNTL_blit = 0x00000000,  # is it even needed?
            # HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000000,

            RB_UNKNOWN_8E06 = 0x02080000,
        ),
        raw_magic_regs = a740_raw_magic_regs,
    )),

    GPUConfig([
        GPUId(chip_id=0x43051401, name="FD750"), # KGSL, no speedbin data
        GPUId(chip_id=0xffff43051401, name="FD750"), # Default no-speedbin fallback
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen3],
        num_ccu = 6,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = dict(
            TPL1_DBG_ECO_CNTL = 0x11100000,
            GRAS_DBG_ECO_CNTL = 0x00004800,
            SP_CHICKEN_BITS = 0x10000400,
            PC_MODE_CNTL = 0x00003f1f,
            SP_DBG_ECO_CNTL = 0x10000000,
            RB_DBG_ECO_CNTL = 0x00000001,
            RB_DBG_ECO_CNTL_blit = 0x00000001,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x40000000,

            RB_UNKNOWN_8E06 = 0x02082000,
        ),
        raw_magic_regs = [
            [A6XXRegs.REG_A6XX_UCHE_CACHE_WAYS, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E10, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E11, 0x00000080],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE08, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE09, 0x00431800],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE0A, 0x00800000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6C, 0x00000000],
            [A6XXRegs.REG_A6XX_PC_DBG_ECO_CNTL, 0x00100000],
            [A6XXRegs.REG_A7XX_PC_UNKNOWN_9E24, 0x01585600],
            [A6XXRegs.REG_A7XX_VFD_UNKNOWN_A600, 0x00008000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE06, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6A, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6B, 0x00000080],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE73, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB02, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB01, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB22, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_B310, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8120, 0x09510840],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8121, 0x00000a62],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8009, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800A, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800B, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800C, 0x00000000],

            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6+1, 0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_80A7, 0x00000000],

            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8899,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_88F5,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8C34,   0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8008, 0x00000000],

            [0x930a, 0],
            [0x960a, 1],
            [A6XXRegs.REG_A7XX_SP_PS_ALIASED_COMPONENTS_CONTROL, 0],
            [A6XXRegs.REG_A7XX_SP_PS_ALIASED_COMPONENTS, 0],
        ],
    )),
]
