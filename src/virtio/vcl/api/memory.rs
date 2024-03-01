/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::context::*;
use crate::core::memory::*;
use crate::dev::renderer::Vcl;

use mesa_rust_util::properties::*;
use vcl_opencl_gen::*;
use vcl_proc_macros::*;

use std::ffi::c_void;
use std::ptr;
use std::sync::Arc;

fn validate_mem_flags(flags: cl_mem_flags, images: bool) -> CLResult<()> {
    let mut valid_flags = cl_bitfield::from(
        CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY | CL_MEM_KERNEL_READ_AND_WRITE,
    );

    if !images {
        valid_flags |= cl_bitfield::from(
            CL_MEM_USE_HOST_PTR
                | CL_MEM_ALLOC_HOST_PTR
                | CL_MEM_COPY_HOST_PTR
                | CL_MEM_HOST_WRITE_ONLY
                | CL_MEM_HOST_READ_ONLY
                | CL_MEM_HOST_NO_ACCESS,
        );
    }

    let read_write_group =
        cl_bitfield::from(CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY);

    let alloc_host_group = cl_bitfield::from(CL_MEM_ALLOC_HOST_PTR | CL_MEM_USE_HOST_PTR);

    let copy_host_group = cl_bitfield::from(CL_MEM_COPY_HOST_PTR | CL_MEM_USE_HOST_PTR);

    let host_read_write_group =
        cl_bitfield::from(CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS);

    if (flags & !valid_flags != 0)
        || (flags & read_write_group).count_ones() > 1
        || (flags & alloc_host_group).count_ones() > 1
        || (flags & copy_host_group).count_ones() > 1
        || (flags & host_read_write_group).count_ones() > 1
    {
        return Err(CL_INVALID_VALUE);
    }
    Ok(())
}

fn validate_host_ptr(host_ptr: *mut c_void, flags: cl_mem_flags) -> CLResult<()> {
    // CL_INVALID_HOST_PTR if host_ptr is NULL and CL_MEM_USE_HOST_PTR or CL_MEM_COPY_HOST_PTR are
    // set in flags
    if host_ptr.is_null()
        && flags & (cl_mem_flags::from(CL_MEM_USE_HOST_PTR | CL_MEM_COPY_HOST_PTR)) != 0
    {
        return Err(CL_INVALID_HOST_PTR);
    }

    // or if host_ptr is not NULL but CL_MEM_COPY_HOST_PTR or CL_MEM_USE_HOST_PTR are not set in
    // flags.
    if !host_ptr.is_null()
        && flags & (cl_mem_flags::from(CL_MEM_USE_HOST_PTR | CL_MEM_COPY_HOST_PTR)) == 0
    {
        return Err(CL_INVALID_HOST_PTR);
    }

    Ok(())
}

fn validate_size(ctx: &Context, size: usize) -> CLResult<()> {
    // CL_INVALID_BUFFER_SIZE if size is 0
    if size == 0 {
        return Err(CL_INVALID_BUFFER_SIZE);
    }

    // CL_INVALID_BUFFER_SIZE if size is greater than
    // CL_DEVICE_MAX_MEM_ALLOC_SIZE for all devices in context
    for dev in &ctx.devices {
        if size <= dev.max_mem_alloc_size as usize {
            return Ok(());
        }
    }
    Err(CL_INVALID_BUFFER_SIZE)
}

#[cl_entrypoint(clCreateBufferWithProperties)]
fn create_buffer_with_properties(
    context: cl_context,
    properties: *const cl_mem_properties,
    flags: cl_mem_flags,
    size: usize,
    host_ptr: *mut c_void,
) -> CLResult<cl_mem> {
    let ctx = context.get_arc()?;

    // CL_INVALID_VALUE if values specified in flags are not valid as defined in the Memory Flags table.
    validate_mem_flags(flags, false)?;
    validate_size(&ctx, size)?;
    validate_host_ptr(host_ptr, flags)?;

    let props = Properties::from_ptr_raw(properties);
    // CL_INVALID_PROPERTY if a property name in properties is not a supported property name, if
    // the value specified for a supported property name is not valid, or if the same property name
    // is specified more than once.
    if props.len() > 1 {
        // we don't support any properties besides the 0 property
        return Err(CL_INVALID_PROPERTY);
    }

    Mem::new_buffer(&ctx, flags, size, host_ptr)
}

#[cl_entrypoint(clCreateBuffer)]
fn create_buffer(
    context: cl_context,
    flags: cl_mem_flags,
    size: usize,
    host_ptr: *mut c_void,
) -> CLResult<cl_mem> {
    create_buffer_with_properties(context, ptr::null(), flags, size, host_ptr)
}

#[cl_entrypoint(clRetainMemObject)]
fn retain_mem_object(mem: cl_mem) -> CLResult<()> {
    mem.retain()
}

#[cl_entrypoint(clReleaseMemObject)]
fn release_mem_object(mem: cl_mem) -> CLResult<()> {
    let arc_mem = mem.from_raw()?;
    if Arc::strong_count(&arc_mem) == 1 {
        Vcl::get().call_clReleaseMemObject(mem)?;
    }
    Ok(())
}

#[cl_entrypoint(clEnqueueReadBuffer)]
fn enqueue_read_buffer(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_read: cl_bool,
    offset: usize,
    size: usize,
    ptr: *mut c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let buf = buffer.get_arc()?;
    check_cl_bool(blocking_read).ok_or(CL_INVALID_VALUE)?;

    // CL_INVALID_VALUE if the region being read or written specified by (offset, size) is out of
    // bounds or if ptr is a NULL value.
    if offset + size > buf.size || ptr.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_CONTEXT if the context associated with command_queue and buffer are not the same
    if buf.context != queue.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_OPERATION if clEnqueueReadBuffer is called on buffer which has been created with
    // CL_MEM_HOST_WRITE_ONLY or CL_MEM_HOST_NO_ACCESS.
    if bit_check(buf.flags, CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_NO_ACCESS) {
        return Err(CL_INVALID_OPERATION);
    }

    Vcl::get().call_clEnqueueReadBuffer(
        command_queue,
        buffer,
        blocking_read,
        offset,
        size,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
    )
}

#[cl_entrypoint(clEnqueueWriteBuffer)]
fn enqueue_write_buffer(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_write: cl_bool,
    offset: usize,
    size: usize,
    ptr: *const c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let buf = buffer.get_arc()?;
    check_cl_bool(blocking_write).ok_or(CL_INVALID_VALUE)?;

    // CL_INVALID_VALUE if the region being read or written specified by (offset, size) is out of
    // bounds or if ptr is a NULL value.
    if offset + size > buf.size || ptr.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_CONTEXT if the context associated with command_queue and buffer are not the same
    if buf.context != queue.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_OPERATION if clEnqueueWriteBuffer is called on buffer which has been created with
    // CL_MEM_HOST_READ_ONLY or CL_MEM_HOST_NO_ACCESS.
    if bit_check(buf.flags, CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS) {
        return Err(CL_INVALID_OPERATION);
    }

    Vcl::get().call_clEnqueueWriteBuffer(
        command_queue,
        buffer,
        blocking_write,
        offset,
        size,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
    )
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::api::context::*;
    use crate::api::device::get_device_ids;
    use crate::api::platform::get_platform_ids;

    use std::ptr;

    fn get_context() -> cl_context {
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
        ret.unwrap()
    }

    #[test]
    fn test_create_buffer() {
        let context = get_context();
        let ret = create_buffer(context, 0, 1024, ptr::null_mut());
        let buffer = ret.unwrap();
        assert!(release_mem_object(buffer).is_ok());
        assert!(release_context(context).is_ok());
    }
}
