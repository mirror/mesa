/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::context::*;
use crate::core::event::Event;
use crate::core::memory::*;
use crate::dev::renderer::Vcl;

use mesa_rust_util::properties::*;
use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;
use vcl_proc_macros::*;

use std::ffi::c_void;
use std::mem::MaybeUninit;
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

impl CLInfo<cl_mem_info> for cl_mem {
    fn query(&self, q: cl_mem_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        Ok(match q {
            CL_MEM_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cl_entrypoint(clGetMemObjectInfo)]
fn get_mem_object_info(
    mem: cl_mem,
    param_name: cl_mem_info,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    mem.get_ref()?;

    if param_name == CL_MEM_REFERENCE_COUNT {
        return mem.get_info(
            param_name,
            param_value_size,
            param_value,
            param_value_size_ret,
        );
    }

    let mut size = 0;
    Vcl::get().call_clGetMemObjectInfo(
        mem,
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

    let mut ev_handle = if !event.is_null() {
        cl_event::from_arc(Arc::new(Event::default()))
    } else {
        ptr::null_mut()
    };

    Vcl::get().call_clEnqueueReadBuffer(
        command_queue,
        buffer,
        blocking_read,
        offset,
        size,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        &mut ev_handle,
    )?;

    event.write_checked(ev_handle);
    Ok(())
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

    let mut ev_handle = if !event.is_null() {
        cl_event::from_arc(Arc::new(Event::default()))
    } else {
        ptr::null_mut()
    };

    Vcl::get().call_clEnqueueWriteBuffer(
        command_queue,
        buffer,
        blocking_write,
        offset,
        size,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        &mut ev_handle,
    )?;

    event.write_checked(ev_handle);
    Ok(())
}

fn validate_addressing_mode(addressing_mode: cl_addressing_mode) -> CLResult<()> {
    match addressing_mode {
        CL_ADDRESS_NONE
        | CL_ADDRESS_CLAMP_TO_EDGE
        | CL_ADDRESS_CLAMP
        | CL_ADDRESS_REPEAT
        | CL_ADDRESS_MIRRORED_REPEAT => Ok(()),
        _ => Err(CL_INVALID_VALUE),
    }
}

fn validate_filter_mode(filter_mode: cl_filter_mode) -> CLResult<()> {
    match filter_mode {
        CL_FILTER_NEAREST | CL_FILTER_LINEAR => Ok(()),
        _ => Err(CL_INVALID_VALUE),
    }
}

fn create_sampler_impl(
    context: cl_context,
    normalized_coords: cl_bool,
    addressing_mode: cl_addressing_mode,
    filter_mode: cl_filter_mode,
    props: Properties<cl_sampler_properties>,
) -> CLResult<cl_sampler> {
    let context = context.get_arc()?;

    // CL_INVALID_VALUE if addressing_mode, filter_mode, normalized_coords or a combination of these
    // arguements are not valid.
    validate_addressing_mode(addressing_mode)?;
    validate_filter_mode(filter_mode)?;

    let sampler = Sampler::new(
        context,
        check_cl_bool(normalized_coords).ok_or(CL_INVALID_VALUE)?,
        addressing_mode,
        filter_mode,
        props,
    )?;

    Ok(cl_sampler::from_arc(sampler))
}

#[cl_entrypoint(clCreateSampler)]
fn create_sampler(
    context: cl_context,
    normalized_coords: cl_bool,
    addressing_mode: cl_addressing_mode,
    filter_mode: cl_filter_mode,
) -> CLResult<cl_sampler> {
    create_sampler_impl(
        context,
        normalized_coords,
        addressing_mode,
        filter_mode,
        Properties::default(),
    )
}

#[cl_entrypoint(clCreateSamplerWithProperties)]
fn create_sampler_with_properties(
    context: cl_context,
    props: *const cl_sampler_properties,
) -> CLResult<cl_sampler> {
    let mut normalized_coords = CL_TRUE;
    let mut addressing_mode = CL_ADDRESS_CLAMP;
    let mut filter_mode = CL_FILTER_NEAREST;

    // CL_INVALID_VALUE if the same property name is specified more than once.
    let props = Properties::from_ptr(props).ok_or(CL_INVALID_VALUE)?;

    for (key, val) in props.props.iter().copied() {
        match key as u32 {
            CL_SAMPLER_ADDRESSING_MODE => addressing_mode = val as u32,
            CL_SAMPLER_FILTER_MODE => filter_mode = val as u32,
            CL_SAMPLER_NORMALIZED_COORDS => normalized_coords = val as u32,
            // CL_INVALID_VALUE if the property name in sampler_properties is not a supported
            // property name
            _ => return Err(CL_INVALID_VALUE),
        }
    }

    create_sampler_impl(
        context,
        normalized_coords,
        addressing_mode,
        filter_mode,
        props,
    )
}

#[cl_entrypoint(clRetainSampler)]
fn retain_sampler(sampler: cl_sampler) -> CLResult<()> {
    sampler.retain()
}

#[cl_entrypoint(clReleaseSampler)]
fn release_sampler(sampler: cl_sampler) -> CLResult<()> {
    let arc_sampler = sampler.from_raw()?;
    if Arc::strong_count(&arc_sampler) == 1 {
        Vcl::get().call_clReleaseSampler(sampler)?;
    }
    Ok(())
}

#[cl_info_entrypoint(clGetSamplerInfo)]
impl CLInfo<cl_sampler_info> for cl_sampler {
    fn query(&self, q: cl_sampler_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        let sampler = self.get_ref()?;
        Ok(match q {
            CL_SAMPLER_ADDRESSING_MODE => cl_prop::<cl_addressing_mode>(sampler.addressing_mode),
            CL_SAMPLER_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&sampler.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_SAMPLER_FILTER_MODE => cl_prop::<cl_filter_mode>(sampler.filter_mode),
            CL_SAMPLER_NORMALIZED_COORDS => cl_prop::<bool>(sampler.normalized_coords),
            CL_SAMPLER_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            CL_SAMPLER_PROPERTIES => cl_prop::<&Properties<cl_sampler_properties>>(&sampler.props),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cfg(test)]
mod test {
    use super::*;

    use crate::api::context::release_context;
    use crate::api::test_util::*;

    use std::ptr;

    #[test]
    fn test_create_buffer() {
        let (context, _, _) = setup_context();
        let ret = create_buffer(context, 0, 1024, ptr::null_mut());
        let buffer = ret.unwrap();
        assert!(release_mem_object(buffer).is_ok());
        assert!(release_context(context).is_ok());
    }

    fn setup_sampler() -> (cl_sampler, cl_context, cl_device_id, cl_platform_id) {
        let (context, device, platform) = setup_context();

        // This line needs some kind of conditional compilation to choose
        // between create_sampler() and create_sampler_with_properties() based
        // on the OpenCL version used
        let sampler = create_sampler_with_properties(context, ptr::null_mut());
        assert!(sampler.is_ok());

        (sampler.unwrap(), context, device, platform)
    }

    #[test]
    fn test_retain_sampler() {
        let (sampler, _, _, _) = setup_sampler();

        assert!(retain_sampler(sampler).is_ok());
        assert!(release_sampler(sampler).is_ok());
        assert!(release_sampler(sampler).is_ok());
    }

    #[test]
    fn test_release_sampler() {
        let (sampler, _, _, _) = setup_sampler();

        assert!(release_sampler(sampler).is_ok());
    }

    #[test]
    fn test_get_sampler_info() {
        let (sampler, _, _, _) = setup_sampler();

        let param_value_size = std::mem::size_of::<cl_bool>();
        assert!(sampler
            .get_info(
                CL_SAMPLER_NORMALIZED_COORDS,
                param_value_size,
                ptr::null_mut(),
                ptr::null_mut()
            )
            .is_ok());
    }
}
