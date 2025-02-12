#
# Copyright © 2021 Google, Inc.
#
# SPDX-License-Identifier: MIT

from a6xx import *
from adreno_gpu import *

# Props could be modified with env var:
#  FD_DEV_FEATURES=%feature_name%=%value%:%feature_name%=%value%:...
# e.g.
#  FD_DEV_FEATURES=has_fs_tex_prefetch=0:max_sets=4

a6xx_base = A6XXProps(
        has_cp_reg_write = True,
        has_8bpp_ubwc = True,
        has_gmem_fast_clear = True,
        has_hw_multiview = True,
        has_fs_tex_prefetch = True,
        has_sampler_minmax = True,

        supports_double_threadsize = True,

        sysmem_per_ccu_depth_cache_size = 64 * 1024,
        sysmem_per_ccu_color_cache_size = 64 * 1024,
        gmem_ccu_color_cache_fraction = CCUColorCacheFraction.QUARTER.value,

        prim_alloc_threshold = 0x7,
        vs_max_inputs_count = 32,
        max_sets = 5,
        line_width_min = 1.0,
        line_width_max = 1.0,
    )


# a6xx and a7xx can be divided into distinct sub-generations, where certain
# device-info parameters are keyed to the sub-generation.  These templates
# reduce the copypaste

a6xx_gen1_low = A6XXProps(
        reg_size_vec4 = 48,
        instr_cache_size = 64,
        indirect_draw_wfm_quirk = True,
        depth_bounds_require_depth_test_quirk = True,

        has_gmem_fast_clear = False,
        has_hw_multiview = False,
        has_sampler_minmax = False,
        has_fs_tex_prefetch = False,
        sysmem_per_ccu_color_cache_size = 8 * 1024,
        sysmem_per_ccu_depth_cache_size = 8 * 1024,
        gmem_ccu_color_cache_fraction = CCUColorCacheFraction.HALF.value,
        vs_max_inputs_count = 16,
        supports_double_threadsize = False,
    )

a6xx_gen1 = A6XXProps(
        reg_size_vec4 = 96,
        instr_cache_size = 64,
        indirect_draw_wfm_quirk = True,
        depth_bounds_require_depth_test_quirk = True,
    )

a6xx_gen2 = A6XXProps(
        reg_size_vec4 = 96,
        instr_cache_size = 64, # TODO
        supports_multiview_mask = True,
        has_z24uint_s8uint = True,
        indirect_draw_wfm_quirk = True,
        depth_bounds_require_depth_test_quirk = True, # TODO: check if true
        has_dp2acc = False, # TODO: check if true
        has_8bpp_ubwc = False,
    )

a6xx_gen3 = A6XXProps(
        reg_size_vec4 = 64,
        # Blob limits it to 128 but we hang with 128
        instr_cache_size = 127,
        supports_multiview_mask = True,
        has_z24uint_s8uint = True,
        tess_use_shared = True,
        storage_16bit = True,
        has_tex_filter_cubic = True,
        has_separate_chroma_filter = True,
        has_sample_locations = True,
        has_8bpp_ubwc = False,
        has_dp2acc = True,
        has_lrz_dir_tracking = True,
        enable_lrz_fast_clear = True,
        lrz_track_quirk = True,
        has_lrz_feedback = True,
        has_per_view_viewport = True,
        has_scalar_alu = True,
        has_early_preamble = True,
        prede_nop_quirk = True,
    )

a6xx_gen4 = A6XXProps(
        reg_size_vec4 = 64,
        # Blob limits it to 128 but we hang with 128
        instr_cache_size = 127,
        supports_multiview_mask = True,
        has_z24uint_s8uint = True,
        tess_use_shared = True,
        storage_16bit = True,
        has_tex_filter_cubic = True,
        has_separate_chroma_filter = True,
        has_sample_locations = True,
        has_cp_reg_write = False,
        has_8bpp_ubwc = False,
        has_lpac = True,
        has_legacy_pipeline_shading_rate = True,
        has_getfiberid = True,
        has_dp2acc = True,
        has_dp4acc = True,
        enable_lrz_fast_clear = True,
        has_lrz_dir_tracking = True,
        has_lrz_feedback = True,
        has_per_view_viewport = True,
        has_scalar_alu = True,
        has_isam_v = True,
        has_ssbo_imm_offsets = True,
        has_ubwc_linear_mipmap_fallback = True,
        # TODO: there seems to be a quirk where at least rcp can't be in an
        # early preamble. a660 at least is affected.
        #has_early_preamble = True,
        prede_nop_quirk = True,
        predtf_nop_quirk = True,
        has_sad = True,
    )

configs = [
    GPUConfig([
        GPUId(605), # TODO: Test it, based only on libwrapfake dumps
        GPUId(608), # TODO: Test it, based only on libwrapfake dumps
        GPUId(610),
        GPUId(612), # TODO: Test it, based only on libwrapfake dumps
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen1_low],
        num_ccu = 1,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 16,
        cs_shared_mem_size = 16 * 1024,
        wave_granularity = 1,
        fibers_per_sp = 128 * 16,
        highest_bank_bit = 13,
        ubwc_swizzle = 0x7,
        macrotile_mode = 0,
        magic_regs = dict(
            PC_POWER_CNTL = 0,
            TPL1_DBG_ECO_CNTL = 0,
            GRAS_DBG_ECO_CNTL = 0,
            SP_CHICKEN_BITS = 0,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0xf,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0,
            RB_UNKNOWN_8E01 = 0x00000001,
            VPC_DBG_ECO_CNTL = 0x0,
            UCHE_UNKNOWN_0E12 = 0x10000000,
        ),
    )),

    GPUConfig([
        GPUId(615),
        GPUId(616),
        GPUId(618),
        GPUId(619),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen1],
        num_ccu = 1,
        tile_align_w = 32,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 16,
        highest_bank_bit = 14,
        macrotile_mode = 0,
        magic_regs = dict(
            PC_POWER_CNTL = 0,
            TPL1_DBG_ECO_CNTL = 0x00108000,
            GRAS_DBG_ECO_CNTL = 0x00000880,
            SP_CHICKEN_BITS = 0x00000430,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x00080000,
            RB_UNKNOWN_8E01 = 0x00000001,
            VPC_DBG_ECO_CNTL = 0x0,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    )),

    GPUConfig([
        GPUId(620),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen1],
        num_ccu = 1,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 16,
        magic_regs = dict(
            PC_POWER_CNTL = 0,
            TPL1_DBG_ECO_CNTL = 0x01008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00000400,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x01000000,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    )),

    GPUConfig([
        GPUId(chip_id=0xffff06020100, name="FD621"),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen3, A6XXProps(lrz_track_quirk = False)],
        num_ccu = 2,
        tile_align_w = 96,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        magic_regs = dict(
            PC_POWER_CNTL = 0,
            # this seems to be a chicken bit that fixes cubic filtering:
            TPL1_DBG_ECO_CNTL = 0x01008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            # UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x03000000,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    )),

    GPUConfig([
        GPUId(630),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen1],
        num_ccu = 2,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 16,
        highest_bank_bit = 15,
        macrotile_mode = 0,
        magic_regs = dict(
            PC_POWER_CNTL = 1,
            TPL1_DBG_ECO_CNTL = 0x00108000,
            GRAS_DBG_ECO_CNTL = 0x00000880,
            SP_CHICKEN_BITS = 0x00001430,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x05100000,
            HLSQ_DBG_ECO_CNTL = 0x00080000,
            RB_UNKNOWN_8E01 = 0x00000001,
            VPC_DBG_ECO_CNTL = 0x0,
            UCHE_UNKNOWN_0E12 = 0x10000001
        )
    )),

    GPUConfig([
        GPUId(640),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen2],
        num_ccu = 2,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 4 * 16,
        highest_bank_bit = 15,
        macrotile_mode = 0,
        magic_regs = dict(
            PC_POWER_CNTL = 1,
            TPL1_DBG_ECO_CNTL = 0x00008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00000420,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x00000001,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    )),

    GPUConfig([
        GPUId(680),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen2],
        num_ccu = 4,
        tile_align_w = 64,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 4 * 16,
        highest_bank_bit = 15,
        macrotile_mode = 0,
        magic_regs = dict(
            PC_POWER_CNTL = 3,
            TPL1_DBG_ECO_CNTL = 0x00108000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001430,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x00000001,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    )),

    GPUConfig([
        GPUId(650),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen3],
        num_ccu = 3,
        tile_align_w = 96,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = dict(
            PC_POWER_CNTL = 2,
            # this seems to be a chicken bit that fixes cubic filtering:
            TPL1_DBG_ECO_CNTL = 0x01008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x01000000,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    )),

    GPUConfig([
        # These are all speedbins/variants of A635
        GPUId(chip_id=0x00be06030500, name="Adreno 8c Gen 3"),
        GPUId(chip_id=0x007506030500, name="Adreno 7c+ Gen 3"),
        GPUId(chip_id=0x006006030500, name="Adreno 7c+ Gen 3 Lite"),
        GPUId(chip_id=0x00ac06030500, name="FD643"), # e.g. QCM6490, Fairphone 5
        # fallback wildcard entry should be last:
        GPUId(chip_id=0xffff06030500, name="Adreno 7c+ Gen 3"),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen4],
        num_ccu = 2,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 14,
        magic_regs = dict(
            PC_POWER_CNTL = 1,
            TPL1_DBG_ECO_CNTL = 0x05008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            UCHE_CLIENT_PF = 0x00000084,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x00000006,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    )),

    GPUConfig([
        GPUId(660),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen4],
        num_ccu = 3,
        tile_align_w = 96,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = dict(
            PC_POWER_CNTL = 2,
            TPL1_DBG_ECO_CNTL = 0x05008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            UCHE_CLIENT_PF = 0x00000084,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x01000000,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    )),

    GPUConfig([
        GPUId(chip_id=0x6060201, name="FD644"), # Called A662 in kgsl
        GPUId(chip_id=0xffff06060300, name="FD663"),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen4],
        num_ccu = 3,
        tile_align_w = 96,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 4 * 16,
        magic_regs = dict(
            PC_POWER_CNTL = 2,
            TPL1_DBG_ECO_CNTL = 0x05008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            UCHE_CLIENT_PF = 0x00000084,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x6,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    )),

    GPUConfig([
        GPUId(690),
        GPUId(chip_id=0xffff06090000, name="FD690"), # Default no-speedbin fallback
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen4, A6XXProps(broken_ds_ubwc_quirk = True)],
        num_ccu = 8,
        tile_align_w = 64,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = dict(
            PC_POWER_CNTL = 7,
            TPL1_DBG_ECO_CNTL = 0x04c00000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            UCHE_CLIENT_PF = 0x00000084,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x1200000,
            RB_DBG_ECO_CNTL = 0x100000,
            RB_DBG_ECO_CNTL_blit = 0x00100000,  # ???
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x2000400,
            UCHE_UNKNOWN_0E12 = 0x00000001
        ),
        raw_magic_regs = [
            [A6XXRegs.REG_A6XX_SP_UNKNOWN_AAF2, 0x00c00000],
        ],
    )),
]
