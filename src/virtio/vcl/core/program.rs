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

use std::ffi::*;
use std::sync::Arc;
use std::*;

impl_cl_type_trait!(cl_program, Program, CL_INVALID_PROGRAM);

pub struct Program {
    base: CLObjectBase<CL_INVALID_PROGRAM>,
    pub context: Arc<Context>,
}

impl Program {
    pub fn new_with_source(
        context: Arc<Context>,
        count: cl_uint,
        strings: *mut *const c_char,
        lengths: *const usize,
    ) -> CLResult<Arc<Program>> {
        let program = Arc::new(Program {
            base: Default::default(),
            context: context.clone(),
        });

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
        let program = Arc::new(Program {
            base: Default::default(),
            context: context.clone(),
        });

        let mut dev_handles = Vec::default();
        let mut lengths = Vec::default();
        let mut binaries_size: usize = 0;
        let mut tot_bin = Vec::new();

        for (i, d) in devs.iter().enumerate() {
            dev_handles.push(d.get_handle());
            lengths.push(bins[i].len());
            binaries_size += bins[i].len();
            tot_bin.extend_from_slice(bins[i]);
        }

        Vcl::get().call_clCreateProgramWithBinaryMESA(
            context.get_handle(),
            devs.len() as u32,
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
        il: *const ::std::os::raw::c_void,
        length: usize,
    ) -> CLResult<Arc<Program>> {
        let program = Arc::new(Program {
            base: Default::default(),
            context: context.clone(),
        });

        Vcl::get().call_clCreateProgramWithILMESA(
            context.get_handle(),
            il,
            length,
            &mut program.get_handle(),
        )?;

        Ok(program)
    }

    pub fn link(
        context: Arc<Context>,
        num_devices: cl_uint,
        device_list: *const cl_device_id,
        options: *const c_char,
        num_input_programs: cl_uint,
        input_programs: *const cl_program,
    ) -> CLResult<Arc<Program>> {
        let program = Arc::new(Program {
            base: Default::default(),
            context: context.clone(),
        });

        Vcl::get().call_clLinkProgramMESA(
            context.get_handle(),
            num_devices,
            device_list,
            options,
            num_input_programs,
            input_programs,
            ptr::null_mut(),
            &mut program.get_handle(),
        )?;

        Ok(program)
    }
}
