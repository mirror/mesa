/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::device::*;
use crate::impl_cl_type_trait;

use vcl_opencl_gen::*;

use std::ffi::CString;

impl_cl_type_trait!(cl_platform_id, Platform, CL_INVALID_PLATFORM);

pub struct Platform {
    base: CLObjectBase<CL_INVALID_PLATFORM>,
    devices: Vec<Device>,
    pub name: Option<CString>,
}

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

impl Platform {
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

    pub fn get_devices<'a>(&'a self, device_type: cl_device_type) -> Vec<&'a Device> {
        // We only support GPUs
        let v: u32 = device_type.try_into().unwrap_or(0);
        if v & (CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_DEFAULT) != 0 {
            self.devices.iter().collect()
        } else {
            Vec::default()
        }
    }
}
