/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::core::program::Program;

use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;

use core::ptr;
use std::ffi::*;
use std::slice;

#[cl_entrypoint(clCreateProgramWithSource)]
pub fn create_program_with_source(
    context: cl_context,
    count: cl_uint,
    strings: *mut *const c_char,
    lengths: *const usize,
) -> CLResult<cl_program> {
    if count == 0 {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE if count is zero or if strings ...
    if count == 0 || strings.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // ... or any entry in strings is NULL.
    let srcs = unsafe { slice::from_raw_parts(strings, count as usize) };
    if srcs.contains(&ptr::null()) {
        return Err(CL_INVALID_VALUE);
    }

    let c = context.get_arc()?;

    Ok(cl_program::from_arc(Program::new_with_source(
        c, count, strings, lengths,
    )?))
}
