/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::device::*;
use crate::dev::renderer::*;
use crate::impl_cl_type_trait;

use std::pin::Pin;
use std::ptr;

use vcl_opencl_gen::*;

impl_cl_type_trait!(cl_platform_id, Platform, CL_INVALID_PLATFORM);

pub struct Platform {
    base: CLObjectBase<CL_INVALID_PLATFORM>,
    pub devices: Vec<Pin<Box<Device>>>,
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
    pub fn get_devices<'a>(&'a self, device_type: cl_device_type) -> Vec<&Pin<Box<Device>>> {
        self.devices
            .iter()
            .filter(|device| device.is_type(device_type))
            .collect()
    }

    pub fn new() -> Self {
        Self {
            base: CLObjectBase::new(),
            devices: Vec::default(),
        }
    }

    pub fn all(renderer: &Vcl) -> CLResult<Vec<Pin<Box<Platform>>>> {
        let mut count = 0;
        renderer.call_clGetPlatformIDs(0, ptr::null_mut(), &mut count)?;
        if count == 0 {
            return Ok(Vec::new());
        }

        let mut platforms = Vec::with_capacity(count as usize);
        let mut handles = Vec::with_capacity(count as usize);

        for _ in 0..count {
            // Since we use the platform address as cl_platform_id, let us make
            // sure platforms do not move from their memory area once created
            let platform = Box::pin(Platform::new());
            handles.push(platform.get_handle());
            platforms.push(platform);
        }

        renderer.call_clGetPlatformIDs(count, handles.as_mut_ptr(), ptr::null_mut())?;

        for platform in &mut platforms {
            platform.devices = Device::all(platform, renderer)?;
        }

        Ok(platforms)
    }

    pub fn contains_device(&self, id: cl_device_id) -> bool {
        for dev in &self.devices {
            if dev.get_handle() == id {
                return true;
            }
        }
        false
    }
}
