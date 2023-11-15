/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#![allow(non_snake_case)]

use crate::api::icd::CLResult;

use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;

#[cl_entrypoint(clGetPlatformIDs)]
fn get_platform_ids(
    num_entries: cl_uint,
    platforms: *mut cl_platform_id,
    num_platforms: *mut cl_uint,
) -> CLResult<()> {
    // CL_INVALID_VALUE if num_entries is equal to zero and platforms is not NULL
    if num_entries == 0 && !platforms.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // Or if both num_platforms and platforms are NULL
    if num_platforms.is_null() && platforms.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // The number of OpenCL platforms available should be stored in num_platforms.
    // If num_platforms is NULL, then this argument is ignored.
    num_platforms.write_checked(0);

    Ok(())
}
