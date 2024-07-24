/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#![allow(non_snake_case)]

use crate::api::icd::*;
use crate::api::util::*;
use crate::dev::renderer::Vcl;

use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;

use std::cmp::min;
use std::ffi::*;
use std::mem::MaybeUninit;
use std::ptr;

#[cl_entrypoint(clGetPlatformIDs)]
pub fn get_platform_ids(
    num_entries: cl_uint,
    platforms: *mut cl_platform_id,
    num_platforms: *mut cl_uint,
) -> CLResult<()> {
    // Run initialization code once
    if Vcl::init_once().is_err() {
        // Failure to allocate resources required by the implementation on the host
        return Err(CL_OUT_OF_HOST_MEMORY);
    }

    if num_entries == 0 && !platforms.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    if num_platforms.is_null() && platforms.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    let virtgpu = Vcl::get();
    let virt_platforms = virtgpu.get_platforms();

    num_platforms.write_checked(virt_platforms.len() as cl_uint);

    if !platforms.is_null() {
        let n = min(num_entries as usize, virt_platforms.len());

        #[allow(clippy::needless_range_loop)]
        for i in 0..n {
            unsafe { *platforms.add(i) = virt_platforms[i].get_handle() }
        }
    }

    Ok(())
}

impl CLInfo<cl_platform_info> for cl_platform_id {
    fn query(&self, q: cl_platform_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        let platform = self.get_ref()?;
        Ok(match q {
            CL_PLATFORM_EXTENSIONS => cl_prop(platform.extensions_str.as_str()),
            CL_PLATFORM_ICD_SUFFIX_KHR => cl_prop("MESA"),
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cl_entrypoint(clGetPlatformInfo)]
pub fn get_platform_info(
    platform: cl_platform_id,
    param_name: cl_uint,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    if !Vcl::get().contains_platform(platform) {
        return Err(CL_INVALID_PLATFORM);
    }

    match param_name {
        CL_PLATFORM_EXTENSIONS | CL_PLATFORM_ICD_SUFFIX_KHR => platform.get_info(
            param_name,
            param_value_size,
            param_value,
            param_value_size_ret,
        ),
        _ => forward_get_platform_info(
            platform,
            param_name,
            param_value_size,
            param_value,
            param_value_size_ret,
        ),
    }
}

fn forward_get_platform_info(
    platform: cl_platform_id,
    param_name: cl_uint,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    let mut size = 0;
    Vcl::get().call_clGetPlatformInfo(
        platform,
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

#[no_mangle]
pub extern "C" fn clGetExtensionFunctionAddressForPlatform(
    platform: cl_platform_id,
    function_name: *const c_char,
) -> *mut c_void {
    if Vcl::get().contains_platform(platform) {
        clGetExtensionFunctionAddress(function_name)
    } else {
        ptr::null_mut()
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
    }

    #[test]
    fn test_get_platform_ids_valid() {
        let mut num_platforms = 0;
        let mut platform = ptr::null_mut();

        assert_eq!(
            get_platform_ids(0, ptr::null_mut(), &mut num_platforms),
            Ok(())
        );
        assert!(num_platforms > 0);

        assert_eq!(get_platform_ids(1, &mut platform, ptr::null_mut()), Ok(()));
    }

    #[test]
    fn test_get_platform_info() {
        let mut platform = ptr::null_mut();
        assert_eq!(get_platform_ids(1, &mut platform, ptr::null_mut()), Ok(()));

        const PV_SIZE: usize = 256;
        let mut pv = ['\0'; PV_SIZE];
        let pv_ptr = pv.as_mut_ptr() as _;
        let mut pv_size_ret = 0;

        let res = get_platform_info(
            platform,
            CL_PLATFORM_NAME,
            PV_SIZE,
            pv_ptr,
            &mut pv_size_ret,
        );
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
        let res = get_platform_info(platform, WRONG_PARAM, PV_SIZE, pv_ptr, &mut pv_size_ret);
        assert_eq!(res, Err(CL_INVALID_VALUE));

        let res = get_platform_info(
            platform,
            CL_PLATFORM_NAME,
            PV_SIZE,
            ptr::null_mut(),
            &mut pv_size_ret,
        );
        assert_eq!(res, Ok(()));

        let res = get_platform_info(platform, CL_PLATFORM_NAME, 0, pv_ptr, &mut pv_size_ret);
        assert_eq!(res, Err(CL_INVALID_VALUE));

        let res = get_platform_info(platform, CL_PLATFORM_NAME, PV_SIZE, pv_ptr, ptr::null_mut());
        assert_eq!(res, Ok(()));
    }
}
