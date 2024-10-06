// Copyright © 2024 Valve Corp. and Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::extent::{units, Extent4D, Offset4D};
use crate::tiling::Tiling;

use std::ffi::c_void;
use std::ops::Range;

#[cfg(any(target_arch = "x86_64"))]
use std::arch::x86_64::__m128i;

// This file is dedicated to the internal tiling layout, mainly in the context
// of CPU-based tiled memcpy implementations (and helpers) for VK_EXT_host_image_copy
//
// Work here is based on isl_tiled_memcpy, fd6_tiled_memcpy, old work by Rebecca Mckeever,
// and https://fgiesen.wordpress.com/2011/01/17/texture-tiling-and-swizzling/
//
// On NVIDIA, the tiling system is a two-tier one, and images are first tiled in
// a grid of rows of tiles (called "Blocks") with one or more columns:
//
// +----------+----------+----------+----------+
// | Block 0  | Block 1  | Block 2  | Block 3  |
// +----------+----------+----------+----------+
// | Block 4  | Block 5  | Block 6  | Block 7  |
// +----------+----------+----------+----------+
// | Block 8  | Block 9  | Block 10 | Block 11 |
// +----------+----------+----------+----------+
//
// The blocks themselves are ordered linearly as can be seen above, which is
// where the "Block Linear" naming comes from for NVIDIA's tiling scheme.
//
// For 3D images, each block continues in the Z direction such that tiles
// contain multiple Z slices. If the image depth is longer than the
// block depth, there will be more than one layer of blocks, where a layer is
// made up of 1 or more Z slices. For example, if the above tile pattern was
// the first layer of a multilayer arrangement, the second layer would be:
//
// +----------+----------+----------+----------+
// | Block 12 | Block 13 | Block 14 | Block 15 |
// +----------+----------+----------+----------+
// | Block 16 | Block 17 | Block 18 | Block 19 |
// +----------+----------+----------+----------+
// | Block 20 | Block 21 | Block 22 | Block 23 |
// +----------+----------+----------+----------+
//
// The number of rows, columns, and layers of tiles can thus be deduced to be:
//    rows    >= ceiling(image_height / block_height)
//    columns >= ceiling(image_width  / block_width)
//    layers  >= ceiling(image_depth  / block_depth)
//
// Where block_width is a constant 64B (unless for sparse) and block_height
// can be either 8 or 16 GOBs tall (more on GOBs below). For us, block_depth
// is one for now.
//
// The >= is in case the blocks around the edges are partial.
//
// Now comes the second tier. Each block is composed of GOBs (Groups of Bytes)
// arranged in ascending order in a single column:
//
// +---------------------------+
// |           GOB 0           |
// +---------------------------+
// |           GOB 1           |
// +---------------------------+
// |           GOB 2           |
// +---------------------------+
// |           GOB 3           |
// +---------------------------+
//
// The number of GOBs in a full block is
//    block_height * block_depth
//
// An Ampere GOB is 512 bytes, arranged in a 64x8 layout and split into Sectors.
// Each Sector is 32 Bytes, and the Sectors in a GOB are arranged in a 16x2
// layout (i.e., two 16B lines on top of each other). It's then arranged into
// two columns that are 2 sectors by 4, leading to a 4x4 grid of sectors:
//
// +----------+----------+----------+----------+
// | Sector 0 | Sector 1 | Sector 0 | Sector 1 |
// +----------+----------+----------+----------+
// | Sector 2 | Sector 3 | Sector 2 | Sector 3 |
// +----------+----------+----------+----------+
// | Sector 4 | Sector 5 | Sector 4 | Sector 5 |
// +----------+----------+----------+----------+
// | Sector 6 | Sector 7 | Sector 6 | Sector 7 |
// +----------+----------+----------+----------+
//
// From the given pixel address equations in the Orin manual, we arrived at
// the following bit interleave pattern for the pixel address:
//
//      b8 b7 b6 b5 b4 b3 b2 b1 b0
//      --------------------------
//      x5 y2 y1 x4 y0 x3 x2 x1 x0
//
// Which would look something like this:
// fn get_pixel_offset(
//      x: usize,
//      y: usize,
//  ) -> usize {
//      (x & 15)       |
//      (y & 1)  << 4  |
//      (x & 16) << 1  |
//      (y & 2)  << 5  |
//      (x & 32) << 3
//  }
//
//

// The way our implementation will work is by splitting an image into tiles, then
// each tile will be broken into its GOBs, and finally each GOB into sectors,
// where each sector will be copied into its position.
//
// For code sharing and cleanliness, we write everything to be very generic,
// so as to be shared between Linear <-> Tiled and Tiled <-> Linear paths, and
// (ab)use Rust's traits to specialize the last level (copy_gob/copy_whole_gob)
// for a particular direction.
//
// The copy_x and copy_whole_x distinction is made because if we can guarantee
// that tiles/gobs are whole and aligned, we can skip all bounds checking and
// copy things in fast and tight loops

/// Copies a GOB
///
/// This trait should be implemented twice for each GOB type, once for
/// tiled-to-linear and once for linear-to-tiled.  This allows to implement
/// the rest of tiled copies in a generic way.
trait CopyGOB {
    const GOB_EXTENT_B: Extent4D<units::Bytes>;
    const X_DIVISOR: u32;

    unsafe fn copy_gob(
        tiled: usize,
        linear: LinearPointer,
        start: Offset4D<units::Bytes>,
        end: Offset4D<units::Bytes>,
    );

    // No bounding box for this one
    unsafe fn copy_whole_gob(tiled: usize, linear: LinearPointer) {
        Self::copy_gob(
            tiled,
            linear,
            Offset4D::new(0, 0, 0, 0),
            Offset4D::new(0, 0, 0, 0) + Self::GOB_EXTENT_B,
        );
    }
}

/// Copies at most 16B of data to/from linear
trait Copy16B {
    const X_DIVISOR: u32;

    unsafe fn copy(tiled: *mut u8, linear: *mut u8, bytes: usize);
    unsafe fn copy_16b(tiled: *mut [u8; 16], linear: *mut [u8; 16]);
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    unsafe fn copy_16b_SSE2(dst: *mut __m128i, data_ptr: *mut [u8; 16]);
}

struct CopyGOBTuring2D<C: Copy16B> {
    phantom: std::marker::PhantomData<C>,
}

impl<C: Copy16B> CopyGOBTuring2D<C> {
    fn for_each_16b(mut f: impl FnMut(u32, u32, u32)) {
        for i in 0..2 {
            f(i * 0x100 + 0x00, i * 32 + 0, 0);
            f(i * 0x100 + 0x10, i * 32 + 0, 1);
            f(i * 0x100 + 0x20, i * 32 + 0, 2);
            f(i * 0x100 + 0x30, i * 32 + 0, 3);

            f(i * 0x100 + 0x40, i * 32 + 16, 0);
            f(i * 0x100 + 0x50, i * 32 + 16, 1);
            f(i * 0x100 + 0x60, i * 32 + 16, 2);
            f(i * 0x100 + 0x70, i * 32 + 16, 3);

            f(i * 0x100 + 0x80, i * 32 + 0, 4);
            f(i * 0x100 + 0x90, i * 32 + 0, 5);
            f(i * 0x100 + 0xa0, i * 32 + 0, 6);
            f(i * 0x100 + 0xb0, i * 32 + 0, 7);

            f(i * 0x100 + 0xc0, i * 32 + 16, 4);
            f(i * 0x100 + 0xd0, i * 32 + 16, 5);
            f(i * 0x100 + 0xe0, i * 32 + 16, 6);
            f(i * 0x100 + 0xf0, i * 32 + 16, 7);
        }
    }
}

impl<C: Copy16B> CopyGOB for CopyGOBTuring2D<C> {
    const GOB_EXTENT_B: Extent4D<units::Bytes> = Extent4D::new(64, 8, 1, 1);
    const X_DIVISOR: u32 = C::X_DIVISOR;

    unsafe fn copy_gob(
        tiled: usize,
        linear: LinearPointer,
        start: Offset4D<units::Bytes>,
        end: Offset4D<units::Bytes>,
    ) {
        Self::for_each_16b(|offset, x, y| {
            if y >= start.y && y < end.y {
                let tiled = tiled + (offset as usize);
                let linear = linear.at(Offset4D::new(x, y, 0, 0));
                if x >= start.x && x + 16 <= end.x {
                    C::copy_16b(tiled as *mut _, linear as *mut _);
                } else if x + 16 >= start.x && x < end.x {
                    let start = (std::cmp::max(x, start.x) - x) as usize;
                    let end = std::cmp::min(end.x - x, 16) as usize;
                    C::copy(
                        (tiled + start) as *mut _,
                        (linear + start) as *mut _,
                        end - start,
                    );
                }
            }
        });
    }

    unsafe fn copy_whole_gob(tiled: usize, linear: LinearPointer) {
        Self::for_each_16b(|offset, x, y| {
            let tiled = tiled + (offset as usize);
            let linear = linear.at(Offset4D::new(x, y, 0, 0));
            C::copy_16b(tiled as *mut _, linear as *mut _);
        });
    }
}

fn aligned_range(start: u32, end: u32, align: u32) -> Range<u32> {
    debug_assert!(align.is_power_of_two());
    let align_1 = align - 1;
    (start & !align_1)..((end + align_1) & !align_1)
}

fn chunk_range(
    whole: Range<u32>,
    chunk_start: u32,
    chunk_len: u32,
) -> Range<u32> {
    debug_assert!(chunk_start < whole.end);
    let start = if chunk_start < whole.start {
        whole.start - chunk_start
    } else {
        0
    };
    let end = std::cmp::min(whole.end - chunk_start, chunk_len);
    start..end
}

fn for_each_extent4d<U>(
    start: Offset4D<U>,
    end: Offset4D<U>,
    chunk: Extent4D<U>,
    mut f: impl FnMut(Offset4D<U>, Offset4D<U>, Offset4D<U>),
) {
    debug_assert!(chunk.width.is_power_of_two());
    debug_assert!(chunk.height.is_power_of_two());
    debug_assert!(chunk.depth.is_power_of_two());
    debug_assert!(chunk.array_len == 1);

    debug_assert!(start.a == 0);
    debug_assert!(end.a == 1);

    let x_range = aligned_range(start.x, end.x, chunk.width);
    let y_range = aligned_range(start.y, end.y, chunk.height);
    let z_range = aligned_range(start.z, end.z, chunk.depth);

    for z in z_range.step_by(chunk.depth as usize) {
        let chunk_z = chunk_range(start.z..end.z, z, chunk.depth);
        for y in y_range.clone().step_by(chunk.height as usize) {
            let chunk_y = chunk_range(start.y..end.y, y, chunk.height);
            for x in x_range.clone().step_by(chunk.width as usize) {
                let chunk_x = chunk_range(start.x..end.x, x, chunk.width);
                let chunk_start = Offset4D::new(x, y, z, start.a);
                let start = Offset4D::new(
                    chunk_x.start,
                    chunk_y.start,
                    chunk_z.start,
                    start.a,
                );
                let end =
                    Offset4D::new(chunk_x.end, chunk_y.end, chunk_z.end, end.a);
                f(chunk_start, start, end);
            }
        }
    }
}

fn for_each_extent4d_aligned<U>(
    start: Offset4D<U>,
    end: Offset4D<U>,
    chunk: Extent4D<U>,
    mut f: impl FnMut(Offset4D<U>),
) {
    debug_assert!(start.x % chunk.width == 0);
    debug_assert!(start.y % chunk.height == 0);
    debug_assert!(start.z % chunk.depth == 0);
    debug_assert!(start.a == 0);

    debug_assert!(end.x % chunk.width == 0);
    debug_assert!(end.y % chunk.height == 0);
    debug_assert!(end.z % chunk.depth == 0);
    debug_assert!(end.a == 1);

    debug_assert!(chunk.width.is_power_of_two());
    debug_assert!(chunk.height.is_power_of_two());
    debug_assert!(chunk.depth.is_power_of_two());
    debug_assert!(chunk.array_len == 1);

    for z in (start.z..end.z).step_by(chunk.depth as usize) {
        for y in (start.y..end.y).step_by(chunk.height as usize) {
            for x in (start.x..end.x).step_by(chunk.width as usize) {
                f(Offset4D::new(x, y, z, start.a));
            }
        }
    }
}

struct BlockPointer {
    pointer: usize,
    x_mul: usize,
    y_mul: usize,
    z_mul: usize,
    #[cfg(debug_assertions)]
    bl_extent: Extent4D<units::Bytes>,
}

impl BlockPointer {
    fn new(
        pointer: usize,
        bl_extent: Extent4D<units::Bytes>,
        extent: Extent4D<units::Bytes>,
    ) -> BlockPointer {
        debug_assert!(bl_extent.array_len == 1);

        debug_assert!(extent.width % bl_extent.width == 0);
        debug_assert!(extent.height % bl_extent.height == 0);
        debug_assert!(extent.depth % bl_extent.depth == 0);
        debug_assert!(extent.array_len == 1);

        BlockPointer {
            pointer,
            // We assume that offsets passed to at() are aligned to bl_extent so
            //
            //    x_bl * bl_size_B
            //  = (x / bl_extent.width) * bl_size_B
            //  = x * (bl_size_B / bl_extent.width)
            //  = x * bl_extent.height * bl_extent.depth
            x_mul: (bl_extent.height as usize) * (bl_extent.depth as usize),

            //   y_bl * width_bl * bl_size_B
            //   (y / bl_extent.height) * width_bl * bl_size_B
            // = y * (bl_size_B / bl_extent.height) * width_bl
            // = y * bl_extent.width * bl_extent.depth * width_bl
            // = y * (width_bl * bl_extent.width) * bl_extent.depth
            // = x * extent.width * bl_extent.depth
            y_mul: (extent.width as usize) * (bl_extent.depth as usize),

            //   z_bl * width_bl * height_bl * bl_size_B
            // = (z / bl_extent.depth) * width_bl * height_bl * bl_size_B
            // = z * (bl_size_B / bl_extent.depth) * width_bl * height_bl
            // = z * (bl_extent.width * bl_extent.height) * width_bl * height_bl
            // = z * width_bl * bl_extent.width * height_bl * bl_extent.height
            // = z * extent.width * extent.height
            z_mul: (extent.width as usize) * (extent.height as usize),

            #[cfg(debug_assertions)]
            bl_extent,
        }
    }

    #[inline]
    fn at(&self, offset: Offset4D<units::Bytes>) -> usize {
        #[cfg(debug_assertions)]
        {
            debug_assert!(offset.x % self.bl_extent.width == 0);
            debug_assert!(offset.y % self.bl_extent.height == 0);
            debug_assert!(offset.z % self.bl_extent.depth == 0);
            debug_assert!(offset.a == 0);
        }

        self.pointer
            + (offset.z as usize) * self.z_mul
            + (offset.y as usize) * self.y_mul
            + (offset.x as usize) * self.x_mul
    }
}

#[derive(Copy, Clone)]
struct LinearPointer {
    pointer: usize,
    x_shift: u32,
    row_stride_B: usize,
    plane_stride_B: usize,
}

impl LinearPointer {
    fn new(
        pointer: usize,
        x_divisor: u32,
        row_stride_B: usize,
        plane_stride_B: usize,
    ) -> LinearPointer {
        debug_assert!(x_divisor.is_power_of_two());
        LinearPointer {
            pointer,
            x_shift: x_divisor.ilog2(),
            row_stride_B,
            plane_stride_B,
        }
    }

    fn x_divisor(&self) -> u32 {
        1 << self.x_shift
    }

    #[inline]
    fn reverse(self, offset: Offset4D<units::Bytes>) -> LinearPointer {
        debug_assert!(offset.x % (1 << self.x_shift) == 0);
        debug_assert!(offset.a == 0);
        LinearPointer {
            pointer: self
                .pointer
                .wrapping_sub((offset.z as usize) * self.plane_stride_B)
                .wrapping_sub((offset.y as usize) * self.row_stride_B)
                .wrapping_sub((offset.x >> self.x_shift) as usize),
            x_shift: self.x_shift,
            row_stride_B: self.row_stride_B,
            plane_stride_B: self.plane_stride_B,
        }
    }

    #[inline]
    fn at(self, offset: Offset4D<units::Bytes>) -> usize {
        debug_assert!(offset.x % (1 << self.x_shift) == 0);
        debug_assert!(offset.a == 0);
        self.pointer
            .wrapping_add((offset.z as usize) * self.plane_stride_B)
            .wrapping_add((offset.y as usize) * self.row_stride_B)
            .wrapping_add((offset.x >> self.x_shift) as usize)
    }

    #[inline]
    fn offset(self, offset: Offset4D<units::Bytes>) -> LinearPointer {
        LinearPointer {
            pointer: self.at(offset),
            x_shift: self.x_shift,
            row_stride_B: self.row_stride_B,
            plane_stride_B: self.plane_stride_B,
        }
    }
}

unsafe fn copy_tile<CG: CopyGOB>(
    tiling: Tiling,
    tile_ptr: usize,
    linear: LinearPointer,
    start: Offset4D<units::Bytes>,
    end: Offset4D<units::Bytes>,
) {
    debug_assert!(linear.x_divisor() == CG::X_DIVISOR);
    debug_assert!(tiling.gob_type.extent_B() == CG::GOB_EXTENT_B);

    let tile_extent_B = tiling.extent_B();
    let tile_ptr = BlockPointer::new(tile_ptr, CG::GOB_EXTENT_B, tile_extent_B);

    if start.is_aligned_to(CG::GOB_EXTENT_B)
        && end.is_aligned_to(CG::GOB_EXTENT_B)
    {
        for_each_extent4d_aligned(start, end, CG::GOB_EXTENT_B, |gob| {
            CG::copy_whole_gob(tile_ptr.at(gob), linear.offset(gob));
        });
    } else {
        for_each_extent4d(start, end, CG::GOB_EXTENT_B, |gob, start, end| {
            let tiled = tile_ptr.at(gob);
            let linear = linear.offset(gob);
            if start == Offset4D::new(0, 0, 0, 0)
                && end == Offset4D::new(0, 0, 0, 0) + CG::GOB_EXTENT_B
            {
                CG::copy_whole_gob(tiled, linear);
            } else {
                CG::copy_gob(tiled, linear, start, end);
            }
        });
    }
}

unsafe fn copy_tiled<CG: CopyGOB>(
    tiling: Tiling,
    level_extent_B: Extent4D<units::Bytes>,
    level_tiled_ptr: usize,
    linear: LinearPointer,
    start: Offset4D<units::Bytes>,
    end: Offset4D<units::Bytes>,
) {
    let tile_extent_B = tiling.extent_B();
    let level_extent_B = level_extent_B.align(&tile_extent_B);

    // Back up the linear pointer so it also points at the start of the level.
    // This way, every step of the iteration can assume that both pointers
    // point to the start chunk of the level, tile, or GOB.
    let linear = linear.reverse(start);

    let level_tiled_ptr =
        BlockPointer::new(level_tiled_ptr, tile_extent_B, level_extent_B);

    for_each_extent4d(start, end, tile_extent_B, |tile, start, end| {
        let tile_ptr = level_tiled_ptr.at(tile);
        let linear = linear.offset(tile);
        copy_tile::<CG>(tiling, tile_ptr, linear, start, end);
    });
}

struct RawCopyToTiled {}

impl Copy16B for RawCopyToTiled {
    const X_DIVISOR: u32 = 1;

    unsafe fn copy(tiled: *mut u8, linear: *mut u8, bytes: usize) {
        // This is backwards from memcpy
        std::ptr::copy_nonoverlapping(linear, tiled, bytes);
    }

    unsafe fn copy_16b(tiled: *mut [u8; 16], linear: *mut [u8; 16]) {
        #[cfg(any(target_arch = "x86_64"))]
        {
            if is_x86_feature_detected!("sse2") {
                return unsafe { Self::copy_16b_SSE2(tiled as *mut __m128i, linear); };
            }
        }

        // Fallback without using SSE
        Self::copy(tiled as *mut _, linear as *mut _, 16);
    }

    #[cfg(any(target_arch = "x86_64"))]
    #[target_feature(enable = "sse2")]
    unsafe fn copy_16b_SSE2(dst: *mut __m128i, data_ptr: *mut [u8; 16]) {
        #[cfg(target_arch = "x86_64")]
        use std::arch::x86_64::_mm_loadu_si128;
        use std::arch::x86_64::_mm_store_si128;
        
        let linear_data = _mm_loadu_si128(data_ptr as *mut __m128i);
        _mm_store_si128(dst, linear_data);
    }
}

impl CopyGOB for RawCopyToTiled {
    const GOB_EXTENT_B: Extent4D<units::Bytes> = CopyGOBTuring2D::<RawCopyToTiled>::GOB_EXTENT_B;
    const X_DIVISOR: u32 = CopyGOBTuring2D::<RawCopyToTiled>::X_DIVISOR;

    unsafe fn copy_gob(
        tiled: usize,
        linear: LinearPointer,
        start: Offset4D<units::Bytes>,
        end: Offset4D<units::Bytes>,
    ) {
        CopyGOBTuring2D::<RawCopyToTiled>::copy_gob(tiled, linear, start, end);
    }
    
    #[cfg(any(target_arch = "x86_64"))]
    #[target_feature(enable = "sse4.1")]
    unsafe fn copy_whole_gob(tiled: usize, linear: LinearPointer) {
        #[cfg(target_arch = "x86_64")]
        use std::arch::x86_64::{_mm_loadu_si128, _mm_stream_si128, _mm_sfence, _mm_store_si128};
        // Load the linear data into XMM registers. We have 32, so we can fit
        // a whole GOB's worth of data in our RF. We cannot assume linear data
        // is aligned, so we need to use unaligned loads here.

        // COLUMN MAJOR (theoretically worse for perf)
        
        let data_reg_00 = _mm_loadu_si128((linear.at(Offset4D::new(0, 0, 0, 0))) as *mut __m128i);
        let data_reg_01 = _mm_loadu_si128((linear.at(Offset4D::new(0, 1, 0, 0))) as *mut __m128i);
        let data_reg_02 = _mm_loadu_si128((linear.at(Offset4D::new(0, 2, 0, 0))) as *mut __m128i);
        let data_reg_03 = _mm_loadu_si128((linear.at(Offset4D::new(0, 3, 0, 0))) as *mut __m128i);

        let data_reg_04 = _mm_loadu_si128((linear.at(Offset4D::new(16, 0, 0, 0))) as *mut __m128i);
        let data_reg_05 = _mm_loadu_si128((linear.at(Offset4D::new(16, 1, 0, 0))) as *mut __m128i);
        let data_reg_06 = _mm_loadu_si128((linear.at(Offset4D::new(16, 2, 0, 0))) as *mut __m128i);
        let data_reg_07 = _mm_loadu_si128((linear.at(Offset4D::new(16, 3, 0, 0))) as *mut __m128i);

        let data_reg_08 = _mm_loadu_si128((linear.at(Offset4D::new(0, 4, 0, 0))) as *mut __m128i);
        let data_reg_09 = _mm_loadu_si128((linear.at(Offset4D::new(0, 5, 0, 0))) as *mut __m128i);
        let data_reg_0a = _mm_loadu_si128((linear.at(Offset4D::new(0, 6, 0, 0))) as *mut __m128i);
        let data_reg_0b = _mm_loadu_si128((linear.at(Offset4D::new(0, 7, 0, 0))) as *mut __m128i);

        let data_reg_0c = _mm_loadu_si128((linear.at(Offset4D::new(16, 4, 0, 0))) as *mut __m128i);
        let data_reg_0d = _mm_loadu_si128((linear.at(Offset4D::new(16, 5, 0, 0))) as *mut __m128i);
        let data_reg_0e = _mm_loadu_si128((linear.at(Offset4D::new(16, 6, 0, 0))) as *mut __m128i);
        let data_reg_0f = _mm_loadu_si128((linear.at(Offset4D::new(16, 7, 0, 0))) as *mut __m128i);

        let data_reg_10 = _mm_loadu_si128((linear.at(Offset4D::new(32, 0, 0, 0))) as *mut __m128i);
        let data_reg_11 = _mm_loadu_si128((linear.at(Offset4D::new(32, 1, 0, 0))) as *mut __m128i);
        let data_reg_12 = _mm_loadu_si128((linear.at(Offset4D::new(32, 2, 0, 0))) as *mut __m128i);
        let data_reg_13 = _mm_loadu_si128((linear.at(Offset4D::new(32, 3, 0, 0))) as *mut __m128i);

        let data_reg_14 = _mm_loadu_si128((linear.at(Offset4D::new(48, 0, 0, 0))) as *mut __m128i);
        let data_reg_15 = _mm_loadu_si128((linear.at(Offset4D::new(48, 1, 0, 0))) as *mut __m128i);
        let data_reg_16 = _mm_loadu_si128((linear.at(Offset4D::new(48, 2, 0, 0))) as *mut __m128i);
        let data_reg_17 = _mm_loadu_si128((linear.at(Offset4D::new(48, 3, 0, 0))) as *mut __m128i);

        let data_reg_18 = _mm_loadu_si128((linear.at(Offset4D::new(32, 4, 0, 0))) as *mut __m128i);
        let data_reg_19 = _mm_loadu_si128((linear.at(Offset4D::new(32, 5, 0, 0))) as *mut __m128i);
        let data_reg_1a = _mm_loadu_si128((linear.at(Offset4D::new(32, 6, 0, 0))) as *mut __m128i);
        let data_reg_1b = _mm_loadu_si128((linear.at(Offset4D::new(32, 7, 0, 0))) as *mut __m128i);

        let data_reg_1c = _mm_loadu_si128((linear.at(Offset4D::new(48, 4, 0, 0))) as *mut __m128i);
        let data_reg_1d = _mm_loadu_si128((linear.at(Offset4D::new(48, 5, 0, 0))) as *mut __m128i);
        let data_reg_1e = _mm_loadu_si128((linear.at(Offset4D::new(48, 6, 0, 0))) as *mut __m128i);
        let data_reg_1f = _mm_loadu_si128((linear.at(Offset4D::new(48, 7, 0, 0))) as *mut __m128i);

        // ROW MAJOR (theoretically better for perf)
        /*
        let data_reg_00 = _mm_loadu_si128((linear.at(Offset4D::new(0, 0, 0, 0))) as *mut __m128i);
        let data_reg_01 = _mm_loadu_si128((linear.at(Offset4D::new(16, 0, 0, 0))) as *mut __m128i);
        let data_reg_02 = _mm_loadu_si128((linear.at(Offset4D::new(32, 0, 0, 0))) as *mut __m128i);
        let data_reg_03 = _mm_loadu_si128((linear.at(Offset4D::new(48, 0, 0, 0))) as *mut __m128i);

        let data_reg_04 = _mm_loadu_si128((linear.at(Offset4D::new(0, 1, 0, 0))) as *mut __m128i);
        let data_reg_05 = _mm_loadu_si128((linear.at(Offset4D::new(16, 1, 0, 0))) as *mut __m128i);
        let data_reg_06 = _mm_loadu_si128((linear.at(Offset4D::new(32, 1, 0, 0))) as *mut __m128i);
        let data_reg_07 = _mm_loadu_si128((linear.at(Offset4D::new(48, 1, 0, 0))) as *mut __m128i);

        let data_reg_08 = _mm_loadu_si128((linear.at(Offset4D::new(0, 2, 0, 0))) as *mut __m128i);
        let data_reg_09 = _mm_loadu_si128((linear.at(Offset4D::new(16, 2, 0, 0))) as *mut __m128i);
        let data_reg_0a = _mm_loadu_si128((linear.at(Offset4D::new(32, 2, 0, 0))) as *mut __m128i);
        let data_reg_0b = _mm_loadu_si128((linear.at(Offset4D::new(48, 2, 0, 0))) as *mut __m128i);

        let data_reg_0c = _mm_loadu_si128((linear.at(Offset4D::new(0, 3, 0, 0))) as *mut __m128i);
        let data_reg_0d = _mm_loadu_si128((linear.at(Offset4D::new(16, 3, 0, 0))) as *mut __m128i);
        let data_reg_0e = _mm_loadu_si128((linear.at(Offset4D::new(32, 3, 0, 0))) as *mut __m128i);
        let data_reg_0f = _mm_loadu_si128((linear.at(Offset4D::new(48, 3, 0, 0))) as *mut __m128i);

        let data_reg_10 = _mm_loadu_si128((linear.at(Offset4D::new(0, 4, 0, 0))) as *mut __m128i);
        let data_reg_11 = _mm_loadu_si128((linear.at(Offset4D::new(16, 4, 0, 0))) as *mut __m128i);
        let data_reg_12 = _mm_loadu_si128((linear.at(Offset4D::new(32, 4, 0, 0))) as *mut __m128i);
        let data_reg_13 = _mm_loadu_si128((linear.at(Offset4D::new(48, 4, 0, 0))) as *mut __m128i);

        let data_reg_14 = _mm_loadu_si128((linear.at(Offset4D::new(0, 5, 0, 0))) as *mut __m128i);
        let data_reg_15 = _mm_loadu_si128((linear.at(Offset4D::new(16, 5, 0, 0))) as *mut __m128i);
        let data_reg_16 = _mm_loadu_si128((linear.at(Offset4D::new(32, 5, 0, 0))) as *mut __m128i);
        let data_reg_17 = _mm_loadu_si128((linear.at(Offset4D::new(48, 5, 0, 0))) as *mut __m128i);

        let data_reg_18 = _mm_loadu_si128((linear.at(Offset4D::new(0, 6, 0, 0))) as *mut __m128i);
        let data_reg_19 = _mm_loadu_si128((linear.at(Offset4D::new(16, 6, 0, 0))) as *mut __m128i);
        let data_reg_1a = _mm_loadu_si128((linear.at(Offset4D::new(32, 6, 0, 0))) as *mut __m128i);
        let data_reg_1b = _mm_loadu_si128((linear.at(Offset4D::new(48, 6, 0, 0))) as *mut __m128i);

        let data_reg_1c = _mm_loadu_si128((linear.at(Offset4D::new(0, 7, 0, 0))) as *mut __m128i);
        let data_reg_1d = _mm_loadu_si128((linear.at(Offset4D::new(16, 7, 0, 0))) as *mut __m128i);
        let data_reg_1e = _mm_loadu_si128((linear.at(Offset4D::new(32, 7, 0, 0))) as *mut __m128i);
        let data_reg_1f = _mm_loadu_si128((linear.at(Offset4D::new(48, 7, 0, 0))) as *mut __m128i);
        */
        
        // Now we store the data. We use non-temporal stores here for better
        // caching.

        // COLUMN MAJOR (theoretically worse for perf)
        _mm_store_si128((tiled + (0x00 as usize)) as *mut __m128i, data_reg_00);
        _mm_store_si128((tiled + (0x10 as usize)) as *mut __m128i, data_reg_01);
        _mm_store_si128((tiled + (0x20 as usize)) as *mut __m128i, data_reg_02);
        _mm_store_si128((tiled + (0x30 as usize)) as *mut __m128i, data_reg_03);

        _mm_store_si128((tiled + (0x40 as usize)) as *mut __m128i, data_reg_04);
        _mm_store_si128((tiled + (0x50 as usize)) as *mut __m128i, data_reg_05);
        _mm_store_si128((tiled + (0x60 as usize)) as *mut __m128i, data_reg_06);
        _mm_store_si128((tiled + (0x70 as usize)) as *mut __m128i, data_reg_07);

        _mm_store_si128((tiled + (0x80 as usize)) as *mut __m128i, data_reg_08);
        _mm_store_si128((tiled + (0x90 as usize)) as *mut __m128i, data_reg_09);
        _mm_store_si128((tiled + (0xa0 as usize)) as *mut __m128i, data_reg_0a);
        _mm_store_si128((tiled + (0xb0 as usize)) as *mut __m128i, data_reg_0b);

        _mm_store_si128((tiled + (0xc0 as usize)) as *mut __m128i, data_reg_0c);
        _mm_store_si128((tiled + (0xd0 as usize)) as *mut __m128i, data_reg_0d);
        _mm_store_si128((tiled + (0xe0 as usize)) as *mut __m128i, data_reg_0e);
        _mm_store_si128((tiled + (0xf0 as usize)) as *mut __m128i, data_reg_0f);

        _mm_store_si128((tiled + (0x100 as usize)) as *mut __m128i, data_reg_10);
        _mm_store_si128((tiled + (0x110 as usize)) as *mut __m128i, data_reg_11);
        _mm_store_si128((tiled + (0x120 as usize)) as *mut __m128i, data_reg_12);
        _mm_store_si128((tiled + (0x130 as usize)) as *mut __m128i, data_reg_13);

        _mm_store_si128((tiled + (0x140 as usize)) as *mut __m128i, data_reg_14);
        _mm_store_si128((tiled + (0x150 as usize)) as *mut __m128i, data_reg_15);
        _mm_store_si128((tiled + (0x160 as usize)) as *mut __m128i, data_reg_16);
        _mm_store_si128((tiled + (0x170 as usize)) as *mut __m128i, data_reg_17);

        _mm_store_si128((tiled + (0x180 as usize)) as *mut __m128i, data_reg_18);
        _mm_store_si128((tiled + (0x190 as usize)) as *mut __m128i, data_reg_19);
        _mm_store_si128((tiled + (0x1a0 as usize)) as *mut __m128i, data_reg_1a);
        _mm_store_si128((tiled + (0x1b0 as usize)) as *mut __m128i, data_reg_1b);

        _mm_store_si128((tiled + (0x1c0 as usize)) as *mut __m128i, data_reg_1c);
        _mm_store_si128((tiled + (0x1d0 as usize)) as *mut __m128i, data_reg_1d);
        _mm_store_si128((tiled + (0x1e0 as usize)) as *mut __m128i, data_reg_1e);
        _mm_store_si128((tiled + (0x1f0 as usize)) as *mut __m128i, data_reg_1f);

       // ROW MAJOR (theoretically better for perf)
       /*
        _mm_store_si128((tiled + (0x00 as usize)) as *mut __m128i, data_reg_00);
        _mm_store_si128((tiled + (0x40 as usize)) as *mut __m128i, data_reg_01);
        _mm_store_si128((tiled + (0x100 as usize)) as *mut __m128i, data_reg_02);
        _mm_store_si128((tiled + (0x140 as usize)) as *mut __m128i, data_reg_03);

        _mm_store_si128((tiled + (0x10 as usize)) as *mut __m128i, data_reg_04);
        _mm_store_si128((tiled + (0x50 as usize)) as *mut __m128i, data_reg_05);
        _mm_store_si128((tiled + (0x110 as usize)) as *mut __m128i, data_reg_06);
        _mm_store_si128((tiled + (0x150 as usize)) as *mut __m128i, data_reg_07);

        _mm_store_si128((tiled + (0x20 as usize)) as *mut __m128i, data_reg_08);
        _mm_store_si128((tiled + (0x60 as usize)) as *mut __m128i, data_reg_09);
        _mm_store_si128((tiled + (0x120 as usize)) as *mut __m128i, data_reg_0a);
        _mm_store_si128((tiled + (0x160 as usize)) as *mut __m128i, data_reg_0b);

        _mm_store_si128((tiled + (0x30 as usize)) as *mut __m128i, data_reg_0c);
        _mm_store_si128((tiled + (0x70 as usize)) as *mut __m128i, data_reg_0d);
        _mm_store_si128((tiled + (0x130 as usize)) as *mut __m128i, data_reg_0e);
        _mm_store_si128((tiled + (0x170 as usize)) as *mut __m128i, data_reg_0f);

        _mm_store_si128((tiled + (0x80 as usize)) as *mut __m128i, data_reg_10);
        _mm_store_si128((tiled + (0xc0 as usize)) as *mut __m128i, data_reg_11);
        _mm_store_si128((tiled + (0x180 as usize)) as *mut __m128i, data_reg_12);
        _mm_store_si128((tiled + (0x1c0 as usize)) as *mut __m128i, data_reg_13);

        _mm_store_si128((tiled + (0x90 as usize)) as *mut __m128i, data_reg_14);
        _mm_store_si128((tiled + (0xd0 as usize)) as *mut __m128i, data_reg_15);
        _mm_store_si128((tiled + (0x190 as usize)) as *mut __m128i, data_reg_16);
        _mm_store_si128((tiled + (0x1d0 as usize)) as *mut __m128i, data_reg_17);

        _mm_store_si128((tiled + (0xa0 as usize)) as *mut __m128i, data_reg_18);
        _mm_store_si128((tiled + (0xe0 as usize)) as *mut __m128i, data_reg_19);
        _mm_store_si128((tiled + (0x1a0 as usize)) as *mut __m128i, data_reg_1a);
        _mm_store_si128((tiled + (0x1e0 as usize)) as *mut __m128i, data_reg_1b);

        _mm_store_si128((tiled + (0xb0 as usize)) as *mut __m128i, data_reg_1c);
        _mm_store_si128((tiled + (0xf0 as usize)) as *mut __m128i, data_reg_1d);
        _mm_store_si128((tiled + (0x1b0 as usize)) as *mut __m128i, data_reg_1e);
        _mm_store_si128((tiled + (0x1f0 as usize)) as *mut __m128i, data_reg_1f);
        */


        // Since these are non-temporal stores, we need a fence to ensure the
        // stores are fully completed before anything else touches this memory.
        //_mm_sfence();
    }
}

struct RawCopyToLinear {}

impl Copy16B for RawCopyToLinear {
    const X_DIVISOR: u32 = 1;

    unsafe fn copy(tiled: *mut u8, linear: *mut u8, bytes: usize) {
        // This is backwards from memcpy
        std::ptr::copy_nonoverlapping(tiled, linear, bytes);
    }

    unsafe fn copy_16b(tiled: *mut [u8; 16], linear: *mut [u8; 16]) {
        #[cfg(any(target_arch = "x86_64"))]
        {
            if is_x86_feature_detected!("sse2") {
                return unsafe { Self::copy_16b_SSE2(linear as *mut __m128i, tiled); };
            }
        }

        // Fallback without using SSE
        Self::copy(tiled as *mut _, linear as *mut _, 16);
    }

    #[cfg(any(target_arch = "x86_64"))]
    #[target_feature(enable = "sse2")]
    unsafe fn copy_16b_SSE2(linear: *mut __m128i, tiled: *mut [u8; 16]) {
        #[cfg(target_arch = "x86_64")]
        use std::arch::x86_64::_mm_load_si128;
        use std::arch::x86_64::_mm_storeu_si128;
        
        let tiled_data = _mm_load_si128(tiled as *mut __m128i);
        _mm_storeu_si128(linear, tiled_data);
    }
}

impl CopyGOB for RawCopyToLinear {
    const GOB_EXTENT_B: Extent4D<units::Bytes> = CopyGOBTuring2D::<RawCopyToLinear>::GOB_EXTENT_B;
    const X_DIVISOR: u32 = CopyGOBTuring2D::<RawCopyToLinear>::X_DIVISOR;

    unsafe fn copy_gob(
        tiled: usize,
        linear: LinearPointer,
        start: Offset4D<units::Bytes>,
        end: Offset4D<units::Bytes>,
    ) {
        CopyGOBTuring2D::<RawCopyToLinear>::copy_gob(tiled, linear, start, end);
    }
    
    #[cfg(any(target_arch = "x86_64"))]
    #[target_feature(enable = "sse4.1")]
    unsafe fn copy_whole_gob(tiled: usize, linear: LinearPointer) {
        #[cfg(target_arch = "x86_64")]
        use std::arch::x86_64::{_mm_load_si128, _mm_storeu_si128};
        // Load the tiled data into XMM registers. We have 32, so we can fit
        // a whole GOB's worth of data in our RF.

        // COLUMN MAJOR (theoretically worse for perf)
        /*
        let data_reg_00 = _mm_load_si128((tiled + (0x00 as usize)) as *mut __m128i);
        let data_reg_01 = _mm_load_si128((tiled + (0x10 as usize)) as *mut __m128i);
        let data_reg_02 = _mm_load_si128((tiled + (0x20 as usize)) as *mut __m128i);
        let data_reg_03 = _mm_load_si128((tiled + (0x30 as usize)) as *mut __m128i);

        let data_reg_04 = _mm_load_si128((tiled + (0x40 as usize)) as *mut __m128i);
        let data_reg_05 = _mm_load_si128((tiled + (0x50 as usize)) as *mut __m128i);
        let data_reg_06 = _mm_load_si128((tiled + (0x60 as usize)) as *mut __m128i);
        let data_reg_07 = _mm_load_si128((tiled + (0x70 as usize)) as *mut __m128i);

        let data_reg_08 = _mm_load_si128((tiled + (0x80 as usize)) as *mut __m128i);
        let data_reg_09 = _mm_load_si128((tiled + (0x90 as usize)) as *mut __m128i);
        let data_reg_0a = _mm_load_si128((tiled + (0xa0 as usize)) as *mut __m128i);
        let data_reg_0b = _mm_load_si128((tiled + (0xb0 as usize)) as *mut __m128i);

        let data_reg_0c = _mm_load_si128((tiled + (0xc0 as usize)) as *mut __m128i);
        let data_reg_0d = _mm_load_si128((tiled + (0xd0 as usize)) as *mut __m128i);
        let data_reg_0e = _mm_load_si128((tiled + (0xe0 as usize)) as *mut __m128i);
        let data_reg_0f = _mm_load_si128((tiled + (0xf0 as usize)) as *mut __m128i);

        let data_reg_10 = _mm_load_si128((tiled + (0x100 as usize)) as *mut __m128i);
        let data_reg_11 = _mm_load_si128((tiled + (0x110 as usize)) as *mut __m128i);
        let data_reg_12 = _mm_load_si128((tiled + (0x120 as usize)) as *mut __m128i);
        let data_reg_13 = _mm_load_si128((tiled + (0x130 as usize)) as *mut __m128i);

        let data_reg_14 = _mm_load_si128((tiled + (0x140 as usize)) as *mut __m128i);
        let data_reg_15 = _mm_load_si128((tiled + (0x150 as usize)) as *mut __m128i);
        let data_reg_16 = _mm_load_si128((tiled + (0x160 as usize)) as *mut __m128i);
        let data_reg_17 = _mm_load_si128((tiled + (0x170 as usize)) as *mut __m128i);

        let data_reg_18 = _mm_load_si128((tiled + (0x180 as usize)) as *mut __m128i);
        let data_reg_19 = _mm_load_si128((tiled + (0x190 as usize)) as *mut __m128i);
        let data_reg_1a = _mm_load_si128((tiled + (0x1a0 as usize)) as *mut __m128i);
        let data_reg_1b = _mm_load_si128((tiled + (0x1b0 as usize)) as *mut __m128i);

        let data_reg_1c = _mm_load_si128((tiled + (0x1c0 as usize)) as *mut __m128i);
        let data_reg_1d = _mm_load_si128((tiled + (0x1d0 as usize)) as *mut __m128i);
        let data_reg_1e = _mm_load_si128((tiled + (0x1e0 as usize)) as *mut __m128i);
        let data_reg_1f = _mm_load_si128((tiled + (0x1f0 as usize)) as *mut __m128i);
        */
        
        // ROW MAJOR (theoretically better for perf)

        let data_reg_00 = _mm_load_si128((tiled + (0x00 as usize)) as *mut __m128i);
        let data_reg_01 = _mm_load_si128((tiled + (0x40 as usize)) as *mut __m128i);
        let data_reg_02 = _mm_load_si128((tiled + (0x100 as usize)) as *mut __m128i);
        let data_reg_03 = _mm_load_si128((tiled + (0x140 as usize)) as *mut __m128i);

        let data_reg_04 = _mm_load_si128((tiled + (0x10 as usize)) as *mut __m128i);
        let data_reg_05 = _mm_load_si128((tiled + (0x50 as usize)) as *mut __m128i);
        let data_reg_06 = _mm_load_si128((tiled + (0x110 as usize)) as *mut __m128i);
        let data_reg_07 = _mm_load_si128((tiled + (0x150 as usize)) as *mut __m128i);

        let data_reg_08 = _mm_load_si128((tiled + (0x20 as usize)) as *mut __m128i);
        let data_reg_09 = _mm_load_si128((tiled + (0x60 as usize)) as *mut __m128i);
        let data_reg_0a = _mm_load_si128((tiled + (0x120 as usize)) as *mut __m128i);
        let data_reg_0b = _mm_load_si128((tiled + (0x160 as usize)) as *mut __m128i);

        let data_reg_0c = _mm_load_si128((tiled + (0x30 as usize)) as *mut __m128i);
        let data_reg_0d = _mm_load_si128((tiled + (0x70 as usize)) as *mut __m128i);
        let data_reg_0e = _mm_load_si128((tiled + (0x130 as usize)) as *mut __m128i);
        let data_reg_0f = _mm_load_si128((tiled + (0x170 as usize)) as *mut __m128i);

        let data_reg_10 = _mm_load_si128((tiled + (0x80 as usize)) as *mut __m128i);
        let data_reg_11 = _mm_load_si128((tiled + (0xc0 as usize)) as *mut __m128i);
        let data_reg_12 = _mm_load_si128((tiled + (0x180 as usize)) as *mut __m128i);
        let data_reg_13 = _mm_load_si128((tiled + (0x1c0 as usize)) as *mut __m128i);

        let data_reg_14 = _mm_load_si128((tiled + (0x90 as usize)) as *mut __m128i);
        let data_reg_15 = _mm_load_si128((tiled + (0xd0 as usize)) as *mut __m128i);
        let data_reg_16 = _mm_load_si128((tiled + (0x190 as usize)) as *mut __m128i);
        let data_reg_17 = _mm_load_si128((tiled + (0x1d0 as usize)) as *mut __m128i);

        let data_reg_18 = _mm_load_si128((tiled + (0xa0 as usize)) as *mut __m128i);
        let data_reg_19 = _mm_load_si128((tiled + (0xe0 as usize)) as *mut __m128i);
        let data_reg_1a = _mm_load_si128((tiled + (0x1a0 as usize)) as *mut __m128i);
        let data_reg_1b = _mm_load_si128((tiled + (0x1e0 as usize)) as *mut __m128i);

        let data_reg_1c = _mm_load_si128((tiled + (0xb0 as usize)) as *mut __m128i);
        let data_reg_1d = _mm_load_si128((tiled + (0xf0 as usize)) as *mut __m128i);
        let data_reg_1e = _mm_load_si128((tiled + (0x1b0 as usize)) as *mut __m128i);
        let data_reg_1f = _mm_load_si128((tiled + (0x1f0 as usize)) as *mut __m128i);
        
        // Now we store the data. Again, we cannot guarantee that linear data
        // is aligned, so we have to use regular unaligned stores here.

        // COLUMN MAJOR (theoretically worse for perf)
        /*
        _mm_storeu_si128((linear.at(Offset4D::new(0, 0, 0, 0))) as *mut __m128i, data_reg_00);
        _mm_storeu_si128((linear.at(Offset4D::new(0, 1, 0, 0))) as *mut __m128i, data_reg_01);
        _mm_storeu_si128((linear.at(Offset4D::new(0, 2, 0, 0))) as *mut __m128i, data_reg_02);
        _mm_storeu_si128((linear.at(Offset4D::new(0, 3, 0, 0))) as *mut __m128i, data_reg_03);

        _mm_storeu_si128((linear.at(Offset4D::new(16, 0, 0, 0))) as *mut __m128i, data_reg_04);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 1, 0, 0))) as *mut __m128i, data_reg_05);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 2, 0, 0))) as *mut __m128i, data_reg_06);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 3, 0, 0))) as *mut __m128i, data_reg_07);

        _mm_storeu_si128((linear.at(Offset4D::new(0, 4, 0, 0))) as *mut __m128i, data_reg_08);
        _mm_storeu_si128((linear.at(Offset4D::new(0, 5, 0, 0))) as *mut __m128i, data_reg_09);
        _mm_storeu_si128((linear.at(Offset4D::new(0, 6, 0, 0))) as *mut __m128i, data_reg_0a);
        _mm_storeu_si128((linear.at(Offset4D::new(0, 7, 0, 0))) as *mut __m128i, data_reg_0b);

        _mm_storeu_si128((linear.at(Offset4D::new(16, 4, 0, 0))) as *mut __m128i, data_reg_0c);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 5, 0, 0))) as *mut __m128i, data_reg_0d);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 6, 0, 0))) as *mut __m128i, data_reg_0e);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 7, 0, 0))) as *mut __m128i, data_reg_0f);

        _mm_storeu_si128((linear.at(Offset4D::new(32, 0, 0, 0))) as *mut __m128i, data_reg_10);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 1, 0, 0))) as *mut __m128i, data_reg_11);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 2, 0, 0))) as *mut __m128i, data_reg_12);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 3, 0, 0))) as *mut __m128i, data_reg_13);

        _mm_storeu_si128((linear.at(Offset4D::new(48, 0, 0, 0))) as *mut __m128i, data_reg_14);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 1, 0, 0))) as *mut __m128i, data_reg_15);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 2, 0, 0))) as *mut __m128i, data_reg_16);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 3, 0, 0))) as *mut __m128i, data_reg_17);

        _mm_storeu_si128((linear.at(Offset4D::new(32, 4, 0, 0))) as *mut __m128i, data_reg_18);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 5, 0, 0))) as *mut __m128i, data_reg_19);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 6, 0, 0))) as *mut __m128i, data_reg_1a);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 7, 0, 0))) as *mut __m128i, data_reg_1b);

        _mm_storeu_si128((linear.at(Offset4D::new(48, 4, 0, 0))) as *mut __m128i, data_reg_1c);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 5, 0, 0))) as *mut __m128i, data_reg_1d);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 6, 0, 0))) as *mut __m128i, data_reg_1e);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 7, 0, 0))) as *mut __m128i, data_reg_1f);
        */
        
        // ROW MAJOR (theoretically better for perf)

        _mm_storeu_si128((linear.at(Offset4D::new(0, 0, 0, 0))) as *mut __m128i, data_reg_00);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 0, 0, 0))) as *mut __m128i, data_reg_01);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 0, 0, 0))) as *mut __m128i, data_reg_02);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 0, 0, 0))) as *mut __m128i, data_reg_03);

        _mm_storeu_si128((linear.at(Offset4D::new(0, 1, 0, 0))) as *mut __m128i, data_reg_04);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 1, 0, 0))) as *mut __m128i, data_reg_05);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 1, 0, 0))) as *mut __m128i, data_reg_06);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 1, 0, 0))) as *mut __m128i, data_reg_07);

        _mm_storeu_si128((linear.at(Offset4D::new(0, 2, 0, 0))) as *mut __m128i, data_reg_08);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 2, 0, 0))) as *mut __m128i, data_reg_09);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 2, 0, 0))) as *mut __m128i, data_reg_0a);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 2, 0, 0))) as *mut __m128i, data_reg_0b);

        _mm_storeu_si128((linear.at(Offset4D::new(0, 3, 0, 0))) as *mut __m128i, data_reg_0c);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 3, 0, 0))) as *mut __m128i, data_reg_0d);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 3, 0, 0))) as *mut __m128i, data_reg_0e);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 3, 0, 0))) as *mut __m128i, data_reg_0f);

        _mm_storeu_si128((linear.at(Offset4D::new(0, 4, 0, 0))) as *mut __m128i, data_reg_10);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 4, 0, 0))) as *mut __m128i, data_reg_11);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 4, 0, 0))) as *mut __m128i, data_reg_12);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 4, 0, 0))) as *mut __m128i, data_reg_13);

        _mm_storeu_si128((linear.at(Offset4D::new(0, 5, 0, 0))) as *mut __m128i, data_reg_14);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 5, 0, 0))) as *mut __m128i, data_reg_15);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 5, 0, 0))) as *mut __m128i, data_reg_16);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 5, 0, 0))) as *mut __m128i, data_reg_17);

        _mm_storeu_si128((linear.at(Offset4D::new(0, 6, 0, 0))) as *mut __m128i, data_reg_18);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 6, 0, 0))) as *mut __m128i, data_reg_19);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 6, 0, 0))) as *mut __m128i, data_reg_1a);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 6, 0, 0))) as *mut __m128i, data_reg_1b);

        _mm_storeu_si128((linear.at(Offset4D::new(0, 7, 0, 0))) as *mut __m128i, data_reg_1c);
        _mm_storeu_si128((linear.at(Offset4D::new(16, 7, 0, 0))) as *mut __m128i, data_reg_1d);
        _mm_storeu_si128((linear.at(Offset4D::new(32, 7, 0, 0))) as *mut __m128i, data_reg_1e);
        _mm_storeu_si128((linear.at(Offset4D::new(48, 7, 0, 0))) as *mut __m128i, data_reg_1f);
    }
}

#[no_mangle]
pub unsafe extern "C" fn nil_copy_linear_to_tiled(
    tiled_dst: *mut c_void,
    level_extent_B: Extent4D<units::Bytes>,
    linear_src: *const c_void,
    linear_row_stride_B: usize,
    linear_plane_stride_B: usize,
    offset_B: Offset4D<units::Bytes>,
    extent_B: Extent4D<units::Bytes>,
    tiling: &Tiling,
) {
    let end_B = offset_B + extent_B;

    let linear_src = linear_src as usize;
    let tiled_dst = tiled_dst as usize;
    let linear_pointer = LinearPointer::new(
        linear_src,
        1,
        linear_row_stride_B,
        linear_plane_stride_B,
    );

    copy_tiled::<RawCopyToTiled>(
        *tiling,
        level_extent_B,
        tiled_dst,
        linear_pointer,
        offset_B,
        end_B,
    );
}

#[no_mangle]
pub unsafe extern "C" fn nil_copy_tiled_to_linear(
    linear_dst: *mut c_void,
    linear_row_stride_B: usize,
    linear_plane_stride_B: usize,
    tiled_src: *const c_void,
    level_extent_B: Extent4D<units::Bytes>,
    offset_B: Offset4D<units::Bytes>,
    extent_B: Extent4D<units::Bytes>,
    tiling: &Tiling,
) {
    let mut end_B = offset_B + extent_B;
    end_B.a = 1;
    let linear_dst = linear_dst as usize;
    let tiled_src = tiled_src as usize;
    let linear_pointer = LinearPointer::new(
        linear_dst,
        1,
        linear_row_stride_B,
        linear_plane_stride_B,
    );

    copy_tiled::<RawCopyToLinear>(
        *tiling,
        level_extent_B,
        tiled_src,
        linear_pointer,
        offset_B,
        end_B,
    );
}
