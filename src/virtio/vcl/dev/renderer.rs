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

use ops::Deref;
use ops::DerefMut;
use vcl_opencl_gen::*;
use vcl_virglrenderer_gen::*;

use std::ffi::c_void;
use std::ffi::CStr;
use std::pin::Pin;
use std::str::FromStr;
use std::sync::Once;
use std::*;

pub trait VclRenderer {
    fn submit(&self, submit: VclCsEncoder) -> CLResult<()>;
    fn create_buffer(&self, size: usize) -> CLResult<VclBuffer>;
    fn transfer_get(&self, resource: &mut dyn VclResource) -> CLResult<()>;
    fn transfer_put(&self, resource: &mut dyn VclResource) -> CLResult<()>;
}

static VCL_ENV_ONCE: Once = Once::new();
static VCL_ONCE: Once = Once::new();

static mut VCL_DEBUG: VclDebug = VclDebug {
    flags: VclDebugFlags::Empty,
};
static mut VCL: Option<Vcl> = None;

fn load_env() {
    // We can not use log!() yet as it requires VCL_ENV_ONCE to be completed
    let debug = unsafe { &mut *ptr::addr_of_mut!(VCL_DEBUG) };

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

                    for platform in &mut VCL.as_mut().unwrap().platforms {
                        ret = platform.collect_extensions();
                        if ret.is_err() {
                            break;
                        }
                    }
                },
            }
        });
        ret
    }

    pub fn debug() -> &'static VclDebug {
        debug_assert!(VCL_ENV_ONCE.is_completed());
        unsafe { &*ptr::addr_of!(VCL_DEBUG) }
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
        let mut ret = Self {
            renderer: Box::new(Self::find_vcl_virtgpu()?),
            platforms: Vec::new(),
        };
        ret.platforms = Platform::all(&ret)?;
        Ok(ret)
    }

    pub fn find_vcl_virtgpu() -> CLResult<VirtGpu> {
        let drm_devices = DrmDevice::virtgpus()?;
        assert!(!drm_devices.is_empty(), "Failed to find VirtIO-GPUs");

        for device in drm_devices {
            if let Ok(gpu) = VirtGpu::new(device) {
                return Ok(gpu);
            }
        }
        return Err(CL_VIRTGPU_NOT_FOUND_MESA);
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

    /// Used only at initialization time. It is preferable not to use it after.
    pub fn get_unchecked() -> &'static Self {
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
            id: virgl_renderer_capset::VIRGL_RENDERER_CAPSET_VCL,
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

    /// Issues a transfer operation from host to guest
    fn transfer_get(&self) -> CLResult<()>;

    /// Issues a transfer operation from guest to host
    fn transfer_put(&self) -> CLResult<()>;

    /// Transfer get/put
    fn wait(&self) -> CLResult<()>;

    /// Maps resource memory into system memory for read
    fn map(&mut self, offset: usize, size: usize) -> CLResult<&[u8]>;

    /// Maps resource memory into system memory for read/write
    fn map_mut(&mut self, offset: usize, size: usize) -> CLResult<&mut [u8]>;
}

pub struct VclBuffer {
    pub res: Box<dyn VclResource>,
}

impl VclBuffer {
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
}

impl Deref for VclBuffer {
    type Target = Box<dyn VclResource>;

    fn deref(&self) -> &Self::Target {
        &self.res
    }
}

impl DerefMut for VclBuffer {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.res
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

    #[test]
    fn buffer() -> CLResult<()> {
        let vcl = Vcl::get();
        let a_hundred_mb = 100_000_000;
        let mut buffer = vcl.renderer.create_buffer(a_hundred_mb)?;
        let buffer_slice = buffer.map_mut(0, a_hundred_mb)?;
        buffer_slice.fill(1);
        buffer.transfer_put()?;
        buffer.wait()?;
        Ok(())
    }
}
