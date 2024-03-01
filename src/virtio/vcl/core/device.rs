/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::{CLObjectBase, CLResult};
use crate::core::platform::Platform;
use crate::dev::renderer::*;
use crate::impl_cl_type_trait;

use vcl_opencl_gen::*;

use std::mem::size_of_val;
use std::pin::Pin;
use std::ptr;

impl_cl_type_trait!(cl_device_id, Device, CL_INVALID_DEVICE);

pub struct Device {
    base: CLObjectBase<CL_INVALID_DEVICE>,
    ty: cl_device_type,
    /// Max size of memory object allocation in bytes
    pub max_mem_alloc_size: cl_ulong,
}

impl Device {
    pub fn new() -> Self {
        Self {
            base: CLObjectBase::new(),
            ty: CL_DEVICE_TYPE_DEFAULT as u64,
            max_mem_alloc_size: 0,
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
            let device = Box::pin(Device::new());
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
        }

        Ok(devices)
    }
}
