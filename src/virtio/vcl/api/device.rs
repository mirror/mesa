/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::util::*;
use crate::dev::renderer::Vcl;

use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;

use mesa_rust_util::ptr::CheckedPtr;

use std::cmp::min;
use std::ffi::c_void;
use std::mem::MaybeUninit;

#[cl_entrypoint(clGetDeviceIDs)]
pub fn get_device_ids(
    platform: cl_platform_id,
    device_type: cl_device_type,
    num_entries: cl_uint,
    devices: *mut cl_device_id,
    num_devices: *mut cl_uint,
) -> CLResult<()> {
    // CL_INVALID_PLATFORM if platform is not a valid platform
    if !Vcl::get().contains_platform(platform) {
        return Err(CL_INVALID_PLATFORM);
    }
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

    let devs = platform.get_ref()?.get_devices(device_type);
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
                *devices.add(i) = devs[i].get_handle();
            }
        }
    }

    Ok(())
}

impl CLInfo<cl_device_info> for cl_device_id {
    fn query(&self, q: cl_device_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        let dev = self.get_ref()?;

        Ok(match q {
            CL_DEVICE_PLATFORM => cl_prop::<cl_platform_id>(dev.platform_handle),
            CL_DEVICE_REFERENCE_COUNT => cl_prop::<cl_uint>(1),
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cl_entrypoint(clGetDeviceInfo)]
fn get_device_info(
    device: cl_device_id,
    param_name: cl_uint,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    device.get_ref()?;

    if param_name == CL_DEVICE_REFERENCE_COUNT || param_name == CL_DEVICE_PLATFORM {
        return device.get_info(
            param_name,
            param_value_size,
            param_value,
            param_value_size_ret,
        );
    }

    let mut size = 0;
    let ret = Vcl::get().call_clGetDeviceInfo(
        device,
        param_name,
        param_value_size,
        param_value,
        &mut size,
    );

    if param_name == CL_DEVICE_EXECUTION_CAPABILITIES && !param_value.is_null() {
        unsafe {
            *(param_value as *mut cl_device_exec_capabilities) &= !CL_EXEC_NATIVE_KERNEL as u64;
        }
    }

    // Return a valid error for this call
    if let Err(CL_INVALID_OPERATION) = ret {
        return Err(CL_INVALID_VALUE);
    }
    if ret.is_err() {
        return ret;
    }

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

#[cl_entrypoint(clSetDefaultDeviceCommandQueue)]
fn set_default_device_command_queue(
    _context: cl_context,
    _device: cl_device_id,
    _command_queue: cl_command_queue,
) -> CLResult<()> {
    Err(CL_INVALID_OPERATION)
}

#[cl_entrypoint(clRetainDevice)]
fn retain_device(_device: cl_device_id) -> CLResult<()> {
    Ok(())
}

#[cl_entrypoint(clReleaseDevice)]
fn release_device(_device: cl_device_id) -> CLResult<()> {
    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;

    use crate::api::test_util::*;

    use std::ptr;

    #[test]
    fn test_get_device_ids() {
        let platform = setup_platform();

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

        let wrong_dev_ty: u64 = 1 << 7;
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
