/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#![allow(non_snake_case)]

use crate::api::platform;
use crate::api::platform::*;

use vcl_opencl_gen::*;

use std::ffi::CStr;
use std::ptr;

pub static DISPATCH: cl_icd_dispatch = cl_icd_dispatch {
    clGetPlatformIDs: Some(clGetPlatformIDs),
    clGetPlatformInfo: Some(platform::clGetPlatformInfo),
    clGetDeviceIDs: None,
    clGetDeviceInfo: None,
    clCreateContext: None,
    clCreateContextFromType: None,
    clRetainContext: None,
    clReleaseContext: None,
    clGetContextInfo: None,
    clCreateCommandQueue: None,
    clRetainCommandQueue: None,
    clReleaseCommandQueue: None,
    clGetCommandQueueInfo: None,
    clSetCommandQueueProperty: None,
    clCreateBuffer: None,
    clCreateImage2D: None,
    clCreateImage3D: None,
    clRetainMemObject: None,
    clReleaseMemObject: None,
    clGetSupportedImageFormats: None,
    clGetMemObjectInfo: None,
    clGetImageInfo: None,
    clCreateSampler: None,
    clRetainSampler: None,
    clReleaseSampler: None,
    clGetSamplerInfo: None,
    clCreateProgramWithSource: None,
    clCreateProgramWithBinary: None,
    clRetainProgram: None,
    clReleaseProgram: None,
    clBuildProgram: None,
    clUnloadCompiler: None,
    clGetProgramInfo: None,
    clGetProgramBuildInfo: None,
    clCreateKernel: None,
    clCreateKernelsInProgram: None,
    clRetainKernel: None,
    clReleaseKernel: None,
    clSetKernelArg: None,
    clGetKernelInfo: None,
    clGetKernelWorkGroupInfo: None,
    clWaitForEvents: None,
    clGetEventInfo: None,
    clRetainEvent: None,
    clReleaseEvent: None,
    clGetEventProfilingInfo: None,
    clFlush: None,
    clFinish: None,
    clEnqueueReadBuffer: None,
    clEnqueueWriteBuffer: None,
    clEnqueueCopyBuffer: None,
    clEnqueueReadImage: None,
    clEnqueueWriteImage: None,
    clEnqueueCopyImage: None,
    clEnqueueCopyImageToBuffer: None,
    clEnqueueCopyBufferToImage: None,
    clEnqueueMapBuffer: None,
    clEnqueueMapImage: None,
    clEnqueueUnmapMemObject: None,
    clEnqueueNDRangeKernel: None,
    clEnqueueTask: None,
    clEnqueueNativeKernel: None,
    clEnqueueMarker: None,
    clEnqueueWaitForEvents: None,
    clEnqueueBarrier: None,
    clGetExtensionFunctionAddress: Some(clGetExtensionFunctionAddress),
    clCreateFromGLBuffer: None,
    clCreateFromGLTexture2D: None,
    clCreateFromGLTexture3D: None,
    clCreateFromGLRenderbuffer: None,
    clGetGLObjectInfo: None,
    clGetGLTextureInfo: None,
    clEnqueueAcquireGLObjects: None,
    clEnqueueReleaseGLObjects: None,
    clGetGLContextInfoKHR: None,
    clGetDeviceIDsFromD3D10KHR: ptr::null_mut(),
    clCreateFromD3D10BufferKHR: ptr::null_mut(),
    clCreateFromD3D10Texture2DKHR: ptr::null_mut(),
    clCreateFromD3D10Texture3DKHR: ptr::null_mut(),
    clEnqueueAcquireD3D10ObjectsKHR: ptr::null_mut(),
    clEnqueueReleaseD3D10ObjectsKHR: ptr::null_mut(),
    clSetEventCallback: ptr::null_mut(),
    clCreateSubBuffer: ptr::null_mut(),
    clSetMemObjectDestructorCallback: ptr::null_mut(),
    clCreateUserEvent: ptr::null_mut(),
    clSetUserEventStatus: ptr::null_mut(),
    clEnqueueReadBufferRect: ptr::null_mut(),
    clEnqueueWriteBufferRect: ptr::null_mut(),
    clEnqueueCopyBufferRect: ptr::null_mut(),
    clCreateSubDevicesEXT: None,
    clRetainDeviceEXT: None,
    clReleaseDeviceEXT: None,
    clCreateEventFromGLsyncKHR: None,
    clCreateSubDevices: ptr::null_mut(),
    clRetainDevice: ptr::null_mut(),
    clReleaseDevice: ptr::null_mut(),
    clCreateImage: ptr::null_mut(),
    clCreateProgramWithBuiltInKernels: ptr::null_mut(),
    clCompileProgram: ptr::null_mut(),
    clLinkProgram: ptr::null_mut(),
    clUnloadPlatformCompiler: ptr::null_mut(),
    clGetKernelArgInfo: ptr::null_mut(),
    clEnqueueFillBuffer: ptr::null_mut(),
    clEnqueueFillImage: ptr::null_mut(),
    clEnqueueMigrateMemObjects: ptr::null_mut(),
    clEnqueueMarkerWithWaitList: ptr::null_mut(),
    clEnqueueBarrierWithWaitList: ptr::null_mut(),
    clGetExtensionFunctionAddressForPlatform: ptr::null_mut(),
    clCreateFromGLTexture: None,
    clGetDeviceIDsFromD3D11KHR: ptr::null_mut(),
    clCreateFromD3D11BufferKHR: ptr::null_mut(),
    clCreateFromD3D11Texture2DKHR: ptr::null_mut(),
    clCreateFromD3D11Texture3DKHR: ptr::null_mut(),
    clCreateFromDX9MediaSurfaceKHR: ptr::null_mut(),
    clEnqueueAcquireD3D11ObjectsKHR: ptr::null_mut(),
    clEnqueueReleaseD3D11ObjectsKHR: ptr::null_mut(),
    clGetDeviceIDsFromDX9MediaAdapterKHR: ptr::null_mut(),
    clEnqueueAcquireDX9MediaSurfacesKHR: ptr::null_mut(),
    clEnqueueReleaseDX9MediaSurfacesKHR: ptr::null_mut(),
    clCreateFromEGLImageKHR: None,
    clEnqueueAcquireEGLObjectsKHR: None,
    clEnqueueReleaseEGLObjectsKHR: None,
    clCreateEventFromEGLSyncKHR: None,
    clCreateCommandQueueWithProperties: ptr::null_mut(),
    clCreatePipe: ptr::null_mut(),
    clGetPipeInfo: ptr::null_mut(),
    clSVMAlloc: ptr::null_mut(),
    clSVMFree: ptr::null_mut(),
    clEnqueueSVMFree: ptr::null_mut(),
    clEnqueueSVMMemcpy: ptr::null_mut(),
    clEnqueueSVMMemFill: ptr::null_mut(),
    clEnqueueSVMMap: ptr::null_mut(),
    clEnqueueSVMUnmap: ptr::null_mut(),
    clCreateSamplerWithProperties: ptr::null_mut(),
    clSetKernelArgSVMPointer: ptr::null_mut(),
    clSetKernelExecInfo: ptr::null_mut(),
    clGetKernelSubGroupInfoKHR: ptr::null_mut(),
    clCloneKernel: ptr::null_mut(),
    clCreateProgramWithIL: ptr::null_mut(),
    clEnqueueSVMMigrateMem: ptr::null_mut(),
    clGetDeviceAndHostTimer: ptr::null_mut(),
    clGetHostTimer: ptr::null_mut(),
    clGetKernelSubGroupInfo: ptr::null_mut(),
    clSetDefaultDeviceCommandQueue: ptr::null_mut(),
    clSetProgramReleaseCallback: ptr::null_mut(),
    clSetProgramSpecializationConstant: ptr::null_mut(),
    clCreateBufferWithProperties: ptr::null_mut(),
    clCreateImageWithProperties: ptr::null_mut(),
    clSetContextDestructorCallback: ptr::null_mut(),
};

pub type CLError = cl_int;
pub type CLResult<T> = Result<T, CLError>;

#[no_mangle]
extern "C" fn clIcdGetPlatformIDsKHR(
    num_entries: cl_uint,
    platforms: *mut cl_platform_id,
    num_platforms: *mut cl_uint,
) -> cl_int {
    platform::clGetPlatformIDs(num_entries, platforms, num_platforms)
}

macro_rules! cl_ext_func {
    ($func:ident: $api_type:ident) => {{
        let _func: $api_type = Some($func);
        $func as *mut ::std::ffi::c_void
    }};
}

#[rustfmt::skip]
#[no_mangle]
extern "C" fn clGetExtensionFunctionAddress(
    function_name: *const ::std::os::raw::c_char,
) -> *mut ::std::ffi::c_void {
    if function_name.is_null() {
        return ptr::null_mut();
    }
    match unsafe { CStr::from_ptr(function_name) }.to_str().unwrap() {
        // cl_khr_icd: https://registry.khronos.org/OpenCL/sdk/3.0/docs/man/html/cl_khr_icd.html
        "clIcdGetPlatformIDsKHR" => cl_ext_func!(clIcdGetPlatformIDsKHR: clIcdGetPlatformIDsKHR_fn),
        "clGetPlatformInfo" => cl_ext_func!(clGetPlatformInfo: cl_api_clGetPlatformInfo),
        _ => ptr::null_mut(),
    }
}

#[no_mangle]
extern "C" fn clGetPlatformInfo(
    platform: cl_platform_id,
    param_name: cl_platform_info,
    param_value_size: usize,
    param_value: *mut ::std::ffi::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    platform::clGetPlatformInfo(
        platform,
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    )
}
