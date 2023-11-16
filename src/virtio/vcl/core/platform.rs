/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::DISPATCH;

use vcl_opencl_gen::*;

use std::sync::Once;

#[repr(C)]
pub struct Platform {
    dispatch: &'static cl_icd_dispatch,
}

static PLATFORM_ONCE: Once = Once::new();

macro_rules! gen_cl_exts {
    (@COUNT $e:expr) => { 1 };
    (@COUNT $e:expr, $($es:expr),+) => { 1 + gen_cl_exts!(@COUNT $($es),*) };

    (@CONCAT $e:tt) => { $e };
    (@CONCAT $e:tt, $($es:tt),+) => { concat!($e, ' ', gen_cl_exts!(@CONCAT $($es),*)) };

    ([$(($major:expr, $minor:expr, $patch:expr, $ext:tt)$(,)?)+]) => {
        pub static PLATFORM_EXTENSION_STR: &str = concat!(gen_cl_exts!(@CONCAT $($ext),*));
    }
}

gen_cl_exts!([(1, 0, 0, "cl_khr_icd"),]);

static mut PLATFORM: Platform = Platform {
    dispatch: &DISPATCH,
};

impl Platform {
    pub fn init_once() {
        // SAFETY: no concurrent static mut access due to std::Once
        PLATFORM_ONCE.call_once(|| {});
    }

    pub fn get() -> &'static Self {
        debug_assert!(PLATFORM_ONCE.is_completed());
        // SAFETY: no mut references exist at this point
        unsafe { &PLATFORM }
    }

    pub fn as_ptr(&self) -> cl_platform_id {
        (self as *const Self) as cl_platform_id
    }
}
