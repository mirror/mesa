/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::event::Event;
use crate::dev::renderer::Vcl;

use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;

use std::collections::HashSet;
use std::ffi::c_void;
use std::mem::MaybeUninit;
use std::sync::Arc;

#[cl_entrypoint(clCreateUserEvent)]
pub fn create_user_event(context: cl_context) -> CLResult<cl_event> {
    let ctx = context.get_arc()?;

    let Ok(event) = Event::new_user(&ctx) else {
        return Err(CL_OUT_OF_RESOURCES);
    };

    Ok(cl_event::from_arc(event))
}

#[cl_entrypoint(clSetUserEventStatus)]
fn set_user_event_status(event: cl_event, execution_status: cl_int) -> CLResult<()> {
    event.get_ref()?;

    if execution_status != CL_COMPLETE as i32 || execution_status < 0 {
        return Err(CL_INVALID_VALUE);
    }

    Vcl::get().call_clSetUserEventStatus(event, execution_status)
}

#[cl_entrypoint(clWaitForEvents)]
fn wait_for_events(num_events: cl_uint, event_list: *const cl_event) -> CLResult<()> {
    let evs = cl_event::get_arc_vec_from_arr(event_list, num_events)?;

    if evs.is_empty() {
        return Err(CL_INVALID_VALUE);
    }
    // CL_INVALID_CONTEXT if events specified in event_list do not belong to the same context.
    let contexts: HashSet<_> = evs.iter().map(|e| &e.context).collect();
    if contexts.len() != 1 {
        return Err(CL_INVALID_CONTEXT);
    }

    Vcl::get().call_clWaitForEvents(num_events, event_list)
}

impl CLInfo<cl_event_info> for cl_event {
    fn query(&self, info: cl_event_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        let event = self.get_ref()?;
        Ok(match info {
            CL_EVENT_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&event.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_EVENT_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cl_entrypoint(clGetEventInfo)]
fn get_event_info(
    event: cl_event,
    param_name: cl_event_info,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    let mut size = 0;

    if param_name == CL_EVENT_REFERENCE_COUNT || param_name == CL_EVENT_CONTEXT {
        return event.get_info(
            param_name,
            param_value_size,
            param_value,
            param_value_size_ret,
        );
    }

    Vcl::get().call_clGetEventInfo(event, param_name, param_value_size, param_value, &mut size)?;

    if param_value_size < size && !param_value.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    param_value_size_ret.write_checked(size);

    Ok(())
}

#[cl_entrypoint(clRetainEvent)]
fn retain_event(event: cl_event) -> CLResult<()> {
    event.retain()
}

#[cl_entrypoint(clReleaseEvent)]
fn release_event(event: cl_event) -> CLResult<()> {
    let arc_event = event.from_raw()?;
    if Arc::strong_count(&arc_event) == 1 {
        Vcl::get().call_clReleaseEvent(event)?;
    }

    Ok(())
}

#[cl_entrypoint(clEnqueueMarkerWithWaitList)]
fn enqueue_marker_with_wait_list(
    command_queue: cl_command_queue,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    command_queue.get_ref()?;

    if (event_wait_list.is_null() && num_events_in_wait_list > 0)
        || (!event_wait_list.is_null() && num_events_in_wait_list == 0)
    {
        return Err(CL_INVALID_EVENT_WAIT_LIST);
    }

    Vcl::get().call_clEnqueueMarkerWithWaitList(
        command_queue,
        num_events_in_wait_list,
        event_wait_list,
        event,
    )
}

#[cl_entrypoint(clEnqueueBarrierWithWaitList)]
fn enqueue_barrier_with_wait_list(
    command_queue: cl_command_queue,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    command_queue.get_ref()?;

    if (event_wait_list.is_null() && num_events_in_wait_list > 0)
        || (!event_wait_list.is_null() && num_events_in_wait_list == 0)
    {
        return Err(CL_INVALID_EVENT_WAIT_LIST);
    }

    Vcl::get().call_clEnqueueBarrierWithWaitList(
        command_queue,
        num_events_in_wait_list,
        event_wait_list,
        event,
    )
}

#[cl_entrypoint(clGetEventProfilingInfo)]
fn get_event_profiling_info(
    event: cl_event,
    param_name: cl_profiling_info,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    let mut size = 0;
    Vcl::get().call_clGetEventProfilingInfo(
        event,
        param_name,
        param_value_size,
        param_value,
        &mut size,
    )?;

    if param_value_size < size && !param_value.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    param_value_size_ret.write_checked(size);

    Ok(())
}

#[cl_entrypoint(clEnqueueWaitForEvents)]
fn enqueue_wait_for_events(
    command_queue: cl_command_queue,
    num_events: cl_uint,
    event_list: *const cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;

    if num_events == 0 || event_list.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    let events = Event::from_cl_arr(event_list, num_events).map_err(|_| CL_INVALID_VALUE)?;

    // CL_INVALID_CONTEXT if context associated with command_queue and events in event_list are not
    // the same.
    if events.iter().any(|event| event.context != queue.context) {
        return Err(CL_INVALID_CONTEXT);
    }

    Vcl::get().call_clEnqueueWaitForEvents(command_queue, num_events, event_list)?;

    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;

    use crate::api::queue::*;
    use crate::api::test_util::*;

    use std::ptr;

    fn setup_event() -> (cl_event, cl_context, cl_device_id, cl_platform_id) {
        let (context, device, platform) = setup_context();

        let event = create_user_event(context);
        assert!(event.is_ok());

        (event.unwrap(), context, device, platform)
    }

    #[test]
    fn test_create_user_event() {
        let (context, _, _) = setup_context();
        let ret = create_user_event(context);
        assert!(ret.is_ok());

        assert_eq!(create_user_event(ptr::null_mut()), Err(CL_INVALID_CONTEXT),);
    }

    #[test]
    fn test_set_user_event_status() {
        let (event, _, _, _) = setup_event();

        assert!(set_user_event_status(event, CL_COMPLETE as cl_int).is_ok());
    }

    #[test]
    #[ignore]
    fn test_wait_for_user_events() {
        let (event, _, _, _) = setup_event();

        assert!(wait_for_events(1, event as *const cl_event).is_ok());
    }

    #[test]
    fn test_get_event_info() {
        let (event, _, _, _) = setup_event();

        let param_value_size = std::mem::size_of::<usize>();
        assert!(get_event_info(
            event,
            CL_EVENT_COMMAND_EXECUTION_STATUS,
            param_value_size,
            ptr::null_mut(),
            ptr::null_mut()
        )
        .is_ok());
    }

    #[test]
    fn test_retain_event() {
        let (event, _, _, _) = setup_event();

        assert!(retain_event(event).is_ok());
        assert!(release_event(event).is_ok());
        assert!(release_event(event).is_ok());
    }

    #[test]
    fn test_release_event() {
        let (event, _, _, _) = setup_event();

        assert!(release_event(event).is_ok());
    }

    #[test]
    fn test_enqueue_marker_with_wait_list() {
        let (event, context, device, _) = setup_event();

        let ret = create_command_queue(context, device, 0);
        let queue = ret.unwrap();

        assert!(enqueue_marker_with_wait_list(queue, 1, &event, ptr::null_mut()).is_ok());
    }

    #[test]
    fn test_enqueue_barrier_with_wait_list() {
        let (event, context, device, _) = setup_event();

        let ret = create_command_queue(context, device, 0);
        let queue = ret.unwrap();

        assert!(enqueue_barrier_with_wait_list(queue, 1, &event, ptr::null_mut()).is_ok());
    }

    #[test]
    fn test_get_event_profiling_info() {
        let (event, _, _, _) = setup_event();

        let param_value_size = std::mem::size_of::<cl_long>();
        assert_eq!(
            get_event_profiling_info(
                event,
                CL_PROFILING_COMMAND_COMPLETE,
                param_value_size,
                ptr::null_mut(),
                ptr::null_mut()
            ),
            Err(CL_PROFILING_INFO_NOT_AVAILABLE)
        );
    }
}
