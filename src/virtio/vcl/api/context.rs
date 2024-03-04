/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::types::*;
use crate::api::util::*;
use crate::core::context::Context;
use crate::dev::renderer::Vcl;

use mesa_rust_util::properties::Properties;
use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;
use vcl_proc_macros::cl_info_entrypoint;

use std::collections::HashSet;
use std::ffi::*;
use std::mem::MaybeUninit;
use std::slice;
use std::sync::Arc;

#[cl_entrypoint(clCreateContext)]
pub fn create_context(
    properties: *const cl_context_properties,
    num_devices: cl_uint,
    devices: *const cl_device_id,
    pfn_notify: Option<CreateContextCB>,
    user_data: *mut c_void,
) -> CLResult<cl_context> {
    if devices.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    if num_devices == 0 {
        return Err(CL_INVALID_VALUE);
    }

    if pfn_notify.is_none() && !user_data.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_PLATFORM if no platform could be selected
    let platforms = Vcl::get().get_platforms();
    if platforms.is_empty() {
        return Err(CL_INVALID_PLATFORM);
    }

    // CL_INVALID_PROPERTY [...] if the same property name is specified more than once.
    let props = Properties::from_ptr(properties).ok_or(CL_INVALID_PROPERTY)?;
    let platform_id = find_platform_in_properties(&props)?;

    for p in &props.props {
        match p.0 as u32 {
            // Skip context platform as it's already been already handled
            CL_CONTEXT_PLATFORM => (),
            CL_CONTEXT_INTEROP_USER_SYNC => {
                check_cl_bool(p.1).ok_or(CL_INVALID_PROPERTY)?;
            }
            // CL_INVALID_PROPERTY if context property name in properties is not a supported property name
            _ => return Err(CL_INVALID_PROPERTY),
        }
    }

    let device_handles = unsafe { slice::from_raw_parts(devices, num_devices as usize) };

    let platform = platform_id.get_ref()?;
    for dev in device_handles {
        if !platform.contains_device(*dev) {
            // CL_INVALID_DEVICE if devices are not associated with the specified platform
            return Err(CL_INVALID_DEVICE);
        }
    }

    // Duplicate devices specified in devices are ignored.
    let set: HashSet<_> = HashSet::from_iter(device_handles.iter());
    let dev_results: Result<_, _> = set.into_iter().map(cl_device_id::get_ref).collect();
    let devs = dev_results?;

    let Ok(ctx) = Context::new(devs, props) else {
        return Err(CL_OUT_OF_RESOURCES);
    };

    Ok(cl_context::from_arc(ctx))
}

#[cl_entrypoint(clCreateContextFromType)]
fn create_context_from_type(
    properties: *const cl_context_properties,
    device_type: cl_device_type,
    pfn_notify: Option<CreateContextCB>,
    user_data: *mut c_void,
) -> CLResult<cl_context> {
    // CL_INVALID_DEVICE_TYPE if device_type is not a valid value.
    check_cl_device_type(device_type)?;

    // CL_INVALID_PLATFORM if no platform could be selected
    let platforms = Vcl::get().get_platforms();
    if platforms.is_empty() {
        return Err(CL_INVALID_PLATFORM);
    }

    // Find platform in properties
    let props = Properties::from_ptr(properties).ok_or(CL_INVALID_PROPERTY)?;
    let platform_id = find_platform_in_properties(&props)?;

    let devs = platform_id.get_ref()?.get_devices(device_type);
    // CL_DEVICE_NOT_FOUND if no OpenCL devices that matched device_type were found
    if devs.is_empty() {
        return Err(CL_DEVICE_NOT_FOUND);
    }

    let mut device_handles = Vec::with_capacity(devs.len());
    for dev in &devs {
        device_handles.push(dev.get_handle());
    }

    // errors are essentially the same and we will always pass in a valid
    // device list, so that's fine as well
    create_context(
        properties,
        devs.len() as u32,
        device_handles.as_ptr(),
        pfn_notify,
        user_data,
    )
}

fn find_platform_in_properties(properties: &Properties<isize>) -> CLResult<cl_platform_id> {
    for p in &properties.props {
        if p.0 as u32 == CL_CONTEXT_PLATFORM {
            let platform_id = p.1 as cl_platform_id;
            if !Vcl::get().contains_platform(platform_id) {
                // Platform value specified in properties is not a valid platform
                return Err(CL_INVALID_PLATFORM);
            }
            return Ok(platform_id);
        }
    }
    // Implementation-defined: get the first one
    Ok(Vcl::get().get_platforms()[0].get_handle())
}

#[cl_entrypoint(clRetainContext)]
fn retain_context(context: cl_context) -> CLResult<()> {
    context.retain()
}

#[cl_entrypoint(clReleaseContext)]
pub fn release_context(context: cl_context) -> CLResult<()> {
    // Restore the arc from the pointer and let it go out of scope
    // to decrement the refcount
    let arc_context = context.from_raw()?;
    if Arc::strong_count(&arc_context) == 1 {
        Vcl::get().call_clReleaseContext(context)?;
    }
    Ok(())
}

#[cl_info_entrypoint(clGetContextInfo)]
impl CLInfo<cl_context_info> for cl_context {
    fn query(&self, q: cl_context_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        let ctx = self.get_ref()?;
        Ok(match q {
            CL_CONTEXT_DEVICES => cl_prop::<Vec<cl_device_id>>(
                ctx.devices
                    .iter()
                    .map(|&d| cl_device_id::from_ptr(d))
                    .collect(),
            ),
            CL_CONTEXT_NUM_DEVICES => cl_prop::<cl_uint>(ctx.devices.len() as u32),
            CL_CONTEXT_PROPERTIES => cl_prop::<&Properties<cl_context_properties>>(&ctx.properties),
            CL_CONTEXT_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::api::device::get_device_ids;
    use crate::api::platform::get_platform_ids;

    use std::ptr;

    fn get_device() -> cl_device_id {
        let mut platform = ptr::null_mut();
        assert_eq!(get_platform_ids(1, &mut platform, ptr::null_mut()), Ok(()));

        let dev_ty = CL_DEVICE_TYPE_ALL as u64;

        let mut device = ptr::null_mut();
        let mut num_devices = 0;
        assert_eq!(
            get_device_ids(platform, dev_ty, 1, &mut device, &mut num_devices),
            Ok(())
        );
        assert_eq!(num_devices, 1);
        device
    }

    #[test]
    fn test_create_context() {
        let device = get_device();

        let ret = create_context(ptr::null(), 1, &device, None, ptr::null_mut());
        assert!(ret.is_ok());

        let context = ret.unwrap();
        assert!(release_context(context).is_ok());

        let properties = [42isize, 0];
        assert_eq!(
            create_context(&properties as _, 1, &device, None, ptr::null_mut()),
            Err(CL_INVALID_PROPERTY)
        );

        assert_eq!(
            create_context(ptr::null(), 1, ptr::null_mut(), None, ptr::null_mut()),
            Err(CL_INVALID_VALUE)
        );

        assert_eq!(
            create_context(ptr::null(), 0, &device, None, ptr::null_mut()),
            Err(CL_INVALID_VALUE)
        );

        let mut data = 42;
        assert_eq!(
            create_context(ptr::null(), 1, &device, None, &mut data as *mut _ as _),
            Err(CL_INVALID_VALUE)
        );

        let invalid_device = 42;
        assert_eq!(
            create_context(
                ptr::null(),
                1,
                &invalid_device as *const _ as _,
                None,
                ptr::null_mut()
            ),
            Err(CL_INVALID_DEVICE)
        )
    }
}
