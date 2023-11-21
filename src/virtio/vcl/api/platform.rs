/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#![allow(non_snake_case)]

use crate::api::icd::CLResult;
use crate::api::util::*;
use crate::core::platform::*;

use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;
use vcl_proc_macros::*;

use std::mem::MaybeUninit;

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
    if Platform::init_once().is_err() {
        // Failure to allocate resources required by the implementation on the host
        return Err(CL_OUT_OF_HOST_MEMORY);
    }

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

#[cl_info_entrypoint(clGetPlatformInfo)]
impl CLInfo<cl_platform_info> for cl_platform_id {
    fn query(&self, q: cl_platform_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        let platform = self.get_ref()?;

        Ok(match q {
            CL_PLATFORM_EXTENSIONS => cl_prop(PLATFORM_EXTENSION_STR),
            CL_PLATFORM_ICD_SUFFIX_KHR => cl_prop("MESA"),
            CL_PLATFORM_NAME => cl_prop(platform.get_name()),
            CL_PLATFORM_PROFILE => cl_prop("FULL_PROFILE"),
            CL_PLATFORM_VENDOR => cl_prop("Mesa/X.org"),
            // OpenCL<space><major_version.minor_version><space><platform-specific information>
            CL_PLATFORM_VERSION => cl_prop("OpenCL 1.0 "),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
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

    #[test]
    fn test_get_platform_info() {
        let mut platform = ptr::null_mut();
        assert_eq!(get_platform_ids(1, &mut platform, ptr::null_mut()), Ok(()));

        const PV_SIZE: usize = 256;
        let mut pv = ['\0'; PV_SIZE];
        let pv_ptr = pv.as_mut_ptr() as _;
        let mut pv_size_ret = 0;

        let res = platform.get_info(CL_PLATFORM_NAME, PV_SIZE, pv_ptr, &mut pv_size_ret);
        assert_eq!(res, Ok(()));

        let res = clGetPlatformInfo(
            ptr::null_mut(),
            CL_PLATFORM_NAME,
            PV_SIZE,
            pv_ptr,
            &mut pv_size_ret,
        );
        assert_eq!(res, CL_INVALID_PLATFORM);

        const WRONG_PLATFORM: cl_platform_id = unsafe { std::mem::transmute(42u64) };
        let res = clGetPlatformInfo(
            WRONG_PLATFORM,
            CL_PLATFORM_NAME,
            PV_SIZE,
            pv_ptr,
            &mut pv_size_ret,
        );
        assert_eq!(res, CL_INVALID_PLATFORM);

        const WRONG_PARAM: u32 = 42;
        let res = platform.get_info(WRONG_PARAM, PV_SIZE, pv_ptr, &mut pv_size_ret);
        assert_eq!(res, Err(CL_INVALID_VALUE));

        let res = platform.get_info(CL_PLATFORM_NAME, PV_SIZE, ptr::null_mut(), &mut pv_size_ret);
        assert_eq!(res, Ok(()));

        let res = platform.get_info(CL_PLATFORM_NAME, 0, pv_ptr, &mut pv_size_ret);
        assert_eq!(res, Err(CL_INVALID_VALUE));

        let res = platform.get_info(CL_PLATFORM_NAME, PV_SIZE, pv_ptr, ptr::null_mut());
        assert_eq!(res, Ok(()));
    }
}
