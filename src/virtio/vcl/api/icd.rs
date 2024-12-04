/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#![allow(non_snake_case)]

use crate::api::context::*;
use crate::api::device::*;
use crate::api::event::*;
use crate::api::kernel::*;
use crate::api::memory::*;
use crate::api::platform;
use crate::api::platform::*;
use crate::api::program::*;
use crate::api::queue::*;

use vcl_opencl_gen::*;

use std::ffi::CStr;
use std::ptr;
use std::sync::Arc;

pub static DISPATCH: cl_icd_dispatch = cl_icd_dispatch {
    clGetPlatformIDs: Some(clGetPlatformIDs),
    clGetPlatformInfo: Some(platform::clGetPlatformInfo),
    clGetDeviceIDs: Some(clGetDeviceIDs),
    clGetDeviceInfo: Some(clGetDeviceInfo),
    clCreateContext: Some(clCreateContext),
    clCreateContextFromType: Some(clCreateContextFromType),
    clRetainContext: Some(clRetainContext),
    clReleaseContext: Some(clReleaseContext),
    clGetContextInfo: Some(clGetContextInfo),
    clCreateCommandQueue: Some(clCreateCommandQueue),
    clRetainCommandQueue: Some(clRetainCommandQueue),
    clReleaseCommandQueue: Some(clReleaseCommandQueue),
    clGetCommandQueueInfo: Some(clGetCommandQueueInfo),
    clSetCommandQueueProperty: Some(clSetCommandQueueProperty),
    clCreateBuffer: Some(clCreateBuffer),
    clCreateImage2D: Some(clCreateImage2D),
    clCreateImage3D: Some(clCreateImage3D),
    clRetainMemObject: Some(clRetainMemObject),
    clReleaseMemObject: Some(clReleaseMemObject),
    clGetSupportedImageFormats: Some(clGetSupportedImageFormats),
    clGetMemObjectInfo: Some(clGetMemObjectInfo),
    clGetImageInfo: Some(clGetImageInfo),
    clCreateSampler: Some(clCreateSampler),
    clRetainSampler: Some(clRetainSampler),
    clReleaseSampler: Some(clReleaseSampler),
    clGetSamplerInfo: Some(clGetSamplerInfo),
    clCreateProgramWithSource: Some(clCreateProgramWithSource),
    clCreateProgramWithBinary: Some(clCreateProgramWithBinary),
    clRetainProgram: Some(clRetainProgram),
    clReleaseProgram: Some(clReleaseProgram),
    clBuildProgram: Some(clBuildProgram),
    clUnloadCompiler: Some(clUnloadCompiler),
    clGetProgramInfo: Some(clGetProgramInfo),
    clGetProgramBuildInfo: Some(clGetProgramBuildInfo),
    clCreateKernel: Some(clCreateKernel),
    clCreateKernelsInProgram: Some(clCreateKernelsInProgram),
    clRetainKernel: Some(clRetainKernel),
    clReleaseKernel: Some(clReleaseKernel),
    clSetKernelArg: Some(clSetKernelArg),
    clGetKernelInfo: Some(clGetKernelInfo),
    clGetKernelWorkGroupInfo: Some(clGetKernelWorkGroupInfo),
    clWaitForEvents: Some(clWaitForEvents),
    clGetEventInfo: Some(clGetEventInfo),
    clRetainEvent: Some(clRetainEvent),
    clReleaseEvent: Some(clReleaseEvent),
    clGetEventProfilingInfo: Some(clGetEventProfilingInfo),
    clFlush: Some(clFlush),
    clFinish: Some(clFinish),
    clEnqueueReadBuffer: Some(clEnqueueReadBuffer),
    clEnqueueWriteBuffer: Some(clEnqueueWriteBuffer),
    clEnqueueCopyBuffer: Some(clEnqueueCopyBuffer),
    clEnqueueReadImage: Some(clEnqueueReadImage),
    clEnqueueWriteImage: Some(clEnqueueWriteImage),
    clEnqueueCopyImage: Some(clEnqueueCopyImage),
    clEnqueueCopyImageToBuffer: Some(clEnqueueCopyImageToBuffer),
    clEnqueueCopyBufferToImage: Some(clEnqueueCopyBufferToImage),
    clEnqueueMapBuffer: Some(clEnqueueMapBuffer),
    clEnqueueMapImage: Some(clEnqueueMapImage),
    clEnqueueUnmapMemObject: Some(clEnqueueUnmapMemObject),
    clEnqueueNDRangeKernel: Some(clEnqueueNDRangeKernel),
    clEnqueueTask: Some(clEnqueueTask),
    clEnqueueNativeKernel: Some(clEnqueueNativeKernel),
    clEnqueueMarker: Some(clEnqueueMarker),
    clEnqueueWaitForEvents: Some(clEnqueueWaitForEvents),
    clEnqueueBarrier: Some(clEnqueueBarrier),
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
    clSetEventCallback: None,
    clCreateSubBuffer: Some(clCreateSubBuffer),
    clSetMemObjectDestructorCallback: Some(clSetMemObjectDestructorCallback),
    clCreateUserEvent: Some(clCreateUserEvent),
    clSetUserEventStatus: Some(clSetUserEventStatus),
    clEnqueueReadBufferRect: None,
    clEnqueueWriteBufferRect: None,
    clEnqueueCopyBufferRect: Some(clEnqueueCopyBufferRect),
    clCreateSubDevicesEXT: None,
    clRetainDeviceEXT: None,
    clReleaseDeviceEXT: None,
    clCreateEventFromGLsyncKHR: None,
    clCreateSubDevices: None,
    clRetainDevice: Some(clRetainDevice),
    clReleaseDevice: Some(clReleaseDevice),
    clCreateImage: Some(clCreateImage),
    clCreateProgramWithBuiltInKernels: None,
    clCompileProgram: Some(clCompileProgram),
    clLinkProgram: Some(clLinkProgram),
    clUnloadPlatformCompiler: None,
    clGetKernelArgInfo: Some(clGetKernelArgInfo),
    clEnqueueFillBuffer: Some(clEnqueueFillBuffer),
    clEnqueueFillImage: Some(clEnqueueFillImage),
    clEnqueueMigrateMemObjects: Some(clEnqueueMigrateMemObjects),
    clEnqueueMarkerWithWaitList: Some(clEnqueueMarkerWithWaitList),
    clEnqueueBarrierWithWaitList: Some(clEnqueueBarrierWithWaitList),
    clGetExtensionFunctionAddressForPlatform: Some(clGetExtensionFunctionAddressForPlatform),
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
    clCreateCommandQueueWithProperties: Some(clCreateCommandQueueWithProperties),
    clCreatePipe: Some(clCreatePipe),
    clGetPipeInfo: Some(clGetPipeInfo),
    clSVMAlloc: None,
    clSVMFree: None,
    clEnqueueSVMFree: None,
    clEnqueueSVMMemcpy: None,
    clEnqueueSVMMemFill: None,
    clEnqueueSVMMap: None,
    clEnqueueSVMUnmap: None,
    clCreateSamplerWithProperties: Some(clCreateSamplerWithProperties),
    clSetKernelArgSVMPointer: Some(clSetKernelArgSVMPointer),
    clSetKernelExecInfo: Some(clSetKernelExecInfo),
    clGetKernelSubGroupInfoKHR: None,
    clCloneKernel: Some(clCloneKernel),
    clCreateProgramWithIL: Some(clCreateProgramWithIL),
    clEnqueueSVMMigrateMem: None,
    clGetDeviceAndHostTimer: None,
    clGetHostTimer: None,
    clGetKernelSubGroupInfo: Some(clGetKernelSubGroupInfo),
    clSetDefaultDeviceCommandQueue: Some(clSetDefaultDeviceCommandQueue),
    clSetProgramReleaseCallback: None,
    clSetProgramSpecializationConstant: None,
    clCreateBufferWithProperties: None,
    clCreateImageWithProperties: Some(clCreateImageWithProperties),
    clSetContextDestructorCallback: Some(clSetContextDestructorCallback),
};

pub type CLError = cl_int;
pub type CLResult<T> = Result<T, CLError>;

#[repr(C)]
pub struct CLObjectBase<const ERR: i32> {
    dispatch: &'static cl_icd_dispatch,
    type_err: i32,
}

impl<const ERR: i32> Default for CLObjectBase<ERR> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const ERR: i32> CLObjectBase<ERR> {
    pub fn new() -> Self {
        Self {
            dispatch: &DISPATCH,
            type_err: ERR,
        }
    }

    pub fn check_ptr(ptr: *const Self) -> CLResult<()> {
        if ptr.is_null() {
            return Err(ERR);
        }

        unsafe {
            if !::std::ptr::eq((*ptr).dispatch, &DISPATCH) {
                return Err(ERR);
            }

            if (*ptr).type_err != ERR {
                return Err(ERR);
            }

            Ok(())
        }
    }
}

pub trait ReferenceCountedAPIPointer<T, const ERR: i32> {
    fn get_ptr(&self) -> CLResult<*const T>;

    // TODO:  I can't find a trait that would let me say T: pointer so that
    // I can do the cast in the main trait implementation.  So we need to
    // implement that as part of the macro where we know the real type.
    fn from_ptr(ptr: *const T) -> Self;

    fn get_ref(&self) -> CLResult<&T> {
        unsafe { Ok(self.get_ptr()?.as_ref().unwrap()) }
    }

    fn get_arc(&self) -> CLResult<std::sync::Arc<T>> {
        unsafe {
            let ptr = self.get_ptr()?;
            std::sync::Arc::increment_strong_count(ptr);
            Ok(std::sync::Arc::from_raw(ptr))
        }
    }

    fn from_arc(arc: std::sync::Arc<T>) -> Self
    where
        Self: Sized,
    {
        Self::from_ptr(std::sync::Arc::into_raw(arc))
    }

    fn get_arc_vec_from_arr(objs: *const Self, count: u32) -> CLResult<Vec<Arc<T>>>
    where
        Self: Sized,
    {
        // CL spec requires validation for obj arrays, both values have to make sense
        if objs.is_null() && count > 0 || !objs.is_null() && count == 0 {
            return Err(CL_INVALID_VALUE);
        }

        let mut res = Vec::new();
        if objs.is_null() || count == 0 {
            return Ok(res);
        }

        for i in 0..count as usize {
            unsafe {
                res.push((*objs.add(i)).get_arc()?);
            }
        }
        Ok(res)
    }

    fn get_ref_vec_from_arr<'a>(objs: *const Self, count: u32) -> CLResult<Vec<&'a T>>
    where
        Self: Sized + 'a,
    {
        // CL spec requires validation for obj arrays, both values have to make sense
        if objs.is_null() && count > 0 || !objs.is_null() && count == 0 {
            return Err(CL_INVALID_VALUE);
        }

        let mut res = Vec::new();
        if objs.is_null() || count == 0 {
            return Ok(res);
        }

        for i in 0..count as usize {
            unsafe {
                res.push((*objs.add(i)).get_ref()?);
            }
        }
        Ok(res)
    }

    fn retain(&self) -> CLResult<()> {
        unsafe {
            std::sync::Arc::increment_strong_count(self.get_ptr()?);
            Ok(())
        }
    }

    fn from_raw(&self) -> CLResult<std::sync::Arc<T>> {
        Ok(unsafe { std::sync::Arc::from_raw(self.get_ptr()?) })
    }

    fn refcnt(&self) -> CLResult<u32> {
        Ok((std::sync::Arc::strong_count(&self.get_arc()?) - 1) as u32)
    }

    fn check_array(objs: *const Self, len: usize) -> CLResult<()>
    where
        Self: Sized,
    {
        if objs.is_null() || len == 0 {
            return Err(ERR);
        }

        let is_aligned = objs.align_offset(core::mem::align_of::<Self>()) == 0;
        if !is_aligned {
            return Err(ERR);
        }

        Ok(())
    }

    /// Helper method which is going to do some checks before getting a slice, and in
    /// case of error can return a nice error instead of panicking. It returns a slice
    /// with at least one element.
    fn get_slice_from_arr<'a>(objs: *const Self, len: usize) -> CLResult<&'a [Self]>
    where
        Self: Sized,
    {
        Self::check_array(objs, len)?;
        Ok(unsafe { std::slice::from_raw_parts(objs, len) })
    }

    fn get_slice_from_arr_mut<'a>(objs: *mut Self, len: usize) -> CLResult<&'a mut [Self]>
    where
        Self: Sized,
    {
        Self::check_array(objs, len)?;
        Ok(unsafe { std::slice::from_raw_parts_mut(objs, len) })
    }
}

#[macro_export]
macro_rules! impl_cl_type_trait {
    ($cl: ident, $t: path, $err: ident) => {
        impl $crate::api::icd::ReferenceCountedAPIPointer<$t, $err> for $cl {
            fn get_ptr(&self) -> $crate::api::icd::CLResult<*const $t> {
                type Base = $crate::api::icd::CLObjectBase<$err>;
                Base::check_ptr(self.cast())?;

                let offset = ::mesa_rust_util::offset_of!($t, base);
                let mut obj_ptr: *const u8 = self.cast();
                // SAFETY: We offset the pointer back from the ICD specified base type to our
                //         internal type.
                unsafe { obj_ptr = obj_ptr.sub(offset) }
                Ok(obj_ptr.cast())
            }

            fn from_ptr(ptr: *const $t) -> Self {
                if ptr.is_null() {
                    return std::ptr::null_mut();
                }
                let offset = ::mesa_rust_util::offset_of!($t, base);
                // SAFETY: The resulting pointer is safe as we simply offset into the ICD specified
                //         base type.
                unsafe { (ptr as *const u8).add(offset) as Self }
            }
        }

        impl $t {
            pub fn get_handle(&self) -> $cl {
                use crate::api::icd::ReferenceCountedAPIPointer;
                $cl::from_ptr(self)
            }
        }

        // there are two reason to implement those traits for all objects
        //   1. it speeds up operations
        //   2. we want to check for real equality more explicit to stay conformant with the API
        //      and to not break in subtle ways e.g. using CL objects as keys in HashMaps.
        impl std::cmp::Eq for $t {}
        impl std::cmp::PartialEq for $t {
            fn eq(&self, other: &Self) -> bool {
                (self as *const Self) == (other as *const Self)
            }
        }

        impl std::hash::Hash for $t {
            fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
                (self as *const Self).hash(state);
            }
        }
    };
}

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
pub extern "C" fn clGetExtensionFunctionAddress(
    function_name: *const ::std::os::raw::c_char,
) -> *mut ::std::ffi::c_void {
    if function_name.is_null() {
        return ptr::null_mut();
    }
    match unsafe { CStr::from_ptr(function_name) }.to_str().unwrap() {
        // cl_khr_icd: https://registry.khronos.org/OpenCL/sdk/3.0/docs/man/html/cl_khr_icd.html
        "clIcdGetPlatformIDsKHR" => cl_ext_func!(clIcdGetPlatformIDsKHR: clIcdGetPlatformIDsKHR_fn),
        "clGetPlatformInfo" => cl_ext_func!(clGetPlatformInfo: cl_api_clGetPlatformInfo),
        // cl_khr_il_program
        "clCreateProgramWithILKHR" => cl_ext_func!(clCreateProgramWithIL: clCreateProgramWithILKHR_fn),
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

pub extern "C" fn clUnloadCompiler() -> cl_int {
    CL_SUCCESS as _
}
