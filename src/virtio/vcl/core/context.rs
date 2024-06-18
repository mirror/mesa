/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::device::Device;
use crate::dev::renderer::*;
use crate::impl_cl_type_trait;

use mesa_rust_util::properties::Properties;
use vcl_opencl_gen::*;

use std::ptr;
use std::sync::*;

impl_cl_type_trait!(cl_context, Context, CL_INVALID_CONTEXT);

pub struct Context {
    base: CLObjectBase<CL_INVALID_CONTEXT>,
    pub devices: Vec<&'static Device>,
    pub properties: Properties<cl_context_properties>,
    pub dtors: Mutex<Vec<Box<dyn Fn(cl_context)>>>,
}

impl Context {
    pub fn new(
        devices: Vec<&'static Device>,
        properties: Properties<cl_context_properties>,
    ) -> CLResult<Arc<Context>> {
        let context = Arc::new(Context {
            base: Default::default(),
            devices,
            properties,
            dtors: Default::default(),
        });

        let mut device_handles = Vec::default();
        for device in &context.devices {
            device_handles.push(device.get_handle());
        }

        let props = context.properties.to_raw();
        let props_ptr = if props.len() > 1 {
            props.as_ptr()
        } else {
            ptr::null()
        };
        Vcl::get().call_clCreateContextMESA(
            props_ptr,
            device_handles.len() as u32,
            device_handles.as_ptr(),
            ptr::null_mut(),
            &mut context.get_handle(),
        )?;

        Ok(context)
    }

    pub fn get_device_from_handle(&self, device_handle: cl_device_id) -> CLResult<&'static Device> {
        if device_handle.is_null() {
            return Err(CL_INVALID_DEVICE);
        }
        for device in self.devices.iter().copied() {
            if device.get_handle() == device_handle {
                return Ok(device);
            }
        }
        Err(CL_INVALID_DEVICE)
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        let cl = cl_context::from_ptr(self);
        self.dtors
            .lock()
            .unwrap()
            .iter()
            .rev()
            .for_each(|cb| cb(cl));
    }
}
