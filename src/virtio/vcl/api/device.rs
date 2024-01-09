/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::util::check_cl_device_type;
use crate::core::platform::GetPlatformRef;

use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;

use mesa_rust_util::ptr::CheckedPtr;

use std::cmp::min;

#[cl_entrypoint(clGetDeviceIDs)]
fn get_device_ids(
    platform: cl_platform_id,
    device_type: cl_device_type,
    num_entries: cl_uint,
    devices: *mut cl_device_id,
    num_devices: *mut cl_uint,
) -> CLResult<()> {
    // CL_INVALID_PLATFORM if platform is not a valid platform
    let platform = platform.get_ref()?;

    // CL_INVALID_DEVICE_TYPE if device_type is not a valid value
    check_cl_device_type(device_type)?;

    // CL_INVALID_VALUE if num_entries is equal to zero and devices is not NULL
    if num_entries == 0 && !devices.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE [...] if both num_devices and devices are NULL
    if num_devices.is_null() && devices.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    let devs = platform.get_devices(device_type);
    // CL_DEVICE_NOT_FOUND if no OpenCL devices that matched device_type were found
    if devs.is_empty() {
        return Err(CL_DEVICE_NOT_FOUND);
    }

    // num_devices returns the number of OpenCL devices available that match device_type. If
    // num_devices is NULL, this argument is ignored.
    num_devices.write_checked(devs.len() as cl_uint);

    if !devices.is_null() {
        let n = min(num_entries as usize, devs.len());

        #[allow(clippy::needless_range_loop)]
        for i in 0..n {
            unsafe {
                *devices.add(i) = cl_device_id::from_ptr(devs[i]);
            }
        }
    }

    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::api::platform::get_platform_ids;

    use std::ptr;

    #[test]
    fn test_get_device_ids() {
        let mut platform = ptr::null_mut();
        assert_eq!(get_platform_ids(1, &mut platform, ptr::null_mut()), Ok(()));

        let dev_ty = CL_DEVICE_TYPE_ALL as u64;

        let mut device = ptr::null_mut();
        let mut num_devices = 0;
        assert_eq!(
            get_device_ids(platform, dev_ty, 1, &mut device, &mut num_devices),
            Ok(())
        );

        assert_eq!(
            get_device_ids(ptr::null_mut(), dev_ty, 1, &mut device, &mut num_devices),
            Err(CL_INVALID_PLATFORM)
        );

        let wrong_platform = unsafe { std::mem::transmute(42u64) };
        assert_eq!(
            get_device_ids(wrong_platform, dev_ty, 1, &mut device, &mut num_devices),
            Err(CL_INVALID_PLATFORM)
        );

        let wrong_dev_ty: u64 = 1 << 5;
        assert_eq!(
            get_device_ids(platform, wrong_dev_ty, 1, &mut device, &mut num_devices),
            Err(CL_INVALID_DEVICE_TYPE)
        );

        assert_eq!(
            get_device_ids(platform, dev_ty, 0, &mut device, &mut num_devices),
            Err(CL_INVALID_VALUE)
        );

        assert_eq!(
            get_device_ids(platform, dev_ty, 1, ptr::null_mut(), ptr::null_mut()),
            Err(CL_INVALID_VALUE)
        );

        assert_eq!(
            get_device_ids(platform, dev_ty, 1, &mut device, ptr::null_mut()),
            Ok(())
        );
    }
}
