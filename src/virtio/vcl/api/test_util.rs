/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use vcl_opencl_gen::*;

use crate::api::context::*;
use crate::api::device::*;
use crate::api::platform::*;

use std::ptr;

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
