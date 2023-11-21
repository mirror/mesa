/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::device::*;
use crate::dev::virtgpu::VirtGpuError;

use vcl_opencl_gen::*;

use std::ffi::CString;
use std::sync::Once;

#[repr(C)]
pub struct Platform {
    dispatch: &'static cl_icd_dispatch,
    devices: Vec<Device>,
    pub name: Option<CString>,
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
    devices: Vec::new(),
    name: None,
};

impl Platform {
    pub fn init_once() -> Result<(), VirtGpuError> {
        // SAFETY: no concurrent static mut access due to std::Once
        PLATFORM_ONCE.call_once(|| {
            let devices = Device::all().expect("Failed to get devices");

            let mut name = String::from("vcl");
            if let Some(device) = devices.get(0) {
                let host_platform_name = device
                    .gpu
                    .capset
                    .get_host_platform_name()
                    .to_str()
                    .expect("Failed to parse host platform name");
                name = format!("{name} ({host_platform_name})");
            }
            let name_cstr = CString::new(name).expect("Failed to create CString for platform name");

            unsafe {
                PLATFORM.name = Some(name_cstr);
                PLATFORM.devices = devices;
            };
        });
        Ok(())
    }

    pub fn get() -> &'static Self {
        debug_assert!(PLATFORM_ONCE.is_completed());
        // SAFETY: no mut references exist at this point
        unsafe { &PLATFORM }
    }

    pub fn as_ptr(&self) -> cl_platform_id {
        (self as *const Self) as cl_platform_id
    }

    pub fn get_name(&self) -> &str {
        self.name
            .as_ref()
            .unwrap()
            .to_str()
            .expect("Failed to parse platform name")
    }
}

pub trait GetPlatformRef {
    fn get_ref(&self) -> CLResult<&'static Platform>;
}

impl GetPlatformRef for cl_platform_id {
    fn get_ref(&self) -> CLResult<&'static Platform> {
        if !self.is_null() && *self == Platform::get().as_ptr() {
            Ok(Platform::get())
        } else {
            Err(CL_INVALID_PLATFORM)
        }
    }
}
