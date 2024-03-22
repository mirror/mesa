/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::kernel::Kernel;
use crate::dev::renderer::Vcl;

use mesa_rust_util::ptr::CheckedPtr;
use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;

use std::mem::MaybeUninit;
use std::os::raw::c_char;
use std::os::raw::c_void;
use std::sync::Arc;

#[cl_entrypoint(clCreateKernel)]
fn create_kernel(program: cl_program, kernel_name: *const c_char) -> CLResult<cl_kernel> {
    let arc_program = program.get_arc()?;

    Ok(cl_kernel::from_arc(Kernel::new(&arc_program, kernel_name)?))
}

#[cl_entrypoint(clCreateKernelsInProgram)]
fn create_kernels_in_program(
    program: cl_program,
    num_kernels: cl_uint,
    kernels: *mut cl_kernel,
    num_kernels_ret: *mut cl_uint,
) -> CLResult<()> {
    program.get_ref()?;

    let mut num = 0;

    Vcl::get().call_clCreateKernelsInProgram(program, num_kernels, kernels, &mut num)?;

    num_kernels_ret.write_checked(num);

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
    kernel.get_ref()?;

    let mut size = 0;
    Vcl::get().call_clGetKernelWorkGroupInfo(
        kernel,
        device,
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
    kernel.get_ref()?;

    let mut size = 0;
    Vcl::get().call_clGetKernelSubGroupInfo(
        kernel,
        device,
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
