/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::{CLObjectBase, CLResult};
use crate::core::platform::Platform;
use crate::dev::renderer::*;
use crate::impl_cl_type_trait;

use vcl_opencl_gen::*;

use std::mem::{self, size_of_val};
use std::pin::Pin;
use std::ptr;

impl_cl_type_trait!(cl_device_id, Device, CL_INVALID_DEVICE);

pub struct Device {
    base: CLObjectBase<CL_INVALID_DEVICE>,
    pub platform_handle: cl_platform_id,
    ty: cl_device_type,
    /// Max size of memory object allocation in bytes
    pub max_mem_alloc_size: cl_ulong,
    pub major: u8,
    pub minor: u8,
    image_support: cl_bool,
}

impl Device {
    pub fn new(platform: &Platform) -> Self {
        Self {
            base: CLObjectBase::new(),
            platform_handle: platform.get_handle(),
            ty: CL_DEVICE_TYPE_DEFAULT as u64,
            max_mem_alloc_size: 0,
            major: 1,
            minor: 0,
            image_support: CL_FALSE,
        }
    }

    pub fn is_type(&self, device_type: cl_device_type) -> bool {
        self.ty & device_type != 0
    }

    pub fn all(platform: &Platform, renderer: &Vcl) -> CLResult<Vec<Pin<Box<Device>>>> {
        let mut count = 0;
        let ret = renderer.call_clGetDeviceIDs(
            platform.get_handle(),
            CL_DEVICE_TYPE_ALL as _,
            0,
            ptr::null_mut(),
            &mut count,
        );
        match ret {
            Err(CL_DEVICE_NOT_FOUND) => return Ok(Vec::new()),
            Err(e) => return Err(e),
            _ => (),
        }

        let mut devices = Vec::with_capacity(count as usize);
        let mut handles = Vec::with_capacity(count as usize);

        for _ in 0..count {
            // Since we use the device address as cl_devicem_id, let us make
            // sure devices do not move from their memory area once created
            let device = Box::pin(Device::new(platform));
            handles.push(device.get_handle());
            devices.push(device);
        }

        renderer.call_clGetDeviceIDs(
            platform.get_handle(),
            CL_DEVICE_TYPE_ALL as _,
            count,
            handles.as_mut_ptr(),
            ptr::null_mut(),
        )?;

        for device in &mut devices {
            // Update device type
            renderer.call_clGetDeviceInfo(
                device.get_handle(),
                CL_DEVICE_TYPE,
                size_of_val(&device.ty),
                &mut device.ty as *mut cl_device_type as _,
                ptr::null_mut(),
            )?;

            // Update max mem alloc size
            renderer.call_clGetDeviceInfo(
                device.get_handle(),
                CL_DEVICE_MAX_MEM_ALLOC_SIZE,
                size_of_val(&device.max_mem_alloc_size),
                &mut device.max_mem_alloc_size as *mut _ as _,
                ptr::null_mut(),
            )?;

            // Update OpenCL version
            let mut version_len = 0;
            renderer.call_clGetDeviceInfo(
                device.get_handle(),
                CL_DEVICE_VERSION,
                0,
                ptr::null_mut(),
                &mut version_len,
            )?;
            let mut version = vec![0u8; version_len];
            renderer.call_clGetDeviceInfo(
                device.get_handle(),
                CL_DEVICE_VERSION,
                version.len(),
                version.as_mut_slice().as_mut_ptr() as _,
                ptr::null_mut(),
            )?;

            device.major = version[7] - '0' as u8;
            device.minor = version[9] - '0' as u8;

            // Whether image are supported
            renderer.call_clGetDeviceInfo(
                device.get_handle(),
                CL_DEVICE_IMAGE_SUPPORT,
                mem::size_of_val(&device.image_support),
                &mut device.image_support as *mut _ as _,
                ptr::null_mut(),
            )?;
        }

        // Is there a default device?
        let mut default_found = false;
        for device in &devices {
            if device.is_type(CL_DEVICE_TYPE_DEFAULT.into()) {
                default_found = true;
                break;
            }
        }

        // No? Ok then first device is default
        if !devices.is_empty() && !default_found {
            devices[0].ty |= CL_DEVICE_TYPE_DEFAULT as u64;
        }

        Ok(devices)
    }

    pub fn get_invalid_property(&self) -> cl_int {
        if self.major > 1 || self.minor > 1 {
            CL_INVALID_PROPERTY
        } else {
            CL_INVALID_VALUE
        }
    }

    pub fn image_supported(&self) -> bool {
        self.image_support == CL_TRUE
    }
}
