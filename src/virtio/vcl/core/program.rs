/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::context::Context;
use crate::core::device::Device;
use crate::dev::renderer::*;
use crate::impl_cl_type_trait;
use vcl_opencl_gen::*;

use std::collections::HashMap;
use std::ffi::*;
use std::sync::{Arc, Mutex};
use std::*;

impl_cl_type_trait!(cl_program, Program, CL_INVALID_PROGRAM);

pub struct Program {
    base: CLObjectBase<CL_INVALID_PROGRAM>,
    pub context: Arc<Context>,
    pub devs: Vec<&'static Device>,
    build_status: Mutex<HashMap<cl_device_id, cl_build_status>>,
}

impl Program {
    fn new(context: Arc<Context>, devs: Vec<&'static Device>) -> Arc<Self> {
        let mut build_status = HashMap::new();
        for dev in &devs {
            build_status.insert(dev.get_handle(), CL_BUILD_NONE);
        }

        Arc::new(Program {
            base: Default::default(),
            context,
            devs,
            build_status: Mutex::new(build_status),
        })
    }
    pub fn new_with_source(
        context: Arc<Context>,
        count: cl_uint,
        strings: *mut *const c_char,
        lengths: *const usize,
    ) -> CLResult<Arc<Program>> {
        let program = Self::new(context.clone(), context.devices.clone());

        // Construct a list fo null-terminated strings
        let mut c_strings = Vec::new();
        let strings_slice = unsafe { slice::from_raw_parts(strings, count as _) };
        if lengths.is_null() {
            // Already null-terminated strings
            for str_with_nul in strings_slice {
                let cstr = unsafe { CStr::from_ptr(*str_with_nul) };
                c_strings.push(CString::from(cstr));
            }
        } else {
            // Those strings are not necessarily null-terminated, but we know their lengths
            let lengths = unsafe { slice::from_raw_parts(lengths, count as _) };
            for i in 0..count as _ {
                let len = lengths[i];
                let str: &[u8] = unsafe { slice::from_raw_parts(strings_slice[i] as _, len) };
                let str = Vec::from(str);
                let cstr = unsafe { CString::from_vec_unchecked(str) };
                c_strings.push(CString::from(cstr));
            }
        }

        let c_strings_ptrs: Vec<*const c_char> = c_strings.iter().map(|s| s.as_ptr()).collect();

        Vcl::get().call_clCreateProgramWithSourceMESA(
            context.get_handle(),
            count,
            c_strings_ptrs.as_ptr(),
            lengths,
            &mut program.get_handle(),
        )?;

        Ok(program)
    }

    pub fn new_with_bins(
        context: Arc<Context>,
        devs: Vec<&'static Device>,
        bins: &[&[u8]],
    ) -> CLResult<Arc<Program>> {
        let program = Self::new(context.clone(), devs);

        let mut dev_handles = Vec::default();
        let mut lengths = Vec::default();
        let mut binaries_size: usize = 0;
        let mut tot_bin = Vec::new();

        for (i, d) in program.devs.iter().enumerate() {
            dev_handles.push(d.get_handle());
            lengths.push(bins[i].len());
            binaries_size += bins[i].len();
            tot_bin.extend_from_slice(bins[i]);
        }

        Vcl::get().call_clCreateProgramWithBinaryMESA(
            context.get_handle(),
            dev_handles.len() as u32,
            dev_handles.as_ptr(),
            lengths.as_ptr(),
            binaries_size,
            tot_bin.as_ptr(),
            ptr::null_mut(),
            &mut program.get_handle(),
        )?;

        Ok(program)
    }

    pub fn new_with_il(
        context: Arc<Context>,
        il: *const c_void,
        length: usize,
    ) -> CLResult<Arc<Program>> {
        let program = Self::new(context.clone(), context.devices.clone());

        Vcl::get().call_clCreateProgramWithILMESA(
            context.get_handle(),
            length,
            il,
            &mut program.get_handle(),
        )?;

        Ok(program)
    }

    pub fn link(
        context: Arc<Context>,
        devs: Vec<&'static Device>,
        options: *const c_char,
        num_input_programs: cl_uint,
        input_programs: *const cl_program,
    ) -> CLResult<Arc<Program>> {
        let program = Self::new(context.clone(), devs);

        let mut device_handles = Vec::with_capacity(program.devs.len());
        for dev in &program.devs {
            device_handles.push(dev.get_handle());
        }

        Vcl::get().call_clLinkProgramMESA(
            context.get_handle(),
            program.devs.len() as _,
            device_handles.as_ptr(),
            options,
            num_input_programs,
            input_programs,
            ptr::null_mut(),
            &mut program.get_handle(),
        )?;

        Ok(program)
    }

    pub fn bin_sizes(&self) -> CLResult<Vec<usize>> {
        let mut size = 0;

        // Get the size in bytes of the sizes array
        Vcl::get().call_clGetProgramInfo(
            self.get_handle(),
            CL_PROGRAM_BINARY_SIZES,
            0,
            ptr::null_mut(),
            &mut size,
        )?;

        let mut sizes = vec![0usize; size / mem::size_of::<usize>()];
        // Get the sizes of each binary
        Vcl::get().call_clGetProgramInfo(
            self.get_handle(),
            CL_PROGRAM_BINARY_SIZES,
            sizes.len() * mem::size_of::<usize>(),
            sizes.as_mut_ptr().cast(),
            ptr::null_mut(),
        )?;

        Ok(sizes)
    }

    pub fn binaries(&self, vals: &[u8]) -> CLResult<Vec<*mut u8>> {
        // if the application didn't provide any pointers, just return the length of devices
        if vals.is_empty() {
            return Ok(vec![ptr::null_mut(); self.devs.len()]);
        }

        // vals is an array of pointers where we should write the device binaries into
        if vals.len() != self.devs.len() * mem::size_of::<*const u8>() {
            panic!("wrong size")
        }

        let ptrs: &[*mut u8] = unsafe {
            slice::from_raw_parts(vals.as_ptr().cast(), vals.len() / mem::size_of::<*mut u8>())
        };

        let sizes = self.bin_sizes()?;

        // Let us make sure the array of arrays is contiguous memory by creating a new array
        // for all the binaries
        let mut binaries = vec![0u8; sizes.iter().sum()];
        // Fill the binaries array
        Vcl::get().call_clGetProgramInfo(
            self.get_handle(),
            CL_PROGRAM_BINARIES,
            binaries.len(),
            binaries.as_mut_ptr().cast(),
            ptr::null_mut(),
        )?;

        // Copy the binaries back to the original argument
        let mut binary_ptr = binaries.as_ptr();
        for (i, size) in sizes.into_iter().enumerate() {
            unsafe { ptr::copy_nonoverlapping(binary_ptr, ptrs[i], size) };
            binary_ptr = unsafe { binary_ptr.add(size) };
        }

        Ok(ptrs.to_vec())
    }

    pub fn build(&self, devs: &[cl_device_id], options: *const c_char) -> CLResult<()> {
        Vcl::get().call_clBuildProgram(
            self.get_handle(),
            devs.len() as u32,
            devs.as_ptr(),
            options,
            ptr::null_mut(),
        )?;

        for dev in devs {
            let mut param_value_size_ret = 0;
            Vcl::get().call_clGetProgramBuildInfo(
                self.get_handle(),
                *dev,
                CL_PROGRAM_BUILD_STATUS,
                0,
                ptr::null_mut(),
                &mut param_value_size_ret,
            )?;
            let mut build_status = CL_BUILD_NONE as cl_build_status;
            assert_eq!(param_value_size_ret, mem::size_of_val(&build_status));
            Vcl::get().call_clGetProgramBuildInfo(
                self.get_handle(),
                *dev,
                CL_PROGRAM_BUILD_STATUS,
                param_value_size_ret,
                &mut build_status as *mut _ as _,
                ptr::null_mut(),
            )?;

            self.build_status.lock().unwrap().insert(*dev, build_status);
        }

        Ok(())
    }

    pub fn status(&self, device: &'static Device) -> cl_build_status {
        *self
            .build_status
            .lock()
            .unwrap()
            .get(&device.get_handle())
            .unwrap()
    }
}
