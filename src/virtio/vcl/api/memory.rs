/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::types::*;
use crate::api::util::*;
use crate::cl_closure;
use crate::core::context::*;
use crate::core::event::Event;
use crate::core::format::CLFormatInfo;
use crate::core::memory::*;
use crate::dev::renderer::Vcl;

use mesa_rust_util::properties::*;
use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;
use vcl_proc_macros::*;

use std::ffi::c_void;
use std::mem::MaybeUninit;
use std::sync::Arc;
use std::*;

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

fn image_type_valid(image_type: cl_mem_object_type) -> bool {
    CL_IMAGE_TYPES.contains(&image_type)
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

#[cl_entrypoint(clCreateSubBuffer)]
fn create_sub_buffer(
    buffer: cl_mem,
    flags: cl_mem_flags,
    buffer_create_type: cl_buffer_create_type,
    buffer_create_info: *const c_void,
) -> CLResult<cl_mem> {
    buffer.get_ref()?;

    if buffer_create_info.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    Mem::new_sub_buffer(buffer, flags, buffer_create_type, buffer_create_info)
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
        let mem = self.get_ref()?;
        Ok(match q.0 {
            CL_MEM_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&mem.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
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

    if param_name.0 == CL_MEM_REFERENCE_COUNT || param_name.0 == CL_MEM_CONTEXT {
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

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
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
        ev_ptr,
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

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
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
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())
}

#[cl_entrypoint(clEnqueueCopyBuffer)]
fn enqueue_copy_buffer(
    command_queue: cl_command_queue,
    src_buffer: cl_mem,
    dst_buffer: cl_mem,
    src_offset: usize,
    dst_offset: usize,
    size: usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let src = src_buffer.get_arc()?;
    let dst = dst_buffer.get_arc()?;

    // CL_INVALID_CONTEXT if the context associated with command_queue, src_buffer and dst_buffer
    // are not the same
    if queue.context != src.context || queue.context != dst.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_VALUE if src_offset, dst_offset, size, src_offset + size or dst_offset + size
    // require accessing elements outside the src_buffer and dst_buffer buffer objects respectively.
    if src_offset + size > src.size || dst_offset + size > dst.size {
        return Err(CL_INVALID_VALUE);
    }

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueCopyBuffer(
        command_queue,
        src_buffer,
        dst_buffer,
        src_offset,
        dst_offset,
        size,
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())
}

#[cl_entrypoint(clEnqueueCopyBufferRect)]
fn enqueue_copy_buffer_rect(
    command_queue: cl_command_queue,
    src_buffer: cl_mem,
    dst_buffer: cl_mem,
    src_origin: *const usize,
    dst_origin: *const usize,
    region: *const usize,
    src_row_pitch: usize,
    src_slice_pitch: usize,
    dst_row_pitch: usize,
    dst_slice_pitch: usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    src_buffer.get_ref()?;
    dst_buffer.get_ref()?;

    // CL_INVALID_VALUE if src_origin, dst_origin, or region is NULL.
    if src_origin.is_null() || dst_origin.is_null() || region.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueCopyBufferRect(
        command_queue,
        src_buffer,
        dst_buffer,
        src_origin,
        dst_origin,
        region,
        src_row_pitch,
        src_slice_pitch,
        dst_row_pitch,
        dst_slice_pitch,
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())
}

#[cl_entrypoint(clEnqueueFillBuffer)]
fn enqueue_fill_buffer(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    pattern: *const c_void,
    pattern_size: usize,
    offset: usize,
    size: usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let buf = buffer.get_arc()?;

    // CL_INVALID_VALUE if offset or offset + size require accessing elements outside the buffer
    // buffer object respectively.
    if offset + size > buf.size {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE if pattern is NULL or if pattern_size is 0 or if pattern_size is not one of
    // { 1, 2, 4, 8, 16, 32, 64, 128 }.
    if pattern.is_null() || pattern_size.count_ones() != 1 || pattern_size > 128 {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE if offset and size are not a multiple of pattern_size.
    if offset % pattern_size != 0 || size % pattern_size != 0 {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_CONTEXT if the context associated with command_queue and buffer are not the same
    if buf.context != queue.context {
        return Err(CL_INVALID_CONTEXT);
    }

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueFillBuffer(
        command_queue,
        buffer,
        pattern_size,
        pattern,
        offset,
        size,
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())
}

#[cl_entrypoint(clEnqueueMigrateMemObjects)]
fn enqueue_migrate_mem_objects(
    command_queue: cl_command_queue,
    num_mem_objects: cl_uint,
    mem_objects: *const cl_mem,
    flags: cl_mem_migration_flags,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let bufs = cl_mem::get_arc_vec_from_arr(mem_objects, num_mem_objects)?;

    // CL_INVALID_VALUE if num_mem_objects is zero or if mem_objects is NULL.
    if bufs.is_empty() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_CONTEXT if the context associated with command_queue and memory objects in
    // mem_objects are not the same
    if bufs.iter().any(|b| b.context != queue.context) {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_VALUE if flags is not 0 or is not any of the values described in the table above.
    if flags != 0
        && bit_check(
            flags,
            !(CL_MIGRATE_MEM_OBJECT_HOST | CL_MIGRATE_MEM_OBJECT_CONTENT_UNDEFINED),
        )
    {
        return Err(CL_INVALID_VALUE);
    }

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueMigrateMemObjects(
        command_queue,
        num_mem_objects,
        mem_objects,
        flags,
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())
}

#[cl_entrypoint(clSetMemObjectDestructorCallback)]
fn set_mem_object_destructor_callback(
    memobj: cl_mem,
    pfn_notify: Option<MemCB>,
    user_data: *mut c_void,
) -> CLResult<()> {
    let mem = memobj.get_ref()?;

    // CL_INVALID_VALUE if pfn_notify is NULL.
    if pfn_notify.is_none() {
        return Err(CL_INVALID_VALUE);
    }

    mem.callbacks
        .lock()
        .unwrap()
        .push(cl_closure!(|mem| pfn_notify(mem, user_data)));
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

#[cl_entrypoint(clCreateSampler)]
fn create_sampler(
    context: cl_context,
    normalized_coords: cl_bool,
    addressing_mode: cl_addressing_mode,
    filter_mode: cl_filter_mode,
) -> CLResult<cl_sampler> {
    let normalized_coords = check_cl_bool(normalized_coords).ok_or(CL_INVALID_VALUE)?;
    // CL_INVALID_VALUE if addressing_mode, filter_mode, normalized_coords or a combination of these
    // arguements are not valid.
    validate_addressing_mode(addressing_mode)?;
    validate_filter_mode(filter_mode)?;

    let sampler = Sampler::new(
        context.get_arc()?,
        normalized_coords,
        addressing_mode,
        filter_mode,
    )?;

    Ok(cl_sampler::from_arc(sampler))
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

    let normalized_coords = check_cl_bool(normalized_coords).ok_or(CL_INVALID_VALUE)?;
    // CL_INVALID_VALUE if addressing_mode, filter_mode, normalized_coords or a combination of these
    // arguements are not valid.
    validate_addressing_mode(addressing_mode)?;
    validate_filter_mode(filter_mode)?;

    let sampler = Sampler::new_with_properties(
        context.get_arc()?,
        normalized_coords,
        addressing_mode,
        filter_mode,
        props,
    )?;

    Ok(cl_sampler::from_arc(sampler))
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

#[cl_entrypoint(clCreateImage)]
fn create_image(
    context: cl_context,
    flags: cl_mem_flags,
    image_format: *const cl_image_format,
    image_desc: *const cl_image_desc,
    host_ptr: *mut c_void,
) -> CLResult<cl_mem> {
    let ctx = context.get_arc()?;

    if image_format.is_null() {
        return Err(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR);
    }
    if image_desc.is_null() {
        return Err(CL_INVALID_IMAGE_DESCRIPTOR);
    }

    Ok(cl_mem::from_arc(Mem::new_image(
        ctx,
        unsafe { *image_desc },
        flags,
        unsafe { *image_format },
        host_ptr,
    )?))
}

fn validate_image_format<'a>(
    image_format: *const cl_image_format,
) -> CLResult<(&'a cl_image_format, u8)> {
    // CL_INVALID_IMAGE_FORMAT_DESCRIPTOR ... if image_format is NULL.
    let format = unsafe { image_format.as_ref() }.ok_or(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR)?;
    let pixel_size = format
        .pixel_size()
        .ok_or(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR)?;

    // special validation
    let valid_combination = match format.image_channel_data_type {
        CL_UNORM_SHORT_565 | CL_UNORM_SHORT_555 | CL_UNORM_INT_101010 => {
            [CL_RGB, CL_RGBx].contains(&format.image_channel_order)
        }
        CL_UNORM_INT_101010_2 => format.image_channel_order == CL_RGBA,
        _ => true,
    };
    if !valid_combination {
        return Err(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR);
    }

    Ok((format, pixel_size))
}

pub trait CLImageDescInfo {
    fn type_info(&self) -> (u8, bool);
    fn size(&self) -> CLVec<usize>;

    fn dims(&self) -> u8 {
        self.type_info().0
    }

    fn dims_with_array(&self) -> u8 {
        let array: u8 = self.is_array().into();
        self.dims() + array
    }

    fn has_slice(&self) -> bool {
        self.dims() == 3 || self.is_array()
    }

    fn is_array(&self) -> bool {
        self.type_info().1
    }
}

impl CLImageDescInfo for cl_image_desc {
    fn type_info(&self) -> (u8, bool) {
        match self.image_type {
            CL_MEM_OBJECT_IMAGE1D | CL_MEM_OBJECT_IMAGE1D_BUFFER => (1, false),
            CL_MEM_OBJECT_IMAGE1D_ARRAY => (1, true),
            CL_MEM_OBJECT_IMAGE2D => (2, false),
            CL_MEM_OBJECT_IMAGE2D_ARRAY => (2, true),
            CL_MEM_OBJECT_IMAGE3D => (3, false),
            _ => panic!("unknown image_type {:x}", self.image_type),
        }
    }

    fn size(&self) -> CLVec<usize> {
        let mut height = cmp::max(self.image_height, 1);
        let mut depth = cmp::max(self.image_depth, 1);

        match self.image_type {
            CL_MEM_OBJECT_IMAGE1D_ARRAY => height = self.image_array_size,
            CL_MEM_OBJECT_IMAGE2D_ARRAY => depth = self.image_array_size,
            _ => {}
        }

        CLVec::new([self.image_width, height, depth])
    }
}

#[cl_entrypoint(clCreateImageWithProperties)]
fn create_image_with_properties(
    context: cl_context,
    properties: *const cl_mem_properties,
    flags: cl_mem_flags,
    image_format: *const cl_image_format,
    image_desc: *const cl_image_desc,
    host_ptr: *mut c_void,
) -> CLResult<cl_mem> {
    let ctx: Arc<Context> = context.get_arc()?;

    // CL_INVALID_OPERATION if there are no devices in context that support images (i.e.
    // CL_DEVICE_IMAGE_SUPPORT specified in the Device Queries table is CL_FALSE).
    ctx.devices
        .iter()
        .find(|d| d.image_supported())
        .ok_or(CL_INVALID_OPERATION)?;

    // TODO: use element size for calculating image size?
    let (format, _elem_size) = validate_image_format(image_format)?;

    if image_desc.is_null() {
        return Err(CL_INVALID_IMAGE_DESCRIPTOR);
    }

    // validate host_ptr before merging flags
    validate_host_ptr(host_ptr, flags)?;

    validate_mem_flags(flags, false)?;

    let props = Properties::from_ptr(properties).ok_or(CL_INVALID_VALUE)?;

    Ok(cl_mem::from_arc(Mem::new_image_with_properties(
        ctx,
        unsafe { *image_desc },
        flags,
        *format,
        host_ptr,
        props,
    )?))
}

#[cl_entrypoint(clCreateImage2D)]
fn create_image_2d(
    context: cl_context,
    flags: cl_mem_flags,
    image_format: *const cl_image_format,
    image_width: usize,
    image_height: usize,
    image_row_pitch: usize,
    host_ptr: *mut c_void,
) -> CLResult<cl_mem> {
    if image_format.is_null() {
        return Err(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR);
    }
    let image_desc = cl_image_desc {
        image_type: CL_MEM_OBJECT_IMAGE2D,
        image_width: image_width,
        image_height: image_height,
        image_row_pitch: image_row_pitch,
        ..Default::default()
    };
    Ok(cl_mem::from_arc(Mem::new_image_2d(
        &context.get_arc()?,
        image_desc,
        flags,
        unsafe { *image_format },
        host_ptr,
    )?))
}

#[cl_entrypoint(clCreateImage3D)]
fn create_image_3d(
    context: cl_context,
    flags: cl_mem_flags,
    image_format: *const cl_image_format,
    image_width: usize,
    image_height: usize,
    image_depth: usize,
    image_row_pitch: usize,
    image_slice_pitch: usize,
    host_ptr: *mut c_void,
) -> CLResult<cl_mem> {
    if image_format.is_null() {
        return Err(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR);
    }
    let image_desc = cl_image_desc {
        image_type: CL_MEM_OBJECT_IMAGE3D,
        image_width: image_width,
        image_height: image_height,
        image_depth: image_depth,
        image_row_pitch: image_row_pitch,
        image_slice_pitch: image_slice_pitch,
        ..Default::default()
    };
    Ok(cl_mem::from_arc(Mem::new_image_3d(
        &context.get_arc()?,
        image_desc,
        flags,
        unsafe { *image_format },
        host_ptr,
    )?))
}

#[cl_entrypoint(clGetSupportedImageFormats)]
fn get_supported_image_formats(
    context: cl_context,
    flags: cl_mem_flags,
    image_type: cl_mem_object_type,
    num_entries: cl_uint,
    image_formats: *mut cl_image_format,
    num_image_formats: *mut cl_uint,
) -> CLResult<()> {
    context.get_ref()?;

    // CL_INVALID_VALUE if flags
    validate_mem_flags(flags, true)?;

    // or image_type are not valid
    if !image_type_valid(image_type) {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE ... if num_entries is 0 and image_formats is not NULL.
    if num_entries == 0 && !image_formats.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    Vcl::get().call_clGetSupportedImageFormats(
        context,
        flags,
        image_type,
        num_entries,
        image_formats,
        num_image_formats,
    )
}

#[cl_entrypoint(clGetImageInfo)]
fn get_image_info(
    image: cl_mem,
    param_name: cl_image_info,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    image.get_ref()?;

    let mut size = 0;
    Vcl::get().call_clGetImageInfo(image, param_name, param_value_size, param_value, &mut size)?;

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

fn validate_image_bounds(i: &Mem, origin: CLVec<usize>, region: CLVec<usize>) -> CLResult<()> {
    let dims = i.image_desc.dims_with_array();
    let bound = region + origin;
    if bound > i.image_desc.size() {
        return Err(CL_INVALID_VALUE);
    }

    // If image is a 2D image object, origin[2] must be 0. If image is a 1D image or 1D image buffer
    // object, origin[1] and origin[2] must be 0. If image is a 1D image array object, origin[2]
    // must be 0.
    if dims < 3 && origin[2] != 0 || dims < 2 && origin[1] != 0 {
        return Err(CL_INVALID_VALUE);
    }

    // If image is a 2D image object, region[2] must be 1. If image is a 1D image or 1D image buffer
    // object, region[1] and region[2] must be 1. If image is a 1D image array object, region[2]
    // must be 1. The values in region cannot be 0.
    if dims < 3 && region[2] != 1 || dims < 2 && region[1] != 1 || region.contains(&0) {
        return Err(CL_INVALID_VALUE);
    }

    Ok(())
}

#[cl_entrypoint(clEnqueueReadImage)]
fn enqueue_read_image(
    command_queue: cl_command_queue,
    image: cl_mem,
    blocking_read: cl_bool,
    origin: *const usize,
    region: *const usize,
    row_pitch: usize,
    slice_pitch: usize,
    ptr: *mut c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let i = image.get_arc()?;
    check_cl_bool(blocking_read).ok_or(CL_INVALID_VALUE)?;
    let pixel_size = i.image_format.pixel_size().unwrap() as usize;

    // CL_INVALID_CONTEXT if the context associated with command_queue and image are not the same
    if i.context != queue.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_OPERATION if clEnqueueReadImage is called on image which has been created with
    // CL_MEM_HOST_WRITE_ONLY or CL_MEM_HOST_NO_ACCESS.
    if bit_check(i.flags, CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_NO_ACCESS) {
        return Err(CL_INVALID_OPERATION);
    }

    // CL_INVALID_VALUE if origin or region is NULL.
    // CL_INVALID_VALUE if ptr is NULL.
    if origin.is_null() || region.is_null() || ptr.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE if image is a 1D or 2D image and slice_pitch or input_slice_pitch is not 0.
    if !i.image_desc.has_slice() && slice_pitch != 0 {
        return Err(CL_INVALID_VALUE);
    }

    let r = unsafe { CLVec::from_raw(region) };
    let o = unsafe { CLVec::from_raw(origin) };

    // CL_INVALID_VALUE if the region being read or written specified by origin and region is out of
    // bounds.
    // CL_INVALID_VALUE if values in origin and region do not follow rules described in the argument
    // description for origin and region.
    validate_image_bounds(&i, o, r)?;

    let size = r[0] * r[1] * r[2] * pixel_size;

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueReadImageMESA(
        queue.get_handle(),
        i.get_handle(),
        blocking_read,
        origin,
        region,
        row_pitch,
        slice_pitch,
        size,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())

    //• CL_INVALID_IMAGE_SIZE if image dimensions (image width, height, specified or compute row and/or slice pitch) for image are not supported by device associated with queue.
    //• CL_IMAGE_FORMAT_NOT_SUPPORTED if image format (image channel order and data type) for image are not supported by device associated with queue.
    //• CL_INVALID_OPERATION if the device associated with command_queue does not support images (i.e. CL_DEVICE_IMAGE_SUPPORT specified in the Device Queries table is CL_FALSE).
    //• CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST if the read and write operations are blocking and the execution status of any of the events in event_wait_list is a negative integer value.
}

#[cl_entrypoint(clEnqueueWriteImage)]
fn enqueue_write_image(
    command_queue: cl_command_queue,
    image: cl_mem,
    blocking_write: cl_bool,
    origin: *const usize,
    region: *const usize,
    row_pitch: usize,
    slice_pitch: usize,
    ptr: *const ::std::os::raw::c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let i = image.get_arc()?;
    check_cl_bool(blocking_write).ok_or(CL_INVALID_VALUE)?;
    let pixel_size = i.image_format.pixel_size().unwrap() as usize;

    // CL_INVALID_CONTEXT if the context associated with command_queue and image are not the same
    if i.context != queue.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_OPERATION if clEnqueueWriteImage is called on image which has been created with
    // CL_MEM_HOST_READ_ONLY or CL_MEM_HOST_NO_ACCESS.
    if bit_check(i.flags, CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS) {
        return Err(CL_INVALID_OPERATION);
    }

    // CL_INVALID_VALUE if origin or region is NULL.
    // CL_INVALID_VALUE if ptr is NULL.
    if origin.is_null() || region.is_null() || ptr.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE if image is a 1D or 2D image and slice_pitch or input_slice_pitch is not 0.
    if !i.image_desc.has_slice() && slice_pitch != 0 {
        return Err(CL_INVALID_VALUE);
    }

    let r = unsafe { CLVec::from_raw(region) };
    let o = unsafe { CLVec::from_raw(origin) };

    // CL_INVALID_VALUE if the region being read or written specified by origin and region is out of
    // bounds.
    // CL_INVALID_VALUE if values in origin and region do not follow rules described in the argument
    // description for origin and region.
    validate_image_bounds(&i, o, r)?;

    let size = r[0] * r[1] * r[2] * pixel_size;

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueWriteImageMESA(
        queue.get_handle(),
        i.get_handle(),
        blocking_write,
        origin,
        region,
        row_pitch,
        slice_pitch,
        size,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())

    //• CL_INVALID_IMAGE_SIZE if image dimensions (image width, height, specified or compute row and/or slice pitch) for image are not supported by device associated with queue.
    //• CL_IMAGE_FORMAT_NOT_SUPPORTED if image format (image channel order and data type) for image are not supported by device associated with queue.
    //• CL_INVALID_OPERATION if the device associated with command_queue does not support images (i.e. CL_DEVICE_IMAGE_SUPPORT specified in the Device Queries table is CL_FALSE).
    //• CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST if the read and write operations are blocking and the execution status of any of the events in event_wait_list is a negative integer value.
}

#[cl_entrypoint(clEnqueueCopyImage)]
fn enqueue_copy_image(
    command_queue: cl_command_queue,
    src_image: cl_mem,
    dst_image: cl_mem,
    src_origin: *const usize,
    dst_origin: *const usize,
    region: *const usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let src_image = src_image.get_arc()?;
    let dst_image = dst_image.get_arc()?;

    // CL_INVALID_CONTEXT if the context associated with command_queue, src_image and dst_image are not the same
    if src_image.context != queue.context || dst_image.context != queue.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_IMAGE_FORMAT_MISMATCH if src_image and dst_image do not use the same image format.
    if src_image.image_format != dst_image.image_format {
        return Err(CL_IMAGE_FORMAT_MISMATCH);
    }

    // CL_INVALID_VALUE if src_origin, dst_origin, or region is NULL.
    if src_origin.is_null() || dst_origin.is_null() || region.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    let region = unsafe { CLVec::from_raw(region) };
    let dst_origin = unsafe { CLVec::from_raw(dst_origin) };
    let src_origin = unsafe { CLVec::from_raw(src_origin) };

    // CL_INVALID_VALUE if the 2D or 3D rectangular region specified by src_origin and
    // src_origin + region refers to a region outside src_image, or if the 2D or 3D rectangular
    // region specified by dst_origin and dst_origin + region refers to a region outside dst_image.
    // CL_INVALID_VALUE if values in src_origin, dst_origin and region do not follow rules described
    // in the argument description for src_origin, dst_origin and region.
    validate_image_bounds(&src_image, src_origin, region)?;
    validate_image_bounds(&dst_image, dst_origin, region)?;

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueCopyImage(
        queue.get_handle(),
        src_image.get_handle(),
        dst_image.get_handle(),
        src_origin.as_ptr(),
        dst_origin.as_ptr(),
        region.as_ptr(),
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())

    //• CL_INVALID_IMAGE_SIZE if image dimensions (image width, height, specified or compute row and/or slice pitch) for src_image or dst_image are not supported by device associated with queue.
    //• CL_IMAGE_FORMAT_NOT_SUPPORTED if image format (image channel order and data type) for src_image or dst_image are not supported by device associated with queue.
    //• CL_INVALID_OPERATION if the device associated with command_queue does not support images (i.e. CL_DEVICE_IMAGE_SUPPORT specified in the Device Queries table is CL_FALSE).
    //• CL_MEM_COPY_OVERLAP if src_image and dst_image are the same image object and the source and destination regions overlap.
}

#[cl_entrypoint(clEnqueueFillImage)]
fn enqueue_fill_image(
    command_queue: cl_command_queue,
    image: cl_mem,
    fill_color: *const c_void,
    origin: *const [usize; 3],
    region: *const [usize; 3],
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let image = image.get_arc()?;

    // CL_INVALID_CONTEXT if the context associated with command_queue and image are not the same
    if image.context != queue.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_VALUE if fill_color is NULL.
    // CL_INVALID_VALUE if origin or region is NULL.
    if fill_color.is_null() || origin.is_null() || region.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    let region = unsafe { CLVec::from_raw(region.cast()) };
    let origin = unsafe { CLVec::from_raw(origin.cast()) };

    // CL_INVALID_VALUE if the region being filled as specified by origin and region is out of
    // bounds.
    // CL_INVALID_VALUE if values in origin and region do not follow rules described in the argument
    // description for origin and region.
    validate_image_bounds(&image, origin, region)?;

    // Fill color is always a 4 component int value
    // TODO but not for CL_DEPTH
    let fill_color_size = 4 * mem::size_of::<cl_uint>();

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueFillImageMESA(
        command_queue,
        image.get_handle(),
        fill_color_size,
        fill_color,
        origin.as_ptr(),
        region.as_ptr(),
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())

    //• CL_INVALID_IMAGE_SIZE if image dimensions (image width, height, specified or compute row and/or slice pitch) for image are not supported by device associated with queue.
    //• CL_IMAGE_FORMAT_NOT_SUPPORTED if image format (image channel order and data type) for
    //image are not supported by device associated with queue.
}

#[cl_entrypoint(clEnqueueCopyImageToBuffer)]
fn enqueue_copy_image_to_buffer(
    command_queue: cl_command_queue,
    src_image: cl_mem,
    dst_buffer: cl_mem,
    src_origin: *const usize,
    region: *const usize,
    dst_offset: usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let src = src_image.get_arc()?;
    let dst = dst_buffer.get_arc()?;

    // CL_INVALID_CONTEXT if the context associated with command_queue, src_image and dst_buffer
    // are not the same
    if queue.context != src.context || queue.context != dst.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_VALUE if src_origin or region is NULL.
    if src_origin.is_null() || region.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueCopyImageToBuffer(
        command_queue,
        src_image,
        dst_buffer,
        src_origin,
        region,
        dst_offset,
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())
}

#[cl_entrypoint(clEnqueueCopyBufferToImage)]
fn enqueue_copy_buffer_to_image(
    command_queue: cl_command_queue,
    src_buffer: cl_mem,
    dst_image: cl_mem,
    src_offset: usize,
    dst_origin: *const usize,
    region: *const usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let src = src_buffer.get_arc()?;
    let dst = dst_image.get_arc()?;

    // CL_INVALID_CONTEXT if the context associated with command_queue, src_image and dst_buffer
    // are not the same
    if queue.context != src.context || queue.context != dst.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_VALUE if dst_origin or region is NULL.
    if dst_origin.is_null() || region.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;
    let mut ev_handle = Event::maybe_new(&queue.context, event);
    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueCopyBufferToImage(
        command_queue,
        src_buffer,
        dst_image,
        src_offset,
        dst_origin,
        region,
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);
    Ok(())
}

fn validate_map_flags_common(map_flags: cl_mem_flags) -> CLResult<()> {
    // CL_INVALID_VALUE ... if values specified in map_flags are not valid.
    let valid_flags =
        cl_bitfield::from(CL_MAP_READ | CL_MAP_WRITE | CL_MAP_WRITE_INVALIDATE_REGION);
    let read_write_group = cl_bitfield::from(CL_MAP_READ | CL_MAP_WRITE);
    let invalidate_group = cl_bitfield::from(CL_MAP_WRITE_INVALIDATE_REGION);

    if (map_flags & !valid_flags != 0)
        || ((map_flags & read_write_group != 0) && (map_flags & invalidate_group != 0))
    {
        return Err(CL_INVALID_VALUE);
    }

    Ok(())
}

fn validate_map_flags(m: &Mem, map_flags: cl_mem_flags) -> CLResult<()> {
    validate_map_flags_common(map_flags)?;

    // CL_INVALID_OPERATION if buffer has been created with CL_MEM_HOST_WRITE_ONLY or
    // CL_MEM_HOST_NO_ACCESS and CL_MAP_READ is set in map_flags
    if bit_check(m.flags, CL_MEM_HOST_WRITE_ONLY | CL_MEM_HOST_NO_ACCESS) &&
      bit_check(map_flags, CL_MAP_READ) ||
      // or if buffer has been created with CL_MEM_HOST_READ_ONLY or CL_MEM_HOST_NO_ACCESS and
      // CL_MAP_WRITE or CL_MAP_WRITE_INVALIDATE_REGION is set in map_flags.
      bit_check(m.flags, CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS) &&
      bit_check(map_flags, CL_MAP_WRITE | CL_MAP_WRITE_INVALIDATE_REGION)
    {
        return Err(CL_INVALID_OPERATION);
    }

    Ok(())
}

#[cl_entrypoint(clEnqueueMapBuffer)]
fn enqueue_map_buffer(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_map: cl_bool,
    map_flags: cl_map_flags,
    offset: usize,
    size: usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<*mut c_void> {
    let q = command_queue.get_arc()?;
    let b = buffer.get_arc()?;
    check_cl_bool(blocking_map).ok_or(CL_INVALID_VALUE)?;
    event_list_from_cl(&q, num_events_in_wait_list, event_wait_list)?;

    validate_map_flags(&b, map_flags)?;

    // CL_INVALID_VALUE if region being mapped given by (offset, size) is out of bounds or if size
    // is 0
    if offset + size > b.size || size == 0 {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_CONTEXT if context associated with command_queue and buffer are not the same
    if b.context != q.context {
        return Err(CL_INVALID_CONTEXT);
    }

    let ptr = b.map_buffer(
        &q,
        map_flags,
        offset,
        size,
        num_events_in_wait_list,
        event_wait_list,
        event,
    )?;

    Ok(ptr)

    // TODO
    // CL_MISALIGNED_SUB_BUFFER_OFFSET if buffer is a sub-buffer object and offset specified when the sub-buffer object is created is not aligned to CL_DEVICE_MEM_BASE_ADDR_ALIGN value for the device associated with queue. This error code is missing before version 1.1.
    // CL_MAP_FAILURE if there is a failure to map the requested region into the host address space. This error cannot occur for buffer objects created with CL_MEM_USE_HOST_PTR or CL_MEM_ALLOC_HOST_PTR.
    // CL_INVALID_OPERATION if mapping would lead to overlapping regions being mapped for writing.
}

#[cl_entrypoint(clEnqueueMapImage)]
fn enqueue_map_image(
    command_queue: cl_command_queue,
    image: cl_mem,
    blocking_map: cl_bool,
    map_flags: cl_map_flags,
    origin: *const usize,
    region: *const usize,
    image_row_pitch: *mut usize,
    image_slice_pitch: *mut usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<*mut c_void> {
    let queue = command_queue.get_arc()?;
    let image = image.get_arc()?;
    check_cl_bool(blocking_map).ok_or(CL_INVALID_VALUE)?;
    event_list_from_cl(&queue, num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_VALUE ... or if values specified in map_flags are not valid.
    validate_map_flags(&image, map_flags)?;

    // CL_INVALID_CONTEXT if context associated with command_queue and image are not the same
    if image.context != queue.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_VALUE if origin or region is NULL.
    // CL_INVALID_VALUE if image_row_pitch is NULL.
    if origin.is_null() || region.is_null() || image_row_pitch.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    let region = unsafe { CLVec::from_raw(region) };
    let origin = unsafe { CLVec::from_raw(origin) };

    // CL_INVALID_VALUE if region being mapped given by (origin, origin + region) is out of bounds
    // CL_INVALID_VALUE if values in origin and region do not follow rules described in the argument
    // description for origin and region.
    validate_image_bounds(&image, origin, region)?;

    let mut dummy_slice_pitch: usize = 0;
    let image_slice_pitch = if image_slice_pitch.is_null() {
        // CL_INVALID_VALUE if image is a 3D image, 1D or 2D image array object and
        // image_slice_pitch is NULL.
        if image.image_desc.is_array() || image.image_desc.image_type == CL_MEM_OBJECT_IMAGE3D {
            return Err(CL_INVALID_VALUE);
        }
        &mut dummy_slice_pitch
    } else {
        unsafe { image_slice_pitch.as_mut().unwrap() }
    };

    let ptr = image.map_image(
        &queue,
        map_flags,
        origin,
        region,
        image_row_pitch,
        image_slice_pitch,
        num_events_in_wait_list,
        event_wait_list,
        event,
    )?;

    Ok(ptr)

    //• CL_INVALID_IMAGE_SIZE if image dimensions (image width, height, specified or compute row and/or slice pitch) for image are not supported by device associated with queue.
    //• CL_IMAGE_FORMAT_NOT_SUPPORTED if image format (image channel order and data type) for image are not supported by device associated with queue.
    //• CL_MAP_FAILURE if there is a failure to map the requested region into the host address space. This error cannot occur for image objects created with CL_MEM_USE_HOST_PTR or CL_MEM_ALLOC_HOST_PTR.
    //• CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST if the map operation is blocking and the execution status of any of the events in event_wait_list is a negative integer value.
    //• CL_INVALID_OPERATION if the device associated with command_queue does not support images (i.e. CL_DEVICE_IMAGE_SUPPORT specified in the Device Queries table is CL_FALSE).
    //• CL_INVALID_OPERATION if mapping would lead to overlapping regions being mapped for writing.
}

#[cl_entrypoint(clEnqueueUnmapMemObject)]
fn enqueue_unmap_mem_object(
    command_queue: cl_command_queue,
    memobj: cl_mem,
    mapped_ptr: *mut c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let q = command_queue.get_arc()?;
    let m = memobj.get_arc()?;
    event_list_from_cl(&q, num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_CONTEXT if context associated with command_queue and memobj are not the same
    if q.context != m.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_VALUE if mapped_ptr is not a valid pointer returned by clEnqueueMapBuffer or
    // clEnqueueMapImage for memobj.
    if !m.is_mapped_ptr(mapped_ptr) {
        return Err(CL_INVALID_VALUE);
    }

    m.unmap(
        &q,
        mapped_ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
    )?;

    Ok(())
}

#[cl_entrypoint(clCreatePipe)]
fn create_pipe(
    _context: cl_context,
    _flags: cl_mem_flags,
    _pipe_packet_size: cl_uint,
    _pipe_max_packets: cl_uint,
    _properties: *const cl_pipe_properties,
) -> CLResult<cl_mem> {
    Err(CL_INVALID_OPERATION)
}

#[cl_info_entrypoint(clGetPipeInfo)]
impl CLInfo<cl_pipe_info> for cl_mem {
    fn query(&self, _q: cl_pipe_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        // CL_INVALID_MEM_OBJECT if pipe is a not a valid pipe object.
        Err(CL_INVALID_MEM_OBJECT)
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
        let sampler = create_sampler(context, CL_TRUE, CL_ADDRESS_CLAMP, CL_FILTER_NEAREST);
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
