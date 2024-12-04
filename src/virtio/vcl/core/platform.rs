/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::device::*;
use crate::dev::renderer::*;
use crate::impl_cl_type_trait;

use std::ffi::*;
use std::pin::Pin;
use std::ptr;

use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;

impl_cl_type_trait!(cl_platform_id, Platform, CL_INVALID_PLATFORM);

pub struct Platform {
    base: CLObjectBase<CL_INVALID_PLATFORM>,
    pub devices: Vec<Pin<Box<Device>>>,

    /// List of supported extensions
    pub extensions: Vec<String>,
    /// Prepared extensions string for user queries
    pub extensions_str: String,
}

impl Platform {
    const SUPPORTED_EXTENSIONS: [&'static str; 1] = ["cl_khr_il_program"];

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
            extensions: Default::default(),
            extensions_str: Default::default(),
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

    pub fn collect_extensions(&mut self) -> CLResult<()> {
        // Get host platform extensions
        let mut extensions_size = 0;
        self.get_info(
            CL_PLATFORM_EXTENSIONS,
            0,
            ptr::null_mut(),
            &mut extensions_size,
        )?;
        let mut extensions_vec = Vec::<u8>::default();
        extensions_vec.resize(extensions_size, 0);
        self.get_info(
            CL_PLATFORM_EXTENSIONS,
            extensions_size,
            extensions_vec.as_mut_ptr().cast(),
            ptr::null_mut(),
        )?;

        // Convert host extensions to a vector of strings
        let extensions_cstr =
            CString::from_vec_with_nul(extensions_vec).expect("Failed to read host extensions");
        let extensions_str = extensions_cstr
            .into_string()
            .expect("Failed to convert host extensions to String");
        let host_extensions: Vec<String> = extensions_str.split(" ").map(String::from).collect();

        // Always support cl_khr_icd
        self.extensions.push("cl_khr_icd".to_string());

        // Check whether VCL supported extensions are available in the host platform
        for ext in Self::SUPPORTED_EXTENSIONS {
            if host_extensions
                .iter()
                .find(|&host_ext| host_ext == ext)
                .is_some()
            {
                self.extensions.push(ext.to_string());
            }
        }

        self.extensions_str = self.extensions.join(" ");

        Ok(())
    }

    pub fn get_info(
        &self,
        param_name: cl_uint,
        param_value_size: usize,
        param_value: *mut c_void,
        param_value_size_ret: *mut usize,
    ) -> CLResult<()> {
        let mut size = 0;
        Vcl::get_unchecked().call_clGetPlatformInfo(
            self.get_handle(),
            param_name,
            param_value_size,
            param_value,
            &mut size,
        )?;

        // CL_INVALID_VALUE [...] if size in bytes specified by param_value_size is < size of return
        // type as specified in the Context Attributes table and param_value is not a NULL value.
        if param_value_size < size && !param_value.is_null() {
            return Err(CL_INVALID_VALUE);
        }

        // param_value_size_ret returns the actual size in bytes of data being queried by param_name.
        // If param_value_size_ret is NULL, it is ignored.
        param_value_size_ret.write_checked(size);

        Ok(())
    }
}
