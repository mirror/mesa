/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#![allow(non_snake_case)]

use crate::api::icd::CLResult; 
use crate::core::platform::Platform;

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

    // Run initialization code once
    Platform::init_once();

    // A list of OpenCL platforms available should be stored in platforms.
    // The cl_platform_id values returned in platforms are ICD compatible and can be used to identify a
    // specific OpenCL platform. If the platforms argument is NULL, then this argument is ignored. The
    // number of OpenCL platforms returned is the minimum of the value specified by num_entries or the
    // number of OpenCL platforms available.
    platforms.write_checked(Platform::get().as_ptr());

    // The number of OpenCL platforms available should be stored in num_platforms.
    // If num_platforms is NULL, then this argument is ignored.
    num_platforms.write_checked(1);

    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;

    use std::ptr;

    #[test]
    fn test_get_platform_ids_invalid() {
        assert_eq!(
            get_platform_ids(0, ptr::null_mut(), ptr::null_mut()),
            Err(CL_INVALID_VALUE)
        );
        assert_eq!(
            get_platform_ids(1, ptr::null_mut(), ptr::null_mut()),
            Err(CL_INVALID_VALUE)
        );

        let mut platform = ptr::null_mut();
        assert_eq!(
            get_platform_ids(0, &mut platform, ptr::null_mut()),
            Err(CL_INVALID_VALUE)
        );
        assert_eq!(platform, ptr::null_mut());
    }

    #[test]
    fn test_get_platform_ids_valid() {
        let mut num_platforms = 0;
        let mut platform = ptr::null_mut();

        assert_eq!(
            get_platform_ids(0, ptr::null_mut(), &mut num_platforms),
            Ok(())
        );
        assert_eq!(num_platforms, 1);

        assert_eq!(get_platform_ids(1, &mut platform, ptr::null_mut()), Ok(()));
        assert_eq!(platform, Platform::get().as_ptr());
    }
}
