/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use vcl_opencl_gen::*;

use crate::api::context::*;
use crate::api::device::*;
use crate::api::platform::*;
use crate::api::program::*;

use std::ffi::CString;
use std::ptr;

pub const TEST_KERNEL_NAME: &str = "test_kernel";
pub const TEST_KERNEL_SOURCE: &str = "__kernel void test_kernel(int a, int b) { a + b; }";

pub fn setup_platform() -> cl_platform_id {
    let mut platform = ptr::null_mut();

    assert_eq!(get_platform_ids(1, &mut platform, ptr::null_mut()), Ok(()));

    platform
}

pub fn setup_device() -> (cl_device_id, cl_platform_id) {
    let platform = setup_platform();
    let mut device = ptr::null_mut();
    let mut num_devices = 0;

    assert_eq!(
        get_device_ids(
            platform,
            CL_DEVICE_TYPE_ALL as u64,
            1,
            &mut device,
            &mut num_devices
        ),
        Ok(())
    );

    assert_eq!(num_devices, 1);

    (device, platform)
}

pub fn setup_context() -> (cl_context, cl_device_id, cl_platform_id) {
    let (device, platform) = setup_device();

    let context = create_context(ptr::null(), 1, &device, None, ptr::null_mut());
    assert!(context.is_ok());

    (context.unwrap(), device, platform)
}

pub fn setup_program() -> (cl_program, cl_context, cl_device_id, cl_platform_id) {
    let (context, device, platform) = setup_context();

    let source_strings = CString::new(TEST_KERNEL_SOURCE).expect("Failed to create CString");
    let mut strings = [source_strings.as_ptr()];
    let program = create_program_with_source(
        context,
        strings.len() as _,
        strings.as_mut_ptr(),
        ptr::null(),
    );

    assert!(program.is_ok());
    (program.unwrap(), context, device, platform)
}

pub fn setup_and_build_program() -> (cl_program, cl_context, cl_device_id, cl_platform_id) {
    let (program, context, device, platform) = setup_program();
    let devices = [device];

    let ret = build_program(
        program,
        devices.len() as u32,
        devices.as_ptr(),
        ptr::null_mut(),
        None,
        ptr::null_mut(),
    );

    assert!(ret.is_ok());

    (program, context, device, platform)
}
