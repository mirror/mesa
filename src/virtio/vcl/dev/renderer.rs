/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::CLResult;
use crate::core::platform::Platform;
use crate::dev::debug::*;
use crate::dev::drm::DrmDevice;
use crate::dev::virtgpu::*;
use crate::dev::vtest::*;
use crate::protocol::VclCsEncoder;

use vcl_opencl_gen::cl_platform_id;
use vcl_virglrenderer_gen::*;

use std::env;
use std::ffi::c_void;
use std::ffi::CStr;
use std::mem;
use std::pin::Pin;
use std::slice;
use std::str::FromStr;
use std::sync::Once;

pub trait VclRenderer {
    fn submit(&self, submit: VclCsEncoder) -> CLResult<()>;
    fn create_reply_buffer(&self, size: usize) -> CLResult<VclReplyBuffer>;
    fn transfer_get(&self, resource: &mut dyn VclResource) -> CLResult<()>;
}

static VCL_ENV_ONCE: Once = Once::new();
static VCL_ONCE: Once = Once::new();

static mut VCL_DEBUG: VclDebug = VclDebug {
    flags: VclDebugFlags::Empty,
};
static mut VCL: Option<Vcl> = None;

fn load_env() {
    // We can not use log!() yet as it requires VCL_ENV_ONCE to be completed
    let debug = unsafe { &mut VCL_DEBUG };

    if let Ok(debug_flags) = env::var("VCL_DEBUG") {
        for flag in debug_flags.split(',') {
            match VclDebugFlags::from_str(flag) {
                Ok(debug_flag) => debug.flags |= debug_flag,
                Err(e) => eprintln!("vcl: error: VCL_DEBUG: {}", e),
            }
        }
        if debug.flags.contains(VclDebugFlags::Info) {
            eprintln!("vcl: info: VCL_DEBUG enabled: {}", debug.flags);
        }
    }
}

pub struct Vcl {
    pub renderer: Box<dyn VclRenderer>,
    pub platforms: Vec<Pin<Box<Platform>>>,
}

impl Vcl {
    pub fn init_once() -> CLResult<()> {
        VCL_ENV_ONCE.call_once(load_env);
        // SAFETY: no concurrent static mut access due to std::Once
        let mut ret = Ok(());

        VCL_ONCE.call_once(|| {
            let renderer = if Vcl::debug().flags.contains(VclDebugFlags::Vtest) {
                Self::new_with_vtest()
            } else {
                Self::new_with_virtgpu()
            };

            match renderer {
                Err(e) => {
                    ret = Err(e);
                }
                Ok(vcl) => unsafe {
                    VCL.replace(vcl);
                },
            }
        });
        ret
    }

    pub fn debug() -> &'static VclDebug {
        debug_assert!(VCL_ENV_ONCE.is_completed());
        unsafe { &VCL_DEBUG }
    }

    pub fn get_platforms(&self) -> &[Pin<Box<Platform>>] {
        &self.platforms
    }

    pub fn contains_platform(&self, id: cl_platform_id) -> bool {
        for platform in &self.platforms {
            if platform.get_handle() == id {
                return true;
            }
        }
        false
    }

    pub fn new_with_virtgpu() -> CLResult<Self> {
        let drm_devices = DrmDevice::virtgpus()?;
        assert!(!drm_devices.is_empty(), "Failed to find VirtIO-GPUs");

        let first_drm_device = drm_devices.into_iter().nth(0).unwrap();
        let virtgpu = VirtGpu::new(first_drm_device)?;

        let mut ret = Self {
            renderer: Box::new(virtgpu),
            platforms: Vec::new(),
        };
        ret.platforms = Platform::all(&ret)?;
        Ok(ret)
    }

    pub fn new_with_vtest() -> CLResult<Self> {
        let mut ret = Self {
            renderer: Box::new(Vtest::new().expect("Failed to create vtest")),
            platforms: Vec::new(),
        };
        ret.platforms = Platform::all(&ret)?;
        Ok(ret)
    }

    pub fn get() -> &'static Self {
        debug_assert!(VCL_ONCE.is_completed());
        unsafe { &VCL.as_ref().unwrap() }
    }
}

#[repr(C)]
#[derive(Clone)]
pub struct VirglRendererCapset {
    pub id: virgl_renderer_capset,
    pub version: u32,
    pub data: VirglRendererCapsetVcl,
}

impl Default for VirglRendererCapset {
    fn default() -> Self {
        Self {
            id: virgl_renderer_capset_VIRGL_RENDERER_CAPSET_VCL,
            version: 0,
            data: VirglRendererCapsetVcl::default(),
        }
    }
}

impl VirglRendererCapset {
    pub fn get_host_platform_name(&self) -> &CStr {
        CStr::from_bytes_until_nul(&self.data.platform_name)
            .expect("Failed to create CStr for host platform name")
    }
}

#[repr(C)]
#[derive(Clone, Default)]
pub struct VirglRendererCapsetVcl {
    pub platform_name: [u8; 32],
}

impl VirglRendererCapsetVcl {
    pub fn as_mut_ptr(&mut self) -> *mut Self {
        self as _
    }
}

pub trait VclResource {
    fn len(&self) -> usize;

    fn get_ptr(&self) -> *const c_void;

    fn get_handle(&self) -> i32;

    fn get_bo_handle(&self) -> u32;

    fn get_slice(&self) -> &[u8] {
        unsafe { slice::from_raw_parts(self.get_ptr() as _, self.len()) }
    }

    fn map(&mut self, offset: usize, size: usize) -> CLResult<&[u8]>;
}

pub struct VclReplyBuffer {
    /// Resource for receiving the reply
    pub res: Box<dyn VclResource>,
}

impl VclReplyBuffer {
    pub fn new_for_virtgpu(virtgpu: &VirtGpu, size: usize) -> CLResult<Self> {
        Ok(Self {
            res: Box::new(VirtGpuResource::new(virtgpu, size)?),
        })
    }

    pub fn new_for_vtest(vtest: &Vtest, size: usize) -> CLResult<Self> {
        Ok(Self {
            res: Box::new(VtestResource::new(vtest, size)?),
        })
    }

    pub fn map(&mut self, size: usize) -> CLResult<&[u8]> {
        self.res.map(0, size)
    }

    pub fn len(&self) -> usize {
        self.res.len()
    }

    pub fn get_ptr(&self) -> *const c_void {
        self.res.get_ptr()
    }

    pub fn get_handle(&self) -> i32 {
        self.res.get_handle()
    }
}

pub trait AsBytes {
    fn as_bytes(&self) -> &[u8];
    fn as_mut_bytes(&mut self) -> &mut [u8];
}

impl<T> AsBytes for T {
    fn as_bytes(&self) -> &[u8] {
        let new_len = mem::size_of_val(self) / mem::size_of::<u8>();
        unsafe { slice::from_raw_parts(self as *const Self as _, new_len) }
    }

    fn as_mut_bytes(&mut self) -> &mut [u8] {
        let new_len = mem::size_of_val(self) / mem::size_of::<u8>();
        unsafe { slice::from_raw_parts_mut(self as *mut Self as _, new_len) }
    }
}

impl<T> AsBytes for [T] {
    fn as_bytes(&self) -> &[u8] {
        let new_len = mem::size_of::<T>() * self.len() / mem::size_of::<u8>();
        unsafe { slice::from_raw_parts(self as *const Self as _, new_len) }
    }

    fn as_mut_bytes(&mut self) -> &mut [u8] {
        let new_len = mem::size_of::<T>() * self.len() / mem::size_of::<u8>();
        unsafe { slice::from_raw_parts_mut(self as *mut Self as _, new_len) }
    }
}

#[cfg(test)]
mod test {
    use std::ffi::CString;

    use super::*;

    #[test]
    fn slice_as_bytes() {
        let mut integers = [0u32; 4];
        assert_eq!(
            integers.as_bytes().len(),
            integers.len() * mem::size_of_val(&integers[0])
        );
        assert_eq!(integers.as_bytes().len(), mem::size_of_val(&integers));

        let integers_slice = &integers[0..2];
        assert_eq!(
            integers_slice.as_bytes().len(),
            integers_slice.len() * mem::size_of_val(&integers_slice[0])
        );
        let mut_integers_slice = &mut integers[0..2];
        assert_eq!(
            mut_integers_slice.as_mut_bytes().len(),
            mut_integers_slice.len() * mem::size_of_val(&mut_integers_slice[0])
        );

        let mut bytes_array = [0u8, 0, 0, 0];
        let bytes_slice = bytes_array.as_slice();
        assert_eq!(bytes_slice.as_bytes().len(), bytes_slice.len());

        assert_eq!(bytes_array[0..2].as_bytes().len(), 2);
        assert_eq!(bytes_array[0..2].as_mut_bytes().len(), 2);

        let test_string = "test_string";
        let name_c = CString::new(test_string).unwrap();
        let name_bytes = name_c.as_bytes_with_nul();
        assert_eq!(name_bytes.len(), name_bytes.as_bytes().len());
        assert_eq!(name_bytes.len(), test_string.len() + 1);

        let bytes_as_bytes = bytes_array.as_bytes();
        let bytes_as_bytes_slice = &bytes_as_bytes[0..2];
        assert_eq!(
            bytes_as_bytes_slice.as_bytes().len(),
            bytes_as_bytes_slice.len()
        );
    }
}
