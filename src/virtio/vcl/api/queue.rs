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
    // CL_INVALID_DEVICE if device [...] is not associated with context.
    let dev = ctx.get_device_from_handle(device)?;

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

    let ctx = context.get_arc()?;
    // CL_INVALID_DEVICE if device [...] is not associated with context.
    let dev = ctx.get_device_from_handle(device)?;

    Ok(cl_command_queue::from_arc(Queue::new_with_properties(
        ctx, dev, properties,
    )?))
}

#[cl_entrypoint(clEnqueueMarker)]
fn enqueue_marker(command_queue: cl_command_queue, event: *mut cl_event) -> CLResult<()> {
    let queue = command_queue.get_ref()?;
    Vcl::get().call_clEnqueueMarker(queue.get_handle(), event)
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
        let queue = self.get_ref()?;
        Ok(match q {
            CL_QUEUE_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            CL_QUEUE_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&queue.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_QUEUE_DEVICE => cl_prop::<cl_device_id>(cl_device_id::from_ptr(queue.device)),
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

    match param_name {
        CL_QUEUE_REFERENCE_COUNT | CL_QUEUE_CONTEXT | CL_QUEUE_DEVICE => {
            return queue.get_info(
                param_name,
                param_value_size,
                param_value,
                param_value_size_ret,
            )
        }
        _ => (),
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

#[cl_entrypoint(clEnqueueBarrier)]
fn enqueue_barrier(command_queue: cl_command_queue) -> CLResult<()> {
    let queue = command_queue.get_ref()?;
    Vcl::get().call_clEnqueueBarrier(queue.get_handle())
}

#[cl_entrypoint(clFlush)]
fn flush(queue: cl_command_queue) -> CLResult<()> {
    queue.get_ref()?;
    Vcl::get().call_clFlush(queue)
}

#[cl_entrypoint(clFinish)]
fn finish(queue: cl_command_queue) -> CLResult<()> {
    queue.get_ref()?;
    Vcl::get().call_clFinish(queue)
}

#[cfg(test)]
mod test {
    use super::*;

    use crate::api::context::release_context;
    use crate::api::test_util::*;

    #[test]
    fn test_create_queue() {
        let (context, device, _) = setup_context();

        let ret = create_command_queue(context, device, 0);
        assert!(ret.is_ok());

        let queue = ret.unwrap();
        assert!(release_command_queue(queue).is_ok());
        assert!(release_context(context).is_ok());
    }
}
