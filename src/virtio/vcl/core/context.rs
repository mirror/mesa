/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::device::Device;
use crate::dev::virtgpu::*;
use crate::impl_cl_type_trait;
use crate::protocol::VirtGpuRing;

use vcl_opencl_gen::*;

use std::ptr;
use std::sync::Arc;

impl_cl_type_trait!(cl_context, Context, CL_INVALID_CONTEXT);

pub struct Context {
    base: CLObjectBase<CL_INVALID_CONTEXT>,
    pub devices: Vec<&'static Device>,
}

impl Default for Context {
    fn default() -> Self {
        Self {
            base: CLObjectBase::new(),
            devices: Vec::default(),
        }
    }
}

impl Context {
    pub fn new(
        devices: Vec<&'static Device>,
        ring: &mut VirtGpuRing,
    ) -> Result<Arc<Context>, VirtGpuError> {
        let context = Arc::new(Context {
            base: Default::default(),
            devices,
        });

        let mut device_handles = Vec::default();
        for device in &context.devices {
            device_handles.push(device.get_handle());
        }

        ring.call_clCreateContextMESA(
            ptr::null(),
            device_handles.len() as u32,
            device_handles.as_ptr(),
            ptr::null_mut(),
            &mut context.get_handle(),
        )?;

        Ok(context)
    }
}
