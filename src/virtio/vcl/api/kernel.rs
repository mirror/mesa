/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::types::NativeKernelCB;
use crate::api::util::*;
use crate::core::event::Event;
use crate::core::kernel::Kernel;
use crate::dev::renderer::Vcl;

use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;

use std::mem::MaybeUninit;
use std::os::raw::c_char;
use std::os::raw::c_void;
use std::sync::Arc;
use std::*;

#[cl_entrypoint(clCreateKernel)]
fn create_kernel(program: cl_program, kernel_name: *const c_char) -> CLResult<cl_kernel> {
    let arc_program = program.get_arc()?;
    Ok(cl_kernel::from_arc(Kernel::create(
        &arc_program,
        kernel_name,
    )?))
}

#[cl_entrypoint(clCreateKernelsInProgram)]
fn create_kernels_in_program(
    program: cl_program,
    num_kernels: cl_uint,
    kernels: *mut cl_kernel,
    num_kernels_ret: *mut cl_uint,
) -> CLResult<()> {
    let p = program.get_arc()?;

    // First, get the number of kernels that can be created
    let mut kernel_count = 0;
    Vcl::get().call_clCreateKernelsInProgram(program, 0, ptr::null_mut(), &mut kernel_count)?;

    if kernels != ptr::null_mut() {
        // CL_INVALID_VALUE if kernels is not NULL and num_kernels is less than the
        // number of kernels in program.
        if num_kernels < kernel_count {
            return Err(CL_INVALID_VALUE);
        }

        // Since we need to pass host handles to the guest for associating with vcomp kernel objects,
        // we need to create vcl objects here and store their handles in the kernels array.
        let kernels_slice = cl_kernel::get_slice_from_arr_mut(kernels, num_kernels as usize)?;
        for kernel_handle in kernels_slice {
            let host_kernel = Kernel::new(&p);
            *kernel_handle = cl_kernel::from_arc(host_kernel);
        }

        let ret = Vcl::get().call_clCreateKernelsInProgram(
            program,
            num_kernels,
            kernels,
            ptr::null_mut(),
        );

        if ret.is_err() {
            let kernels_slice = cl_kernel::get_slice_from_arr(kernels, num_kernels as usize)?;
            for kernel_handle in kernels_slice {
                // Free kernels in case of error
                kernel_handle.from_raw()?;
            }

            ret?;
        }
    }

    num_kernels_ret.write_checked(kernel_count);

    Ok(())
}

#[cl_entrypoint(clRetainKernel)]
fn retain_kernel(kernel: cl_kernel) -> CLResult<()> {
    kernel.retain()
}

#[cl_entrypoint(clReleaseKernel)]
fn release_kernel(kernel: cl_kernel) -> CLResult<()> {
    let arc_kernel = kernel.from_raw()?;
    if Arc::strong_count(&arc_kernel) == 1 {
        Vcl::get().call_clReleaseKernel(kernel)?;
    }

    Ok(())
}

#[cl_entrypoint(clSetKernelArg)]
fn set_kernel_arg(
    kernel: cl_kernel,
    arg_index: cl_uint,
    arg_size: usize,
    arg_value: *const c_void,
) -> CLResult<()> {
    kernel.get_ref()?;
    Vcl::get().call_clSetKernelArg(kernel, arg_index, arg_size, arg_value)
}

#[cl_entrypoint(clSetKernelArgSVMPointer)]
fn set_kernel_arg_svm_pointer(
    kernel: cl_kernel,
    arg_index: cl_uint,
    arg_value: *const c_void,
) -> CLResult<()> {
    kernel.get_ref()?;
    Vcl::get().call_clSetKernelArgSVMPointer(kernel, arg_index, arg_value)
}

#[cl_entrypoint(clSetKernelExecInfo)]
fn set_kernel_exec_info(
    kernel: cl_kernel,
    param_name: cl_kernel_exec_info,
    param_value_size: usize,
    param_value: *const c_void,
) -> CLResult<()> {
    kernel.get_ref()?;

    if param_name != CL_KERNEL_EXEC_INFO_SVM_PTRS
        || param_name != CL_KERNEL_EXEC_INFO_SVM_FINE_GRAIN_SYSTEM
        || param_value.is_null()
        || param_name == CL_KERNEL_EXEC_INFO_SVM_FINE_GRAIN_SYSTEM
            && param_value_size < std::mem::size_of::<cl_bool>()
    {
        return Err(CL_INVALID_VALUE);
    }

    Vcl::get().call_clSetKernelExecInfo(kernel, param_name, param_value_size, param_value)
}

#[cl_entrypoint(clCloneKernel)]
fn clone_kernel(source_kernel: cl_kernel) -> CLResult<cl_kernel> {
    let arc_source_kernel = source_kernel.get_arc()?;

    let Ok(kernel) = Kernel::clone(&arc_source_kernel) else {
        return Err(CL_OUT_OF_RESOURCES);
    };

    Ok(cl_kernel::from_arc(kernel))
}

impl CLInfo<cl_kernel_info> for cl_kernel {
    fn query(&self, info: cl_kernel_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        let kernel = self.get_ref()?;
        Ok(match info {
            CL_KERNEL_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&kernel.program.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_KERNEL_PROGRAM => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&kernel.program);
                cl_prop::<cl_program>(cl_program::from_ptr(ptr))
            }
            CL_KERNEL_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cl_entrypoint(clGetKernelInfo)]
fn get_kernel_info(
    kernel: cl_kernel,
    param_name: cl_kernel_info,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    kernel.get_ref()?;

    if param_name == CL_KERNEL_REFERENCE_COUNT
        || param_name == CL_KERNEL_CONTEXT
        || param_name == CL_KERNEL_PROGRAM
    {
        return kernel.get_info(
            param_name,
            param_value_size,
            param_value,
            param_value_size_ret,
        );
    }

    let mut size = 0;
    Vcl::get().call_clGetKernelInfo(
        kernel,
        param_name,
        param_value_size,
        param_value,
        &mut size,
    )?;

    param_value_size_ret.write_checked(size);

    Ok(())
}

#[cl_entrypoint(clGetKernelWorkGroupInfo)]
fn get_kernel_work_group_info(
    kernel: cl_kernel,
    device: cl_device_id,
    param_name: cl_kernel_work_group_info,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    let kern = kernel.get_ref()?;
    let dev = if device.is_null() {
        if kern.program.devs.len() > 1 {
            return Err(CL_INVALID_DEVICE);
        } else {
            kern.program.devs[0].get_handle()
        }
    } else {
        device.get_ref()?;
        device
    };

    let mut size = 0;
    Vcl::get().call_clGetKernelWorkGroupInfo(
        kernel,
        dev,
        param_name,
        param_value_size,
        param_value,
        &mut size,
    )?;

    param_value_size_ret.write_checked(size);

    Ok(())
}

#[cl_entrypoint(clGetKernelArgInfo)]
fn get_kernel_arg_info(
    kernel: cl_kernel,
    arg_index: cl_uint,
    param_name: cl_kernel_arg_info,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    kernel.get_ref()?;

    let mut size = 0;
    Vcl::get().call_clGetKernelArgInfo(
        kernel,
        arg_index,
        param_name,
        param_value_size,
        param_value,
        &mut size,
    )?;

    param_value_size_ret.write_checked(size);

    Ok(())
}

#[cl_entrypoint(clGetKernelSubGroupInfo)]
fn get_kernel_sub_group_info(
    kernel: cl_kernel,
    device: cl_device_id,
    param_name: cl_kernel_sub_group_info,
    input_value_size: usize,
    input_value: *const c_void,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    let kern = kernel.get_ref()?;
    let dev = if device.is_null() {
        if kern.program.devs.len() > 1 {
            return Err(CL_INVALID_DEVICE);
        } else {
            kern.program.devs[0].get_handle()
        }
    } else {
        device.get_ref()?;
        device
    };

    let mut size = 0;
    Vcl::get().call_clGetKernelSubGroupInfo(
        kernel,
        dev,
        param_name,
        input_value_size,
        input_value,
        param_value_size,
        param_value,
        &mut size,
    )?;

    param_value_size_ret.write_checked(size);

    Ok(())
}

const ZERO_ARR: [usize; 3] = [0; 3];

/// # Safety
///
/// This function is only safe when called on an array of `work_dim` length
unsafe fn kernel_work_arr_or_default<'a>(arr: *const usize, work_dim: cl_uint) -> &'a [usize] {
    if !arr.is_null() {
        unsafe { slice::from_raw_parts(arr, work_dim as usize) }
    } else {
        &ZERO_ARR
    }
}

#[cl_entrypoint(clEnqueueNDRangeKernel)]
fn enqueue_ndrange_kernel(
    command_queue: cl_command_queue,
    kernel: cl_kernel,
    work_dim: cl_uint,
    global_work_offset: *const usize,
    global_work_size: *const usize,
    local_work_size: *const usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let queue = command_queue.get_arc()?;
    let kernel = kernel.get_arc()?;

    // CL_INVALID_CONTEXT if context associated with command_queue and kernel are not the same
    if queue.context != kernel.program.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_PROGRAM_EXECUTABLE if there is no successfully built program executable available
    // for device associated with command_queue.
    if kernel.program.status(queue.device) != CL_BUILD_SUCCESS as cl_build_status {
        return Err(CL_INVALID_PROGRAM_EXECUTABLE);
    }

    // CL_INVALID_KERNEL_ARGS if the kernel argument values have not been specified.

    // CL_INVALID_WORK_DIMENSION if work_dim is not a valid value (i.e. a value between 1 and
    // CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS).

    let global_work_offset_dim = if global_work_offset.is_null() {
        0
    } else {
        work_dim
    };
    let global_work_size_dim = if global_work_size.is_null() {
        0
    } else {
        work_dim
    };
    let local_work_size_dim = if local_work_size.is_null() {
        0
    } else {
        work_dim
    };

    // we assume the application gets it right and doesn't pass shorter arrays then actually needed.
    let global_work_size = unsafe { kernel_work_arr_or_default(global_work_size, work_dim) };
    let local_work_size = unsafe { kernel_work_arr_or_default(local_work_size, work_dim) };
    let global_work_offset = unsafe { kernel_work_arr_or_default(global_work_offset, work_dim) };

    for i in 0..work_dim as usize {
        let lws = local_work_size[i];
        let gws = global_work_size[i];

        // CL_INVALID_WORK_ITEM_SIZE if the number of work-items specified in any of
        // local_work_size[0], … local_work_size[work_dim - 1] is greater than the corresponding
        // values specified by
        // CL_DEVICE_MAX_WORK_ITEM_SIZES[0], …, CL_DEVICE_MAX_WORK_ITEM_SIZES[work_dim - 1].

        // CL_INVALID_WORK_GROUP_SIZE if the work-group size must be uniform and the
        // local_work_size is not NULL, [...] if the global_work_size is not evenly divisible by
        // the local_work_size.
        if lws != 0 && gws % lws != 0 {
            return Err(CL_INVALID_WORK_GROUP_SIZE);
        }

        // CL_INVALID_WORK_GROUP_SIZE if local_work_size is specified and does not match the
        // required work-group size for kernel in the program source.

        // CL_INVALID_GLOBAL_WORK_SIZE if any of the values specified in global_work_size[0], …
        // global_work_size[work_dim - 1] exceed the maximum value representable by size_t on
        // the device on which the kernel-instance will be enqueued.

        // CL_INVALID_GLOBAL_OFFSET if the value specified in global_work_size + the
        // corresponding values in global_work_offset for any dimensions is greater than the
        // maximum value representable by size t on the device on which the kernel-instance
        // will be enqueued
    }

    // If global_work_size is NULL, or the value in any passed dimension is 0 then the kernel
    // command should trivially succeed after its event dependencies are satisfied and subsequently
    // update its completion event.

    let mut ev_handle = if !event.is_null() {
        cl_event::from_arc(Event::new(&queue.context))
    } else {
        ptr::null_mut()
    };

    let ev_ptr = if ev_handle.is_null() {
        ptr::null_mut()
    } else {
        &mut ev_handle
    };

    Vcl::get().call_clEnqueueNDRangeKernelMESA(
        queue.get_handle(),
        kernel.get_handle(),
        work_dim,
        global_work_offset_dim,
        global_work_offset.as_ptr(),
        global_work_size_dim,
        global_work_size.as_ptr(),
        local_work_size_dim,
        local_work_size.as_ptr(),
        num_events_in_wait_list,
        event_wait_list,
        ev_ptr,
    )?;

    event.write_checked(ev_handle);

    //• CL_INVALID_WORK_GROUP_SIZE if local_work_size is specified and is not consistent with the required number of sub-groups for kernel in the program source.
    //• CL_INVALID_WORK_GROUP_SIZE if local_work_size is specified and the total number of work-items in the work-group computed as local_work_size[0] × … local_work_size[work_dim - 1] is greater than the value specified by CL_KERNEL_WORK_GROUP_SIZE in the Kernel Object Device Queries table.
    //• CL_MISALIGNED_SUB_BUFFER_OFFSET if a sub-buffer object is specified as the value for an argument that is a buffer object and the offset specified when the sub-buffer object is created is not aligned to CL_DEVICE_MEM_BASE_ADDR_ALIGN value for device associated with queue. This error code
    //• CL_INVALID_IMAGE_SIZE if an image object is specified as an argument value and the image dimensions (image width, height, specified or compute row and/or slice pitch) are not supported by device associated with queue.
    //• CL_IMAGE_FORMAT_NOT_SUPPORTED if an image object is specified as an argument value and the image format (image channel order and data type) is not supported by device associated with queue.
    //• CL_OUT_OF_RESOURCES if there is a failure to queue the execution instance of kernel on the command-queue because of insufficient resources needed to execute the kernel. For example, the explicitly specified local_work_size causes a failure to execute the kernel because of insufficient resources such as registers or local memory. Another example would be the number of read-only image args used in kernel exceed the CL_DEVICE_MAX_READ_IMAGE_ARGS value for device or the number of write-only and read-write image args used in kernel exceed the CL_DEVICE_MAX_READ_WRITE_IMAGE_ARGS value for device or the number of samplers used in kernel exceed CL_DEVICE_MAX_SAMPLERS for device.
    //• CL_MEM_OBJECT_ALLOCATION_FAILURE if there is a failure to allocate memory for data store associated with image or buffer objects specified as arguments to kernel.
    //• CL_INVALID_OPERATION if SVM pointers are passed as arguments to a kernel and the device does not support SVM or if system pointers are passed as arguments to a kernel and/or stored inside SVM allocations passed as kernel arguments and the device does not support fine grain system SVM allocations.
    Ok(())
}

#[cl_entrypoint(clEnqueueTask)]
fn enqueue_task(
    command_queue: cl_command_queue,
    kernel: cl_kernel,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    // clEnqueueTask is equivalent to calling clEnqueueNDRangeKernel with work_dim set to 1,
    // global_work_offset set to NULL, global_work_size[0] set to 1, and local_work_size[0] set to
    // 1.
    enqueue_ndrange_kernel(
        command_queue,
        kernel,
        1,
        ptr::null(),
        [1, 1, 1].as_ptr(),
        [1, 0, 0].as_ptr(),
        num_events_in_wait_list,
        event_wait_list,
        event,
    )
}

#[cl_entrypoint(clEnqueueNativeKernel)]
fn enqueue_native_kernel(
    _command_queue: cl_command_queue,
    _user_func: Option<NativeKernelCB>,
    _args: *mut c_void,
    _cb_args: usize,
    _num_mem_objects: cl_uint,
    _mem_list: *const cl_mem,
    _args_mem_loc: *mut *const c_void,
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
) -> CLResult<()> {
    Err(CL_INVALID_OPERATION)
}

#[cfg(test)]
mod test {
    use super::*;

    use crate::api::test_util::*;

    use std::ffi::c_int;
    use std::ffi::CString;
    use std::ptr;

    fn setup_kernel() -> (cl_kernel, cl_context, cl_device_id, cl_platform_id) {
        let (program, context, device, platform) = setup_and_build_program();

        let kernel_name = CString::new(TEST_KERNEL_NAME).expect("Failed to create CString");

        let kernel = create_kernel(program, kernel_name.as_ptr());
        assert!(kernel.is_ok());

        (kernel.unwrap(), context, device, platform)
    }

    #[test]
    fn test_create_kernel() {
        setup_kernel();
    }

    #[ignore]
    #[test]
    fn test_create_kernels_in_program() {
        let (program, _, _, _) = setup_and_build_program();
        let arc_program = program.get_arc().expect("Failed to get Arc for program");

        let mut kernel = cl_kernel::from_arc(Arc::new(Kernel {
            base: CLObjectBase::new(),
            program: arc_program.clone(),
        }));

        let kernel_ptr: *mut cl_kernel = &mut kernel;

        let ret = create_kernels_in_program(program, 1, kernel_ptr, ptr::null_mut());
        assert!(ret.is_ok())
    }

    #[test]
    fn test_retain_kernel() {
        let (kernel, _, _, _) = setup_kernel();

        assert!(retain_kernel(kernel).is_ok());
        assert!(release_kernel(kernel).is_ok());
        assert!(release_kernel(kernel).is_ok());
    }

    #[test]
    fn test_release_kernel() {
        let (kernel, _, _, _) = setup_kernel();

        assert!(release_kernel(kernel).is_ok());
    }

    #[test]
    fn test_set_kernel_arg() {
        let (kernel, _, _, _) = setup_kernel();

        let arg = 42;
        let arg_ptr: *const c_void = &arg as *const _ as *const c_void;

        assert!(set_kernel_arg(kernel, 0, std::mem::size_of::<c_int>(), arg_ptr).is_ok());
    }

    #[test]
    fn test_get_kernel_info() {
        let (kernel, _, _, _) = setup_kernel();

        assert!(get_kernel_info(
            kernel,
            CL_KERNEL_NUM_ARGS,
            std::mem::size_of::<cl_uint>(),
            ptr::null_mut(),
            ptr::null_mut(),
        )
        .is_ok());
    }

    #[test]
    fn test_get_kernel_work_group_info() {
        let (kernel, _, device, _) = setup_kernel();

        assert!(get_kernel_work_group_info(
            kernel,
            device,
            CL_KERNEL_WORK_GROUP_SIZE,
            std::mem::size_of::<usize>(),
            ptr::null_mut(),
            ptr::null_mut(),
        )
        .is_ok());
    }

    #[test]
    fn test_get_kernel_arg_info() {
        let (kernel, _, _, _) = setup_kernel();

        assert!(get_kernel_arg_info(
            kernel,
            0,
            CL_KERNEL_ARG_ADDRESS_QUALIFIER,
            std::mem::size_of::<cl_kernel_arg_address_qualifier>(),
            ptr::null_mut(),
            ptr::null_mut(),
        )
        .is_ok());
    }

    #[ignore]
    #[test]
    fn test_get_kernel_sub_group_info() {
        let (kernel, _, device, _) = setup_kernel();

        let mut param_value = 0;
        let param_value_ptr = &mut param_value as *mut _ as *mut c_void;

        let mut param_value_size_ret = 0;

        assert!(get_kernel_sub_group_info(
            kernel,
            device,
            CL_KERNEL_MAX_NUM_SUB_GROUPS,
            std::mem::size_of::<usize>(),
            ptr::null_mut(),
            std::mem::size_of::<usize>(),
            param_value_ptr,
            &mut param_value_size_ret,
        )
        .is_ok());
    }
}
