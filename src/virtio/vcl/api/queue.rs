/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::queue::Queue;
use crate::dev::renderer::Vcl;

use mesa_rust_util::properties::Properties;
use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;

use std::ffi::c_void;
use std::mem::MaybeUninit;
use std::sync::Arc;

fn valid_command_queue_properties(properties: cl_command_queue_properties) -> bool {
    let valid_flags =
        cl_bitfield::from(CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE);
    properties & !valid_flags == 0
}

fn supported_command_queue_properties(properties: cl_command_queue_properties) -> bool {
    let valid_flags = cl_bitfield::from(CL_QUEUE_PROFILING_ENABLE);
    properties & !valid_flags == 0
}

#[cl_entrypoint(clCreateCommandQueue)]
pub fn create_command_queue(
    context: cl_context,
    device: cl_device_id,
    properties: cl_command_queue_properties,
) -> CLResult<cl_command_queue> {
    // CL_INVALID_VALUE if values specified in properties are not valid.
    if !valid_command_queue_properties(properties) {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_QUEUE_PROPERTIES if values specified in properties are valid but are not supported by the device.
    if !supported_command_queue_properties(properties) {
        return Err(CL_INVALID_QUEUE_PROPERTIES);
    }

    let ctx = context.get_arc()?;
    let dev = device.get_ref()?;

    // CL_INVALID_DEVICE if device [...] is not associated with context.
    if !ctx.devices.contains(&dev) {
        return Err(CL_INVALID_DEVICE);
    }

    Ok(cl_command_queue::from_arc(Queue::new(
        ctx, dev, properties,
    )?))
}

#[cl_entrypoint(clCreateCommandQueueWithProperties)]
fn create_command_queue_with_properties(
    context: cl_context,
    device: cl_device_id,
    properties: *const cl_queue_properties,
) -> CLResult<cl_command_queue> {
    let properties = Properties::from_ptr(properties).ok_or(CL_INVALID_PROPERTY)?;

    for (key, _) in &properties.props {
        match *key as cl_uint {
            // CL_INVALID_QUEUE_PROPERTIES if values specified in properties are valid but are not
            // supported by the device.
            CL_QUEUE_SIZE => return Err(CL_INVALID_QUEUE_PROPERTIES),
            _ => return Err(CL_INVALID_PROPERTY),
        }
    }

    let ctx = context.get_arc()?;
    let dev = device.get_ref()?;

    // CL_INVALID_DEVICE if device [...] is not associated with context.
    if !ctx.devices.contains(&dev) {
        return Err(CL_INVALID_DEVICE);
    }

    Ok(cl_command_queue::from_arc(Queue::new_with_properties(
        ctx, dev, properties,
    )?))
}

#[cl_entrypoint(clRetainCommandQueue)]
fn retain_command_queue(queue: cl_command_queue) -> CLResult<()> {
    queue.retain()
}

#[cl_entrypoint(clReleaseCommandQueue)]
fn release_command_queue(queue: cl_command_queue) -> CLResult<()> {
    // Restore the arc from the pointer and let it go out of scope
    // to decrement the refcount
    let arc_queue = queue.from_raw()?;
    if Arc::strong_count(&arc_queue) == 1 {
        Vcl::get().call_clReleaseCommandQueue(queue)?;
    }
    Ok(())
}

impl CLInfo<cl_command_queue_info> for cl_command_queue {
    fn query(&self, q: cl_command_queue_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        Ok(match q {
            CL_QUEUE_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cl_entrypoint(clGetCommandQueueInfo)]
fn get_command_queue_info(
    queue: cl_command_queue,
    param_name: cl_command_queue_info,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    queue.get_ref()?;

    if param_name == CL_QUEUE_REFERENCE_COUNT {
        return queue.get_info(
            param_name,
            param_value_size,
            param_value,
            param_value_size_ret,
        );
    }

    let mut size = 0;
    Vcl::get().call_clGetCommandQueueInfo(
        queue,
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

#[cl_entrypoint(clSetCommandQueueProperty)]
fn set_command_queue_property(
    queue: cl_command_queue,
    properties: cl_command_queue_properties,
    enable: cl_bool,
    old_properties: *mut cl_command_queue_properties,
) -> CLResult<()> {
    queue.get_ref()?;
    Vcl::get().call_clSetCommandQueueProperty(queue, properties, enable, old_properties)
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::api::context::*;
    use crate::api::device::get_device_ids;
    use crate::api::platform::get_platform_ids;

    use std::ptr;

    fn get_device_and_context() -> (cl_device_id, cl_context) {
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

        let ret = create_context(ptr::null(), 1, &device, None, ptr::null_mut());
        let context = ret.unwrap();

        (device, context)
    }

    #[test]
    fn test_create_queue() {
        let (device, context) = get_device_and_context();

        let ret = create_command_queue(context, device, 0);
        assert!(ret.is_ok());

        let queue = ret.unwrap();
        assert!(release_command_queue(queue).is_ok());
        assert!(release_context(context).is_ok());
    }
}
