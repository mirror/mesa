/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::*;
use crate::api::types::*;
use crate::api::util::*;
use crate::core::program::Program;
use vcl_opencl_gen::*;
use vcl_proc_macros::cl_entrypoint;

use crate::dev::renderer::Vcl;
use mesa_rust_util::ptr::CheckedPtr;
use std::collections::HashSet;
use std::ffi::*;
use std::sync::Arc;
use std::*;

#[cl_entrypoint(clCreateProgramWithSource)]
pub fn create_program_with_source(
    context: cl_context,
    count: cl_uint,
    strings: *mut *const c_char,
    lengths: *const usize,
) -> CLResult<cl_program> {
    if count == 0 {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE if count is zero or if strings ...
    if count == 0 || strings.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // ... or any entry in strings is NULL.
    let srcs = unsafe { slice::from_raw_parts(strings, count as usize) };
    if srcs.contains(&ptr::null()) {
        return Err(CL_INVALID_VALUE);
    }

    let c = context.get_arc()?;

    Ok(cl_program::from_arc(Program::new_with_source(
        c, count, strings, lengths,
    )?))
}

#[cl_entrypoint(clCreateProgramWithBinary)]
pub fn create_program_with_binary(
    context: cl_context,
    num_devices: cl_uint,
    device_list: *const cl_device_id,
    lengths: *const usize,
    binaries: *mut *const ::std::os::raw::c_uchar,
    binary_status: *mut cl_int,
) -> CLResult<cl_program> {
    if device_list.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    if num_devices == 0 {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE if lengths or binaries is NULL
    if lengths.is_null() || binaries.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    let c = context.get_arc()?;
    let devs = unsafe { slice::from_raw_parts(device_list, num_devices as usize) };

    // CL_INVALID_DEVICE if any device in device_list is not in the list of devices associated with
    // context.
    for d in devs.iter() {
        let d_ref = d.get_ref()?;
        if !c.devices.contains(&d_ref) {
            return Err(CL_INVALID_VALUE);
        }
    }

    let lengths = unsafe { slice::from_raw_parts(lengths, num_devices as usize) };
    let binaries = unsafe { slice::from_raw_parts(binaries, num_devices as usize) };

    // now device specific stuff
    let mut err = 0;
    let mut bins: Vec<&[u8]> = vec![&[]; num_devices as usize];
    for i in 0..num_devices as usize {
        let mut dev_err = 0;

        // CL_INVALID_VALUE if lengths[i] is zero or if binaries[i] is a NULL value
        if lengths[i] == 0 || binaries[i].is_null() {
            dev_err = CL_INVALID_VALUE;
        }

        if !binary_status.is_null() {
            unsafe { binary_status.add(i).write(dev_err) };
        }

        // just return the last one
        err = dev_err;

        bins[i] = unsafe { slice::from_raw_parts(binaries[i], lengths[i]) };
    }

    if err != 0 {
        return Err(err);
    }

    let set: HashSet<_> = HashSet::from_iter(devs.iter());
    let dev_results: Result<_, _> = set.into_iter().map(cl_device_id::get_ref).collect();
    let dev_obj_arr = dev_results?;

    Ok(cl_program::from_arc(Program::new_with_bins(
        c,
        dev_obj_arr,
        &bins,
    )?))
}

#[cl_entrypoint(clCreateProgramWithIL)]
pub fn create_program_with_il(
    context: cl_context,
    il: *const ::std::os::raw::c_void,
    length: usize,
) -> CLResult<cl_program> {
    let c = context.get_arc()?;

    if il.is_null() || length == 0 {
        return Err(CL_INVALID_VALUE);
    }

    Ok(cl_program::from_arc(Program::new_with_il(c, il, length)?))
}

#[cl_entrypoint(clReleaseProgram)]
pub fn release_program(program: cl_program) -> CLResult<()> {
    let arc_prog = program.from_raw()?;
    if Arc::strong_count(&arc_prog) == 1 {
        Vcl::get().call_clReleaseProgram(program)?;
    }
    Ok(())
}

#[cl_entrypoint(clRetainProgram)]
pub fn retain_program(program: cl_program) -> CLResult<()> {
    program.retain()
}

fn get_program_binaries(
    program: cl_program,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    let mut size = 0;

    // Get the size of the sizes array
    Vcl::get().call_clGetProgramInfo(
        program,
        CL_PROGRAM_BINARY_SIZES,
        0,
        ptr::null_mut(),
        &mut size,
    )?;

    let mut sizes = vec![0usize, size];
    // Get the sizes of each binary
    Vcl::get().call_clGetProgramInfo(
        program,
        CL_PROGRAM_BINARY_SIZES,
        sizes.len() * mem::size_of::<usize>(),
        sizes.as_mut_ptr().cast(),
        ptr::null_mut(),
    )?;

    // Let us make sure the array of arrays is contiguous memory by creating a new array
    // for all the binaries
    let mut binaries = vec![0u8; sizes.iter().sum()];
    // Fill the binaries array
    Vcl::get().call_clGetProgramInfo(
        program,
        CL_PROGRAM_BINARIES,
        binaries.len(),
        binaries.as_mut_ptr().cast(),
        param_value_size_ret,
    )?;

    // Copy the binaries back to the original argument
    let ptrs: &[*mut u8] = unsafe { slice::from_raw_parts(param_value.cast(), param_value_size) };
    let mut binary_ptr = binaries.as_ptr();
    for (i, size) in sizes.into_iter().enumerate() {
        unsafe { ptr::copy_nonoverlapping(binary_ptr, ptrs[i], size) };
        binary_ptr = unsafe { binary_ptr.add(size) };
    }

    Ok(())
}

#[cl_entrypoint(clGetProgramInfo)]
pub fn get_program_info(
    program: cl_program,
    param_name: cl_program_info,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    program.get_ref()?;

    let mut size = 0;

    // This needs special handling, as the param_value is an array of arrays
    if param_name == CL_PROGRAM_BINARIES && !param_value.is_null() {
        get_program_binaries(program, param_value_size, param_value, &mut size)?;
    } else {
        Vcl::get().call_clGetProgramInfo(
            program,
            param_name,
            param_value_size,
            param_value,
            &mut size,
        )?;
    }

    if param_value_size < size && !param_value.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    param_value_size_ret.write_checked(size);

    Ok(())
}

#[cl_entrypoint(clGetProgramBuildInfo)]
pub fn get_program_build_info(
    program: cl_program,
    device: cl_device_id,
    param_name: cl_program_build_info,
    param_value_size: usize,
    param_value: *mut c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    let p = program.get_ref()?;
    let c = &p.context;
    if !c.devices.contains(&device.get_ref()?) {
        return Err(CL_INVALID_DEVICE);
    }

    let mut size = 0;
    Vcl::get().call_clGetProgramBuildInfo(
        program,
        device,
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

#[cl_entrypoint(clBuildProgram)]
pub fn build_program(
    program: cl_program,
    num_devices: cl_uint,
    device_list: *const cl_device_id,
    options: *const c_char,
    pfn_notify: Option<ProgramCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> CLResult<()> {
    let p = program.get_ref()?;
    let c = &p.context;

    if device_list.is_null() && num_devices > 0 {
        return Err(CL_INVALID_VALUE);
    } else if !device_list.is_null() && num_devices == 0 {
        return Err(CL_INVALID_VALUE);
    }

    check_cb(&pfn_notify, user_data)?;

    let devs = unsafe { slice::from_raw_parts(device_list, num_devices as usize) };
    for d in devs.iter() {
        let d_ref = d.get_ref()?;
        if !c.devices.contains(&d_ref) {
            return Err(CL_INVALID_VALUE);
        }
    }

    Vcl::get().call_clBuildProgram(
        p.get_handle(),
        devs.len() as u32,
        devs.as_ptr(),
        options,
        ptr::null_mut(),
    )?;

    call_cb(pfn_notify, program, user_data);

    Ok(())
}

#[cl_entrypoint(clCompileProgram)]
pub fn compile_program(
    program: cl_program,
    num_devices: cl_uint,
    device_list: *const cl_device_id,
    options: *const c_char,
    num_input_headers: cl_uint,
    input_headers: *const cl_program,
    header_include_names: *mut *const c_char,
    pfn_notify: Option<ProgramCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> CLResult<()> {
    let p = program.get_ref()?;
    let c = &p.context;

    check_cb(&pfn_notify, user_data)?;

    if device_list.is_null() && num_devices > 0 {
        return Err(CL_INVALID_VALUE);
    } else if !device_list.is_null() && num_devices == 0 {
        return Err(CL_INVALID_VALUE);
    }

    let devs = unsafe { slice::from_raw_parts(device_list, num_devices as usize) };
    for d in devs.iter() {
        let d_ref = d.get_ref()?;
        if !c.devices.contains(&d_ref) {
            return Err(CL_INVALID_VALUE);
        }
    }

    if num_input_headers == 0 && (!header_include_names.is_null() || !input_headers.is_null())
        || num_input_headers != 0 && (header_include_names.is_null() || input_headers.is_null())
    {
        return Err(CL_INVALID_VALUE);
    }

    Vcl::get().call_clCompileProgram(
        program,
        num_devices,
        device_list,
        options,
        num_input_headers,
        input_headers,
        header_include_names,
        ptr::null_mut(),
    )?;

    call_cb(pfn_notify, program, user_data);

    Ok(())
}

#[cl_entrypoint(clLinkProgram)]
pub fn link_program(
    context: cl_context,
    num_devices: cl_uint,
    device_list: *const cl_device_id,
    options: *const ::std::os::raw::c_char,
    num_input_programs: cl_uint,
    input_programs: *const cl_program,
    pfn_notify: Option<ProgramCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> CLResult<cl_program> {
    let c = context.get_arc()?;

    check_cb(&pfn_notify, user_data)?;

    if device_list.is_null() && num_devices > 0 {
        return Err(CL_INVALID_VALUE);
    } else if !device_list.is_null() && num_devices == 0 {
        return Err(CL_INVALID_VALUE);
    }

    if input_programs.is_null() && num_input_programs > 0 {
        return Err(CL_INVALID_VALUE);
    } else if !input_programs.is_null() && num_input_programs == 0 {
        return Err(CL_INVALID_VALUE);
    }

    let program = cl_program::from_arc(Program::link(
        c,
        num_devices,
        device_list,
        options,
        num_input_programs,
        input_programs,
    )?);

    call_cb(pfn_notify, program, user_data);

    Ok(program)
}

#[cfg(test)]
mod test {
    use super::*;

    use crate::api::test_util::*;

    use std::ffi::CString;
    use std::ptr;

    const SOURCE_STRING: &str = "__kernel void nop() { }";

    fn setup_program() -> (cl_program, cl_context, cl_device_id, cl_platform_id) {
        let (context, device, platform) = setup_context();

        let source_strings = CString::new(SOURCE_STRING).expect("Failed to create CString");
        let mut strings = [source_strings.as_c_str().as_ptr()];
        let program = create_program_with_source(
            context,
            strings.len() as _,
            strings.as_mut_ptr(),
            ptr::null(),
        );

        assert!(program.is_ok());
        (program.unwrap(), context, device, platform)
    }

    #[test]
    fn test_create_program_with_source() {
        let (context, _, _) = setup_context();

        let source_strings = CString::new(SOURCE_STRING).expect("Failed to create CString");
        let mut strings = [source_strings.as_c_str().as_ptr()];
        let program = create_program_with_source(
            context,
            strings.len() as _,
            strings.as_mut_ptr(),
            ptr::null(),
        );

        assert!(program.is_ok());
    }

    #[test]
    fn test_retain_program() {
        let (program, _, _, _) = setup_program();

        assert!(retain_program(program).is_ok());
        assert!(release_program(program).is_ok());
        assert!(release_program(program).is_ok());
    }

    #[test]
    fn test_release_program() {
        let (program, _, _, _) = setup_program();

        assert!(release_program(program).is_ok());
    }

    #[test]
    fn test_get_program_info() {
        let (program, _, _, _) = setup_program();

        let mut param_value = 0;
        let param_value_ptr: *mut c_void = &mut param_value as *mut _ as *mut c_void;
        let mut param_value_size_ret = 0;

        let ret = get_program_info(
            program,
            CL_PROGRAM_NUM_DEVICES,
            std::mem::size_of::<cl_uint>(),
            param_value_ptr,
            &mut param_value_size_ret,
        );

        assert!(ret.is_ok());
        assert_eq!(param_value, 1);
        assert_eq!(param_value_size_ret, std::mem::size_of::<cl_uint>());
    }

    #[test]
    fn test_get_program_build_info() {
        let (program, _, device, _) = setup_program();

        let mut param_value = CL_BUILD_ERROR;
        let param_value_ptr: *mut c_void = &mut param_value as *mut _ as *mut c_void;
        let mut param_value_size_ret = 0;

        let ret = get_program_build_info(
            program,
            device,
            CL_PROGRAM_BUILD_STATUS,
            std::mem::size_of::<cl_build_status>(),
            param_value_ptr,
            &mut param_value_size_ret,
        );

        assert!(ret.is_ok());
        assert_eq!(param_value, CL_BUILD_NONE);
        assert_eq!(param_value_size_ret, std::mem::size_of::<cl_build_status>());
    }

    #[test]
    fn test_build_program() {
        let (program, _, device, _) = setup_program();
        let devices = [device];

        let ret = build_program(
            program,
            devices.len() as u32,
            devices.as_ptr(),
            ptr::null_mut(),
            None,
            ptr::null_mut(),
        );

        assert!(ret.is_ok());
    }

    #[test]
    fn test_compile_program() {
        let (program, _, device, _) = setup_program();
        let devices = [device];

        let ret = compile_program(
            program,
            devices.len() as u32,
            devices.as_ptr(),
            ptr::null_mut(),
            0,
            ptr::null_mut(),
            ptr::null_mut(),
            None,
            ptr::null_mut(),
        );

        assert!(ret.is_ok());
    }

    #[ignore]
    #[test]
    fn test_link_program() {
        let (program, context, device, _) = setup_program();
        let devices = [device];
        let programs = [program];

        let ret = compile_program(
            program,
            devices.len() as u32,
            devices.as_ptr(),
            ptr::null_mut(),
            0,
            ptr::null_mut(),
            ptr::null_mut(),
            None,
            ptr::null_mut(),
        );

        assert!(ret.is_ok());

        let new_program = link_program(
            context,
            devices.len() as u32,
            devices.as_ptr(),
            ptr::null_mut(),
            programs.len() as u32,
            programs.as_ptr(),
            None,
            ptr::null_mut(),
        );

        assert!(new_program.is_ok());
    }
}
