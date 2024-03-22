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

use std::ffi::c_char;
use std::ptr;
use std::sync::Arc;

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

        Vcl::get().call_clCreateProgramWithSourceMESA(
            context.get_handle(),
            count,
            strings,
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
        let mut total_length: usize = 0;
        let mut tot_bin = Vec::new();

        for (i, d) in devs.iter().enumerate() {
            dev_handles.push(d.get_handle());
            lengths.push(bins[i].len());
            total_length += bins[i].len();
            tot_bin.extend_from_slice(bins[i]);
        }

        Vcl::get().call_clCreateProgramWithBinaryMESA(
            context.get_handle(),
            devs.len() as u32,
            dev_handles.as_ptr(),
            lengths.as_ptr(),
            total_length,
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
