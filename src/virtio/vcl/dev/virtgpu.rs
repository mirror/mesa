/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::core::platform::Platform;
use crate::dev::debug::*;
use crate::dev::drm::*;
use crate::log;
use crate::protocol::ring::VirtGpuRing;

use vcl_drm_gen::*;
use vcl_opencl_gen::cl_platform_id;
use vcl_virglrenderer_gen::*;

use std::env;
use std::ffi::CStr;
use std::pin::Pin;
use std::rc::Rc;
use std::str::FromStr;
use std::sync::Once;

#[derive(Debug)]
pub enum VirtGpuError {
    DrmDevice,
    Ioctl(i32),
    Param,
    Map,
}

pub struct VirtGpu {
    pub drm_device: Rc<DrmDevice>,
    pub params: VirtGpuParams,
    pub capset: VirtGpuCapset,
    pub ring: VirtGpuRing,
    pub platforms: Vec<Pin<Box<Platform>>>,
}

static VCL_ENV_ONCE: Once = Once::new();
static VIRTGPU_ONCE: Once = Once::new();

static mut VCL_DEBUG: VclDebug = VclDebug {
    flags: VclDebugFlags::Empty,
};
static mut VIRTGPU: Option<VirtGpu> = None;

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

impl VirtGpu {
    pub fn init_once() -> Result<(), VirtGpuError> {
        VCL_ENV_ONCE.call_once(load_env);
        // SAFETY: no concurrent static mut access due to std::Once
        VIRTGPU_ONCE.call_once(|| {
            let drm_devices = DrmDevice::virtgpus().expect("Failed to find VirtIO-GPUs");
            assert!(!drm_devices.is_empty(), "Failed to find VirtIO-GPUs");

            let first_drm_device = drm_devices.into_iter().nth(0).unwrap();
            let virtgpu =
                VirtGpu::new(first_drm_device).expect("Failed to create VirtGpu from DRM device");
            unsafe { VIRTGPU.replace(virtgpu) };
        });
        Ok(())
    }

    pub fn debug() -> &'static VclDebug {
        debug_assert!(VCL_ENV_ONCE.is_completed());
        unsafe { &VCL_DEBUG }
    }

    pub fn get() -> &'static Self {
        debug_assert!(VIRTGPU_ONCE.is_completed());
        // SAFETY: no mut references exist at this point
        unsafe { VIRTGPU.as_ref().unwrap() }
    }

    pub fn get_mut() -> &'static mut Self {
        debug_assert!(VIRTGPU_ONCE.is_completed());
        // This is NOT safe
        unsafe { VIRTGPU.as_mut().unwrap() }
    }

    pub fn get_ring(&mut self) -> &mut VirtGpuRing {
        debug_assert!(VIRTGPU_ONCE.is_completed());
        &mut self.ring
    }

    pub fn get_platforms(&self) -> &[Pin<Box<Platform>>] {
        debug_assert!(VIRTGPU_ONCE.is_completed());
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

    pub fn new(drm_device: DrmDevice) -> Result<Self, VirtGpuError> {
        let drm_device = Rc::new(drm_device);

        let params = VirtGpuParams::new(&drm_device)?;

        // CONTEXT_INIT is needed to initialize a context for VCL.
        // Without this, it would go for a VIRGL context.
        if !params[VirtGpuParamId::ContextInit].is_set() {
            log!(
                VclDebugFlags::Error,
                "VCL can not use VirtIO-GPU without CONTEXT_INIT kernel parameter"
            );
            return Err(VirtGpuError::Param);
        }

        drm_device.init_context()?;

        let capset = VirtGpuCapset::new(&drm_device)?;

        let mut ring = VirtGpuRing::new(drm_device.clone())?;

        let platforms = Platform::all(&mut ring)?;

        let ret = Self {
            drm_device,
            params,
            capset,
            ring,
            platforms,
        };
        Ok(ret)
    }

    /// Returns all VirtIO-GPUs available in the system
    pub fn all() -> Result<Vec<VirtGpu>, VirtGpuError> {
        let gpus: Result<_, _> = DrmDevice::virtgpus()?
            .into_iter()
            .map(VirtGpu::new)
            .collect();
        Ok(gpus?)
    }
}

#[repr(u64)]
#[derive(Copy, Clone, Debug)]
pub enum VirtGpuParamId {
    Features3D = 0,
    CapsetFix = 1,
    ResourceBlob = 2,
    HostVisible = 3,
    CrossDevice = 4,
    ContextInit = 5,
    SupportedCapsetIds = 6,
    Max = 7,
}

impl VirtGpuParamId {
    const fn count() -> usize {
        Self::Max as usize
    }

    pub fn id(self) -> u64 {
        self as u64
    }
}

#[derive(Debug)]
pub struct VirtGpuParam {
    id: VirtGpuParamId,
    value: u64,
}

impl VirtGpuParam {
    fn new(id: VirtGpuParamId) -> Self {
        Self { id, value: 0 }
    }

    fn set(&mut self, value: u64) {
        self.value = value;
    }

    fn is_set(&self) -> bool {
        self.value == 1
    }
}

pub struct VirtGpuParams {
    params: [VirtGpuParam; VirtGpuParamId::count()],
}

impl VirtGpuParams {
    pub fn new(drm_device: &DrmDevice) -> Result<VirtGpuParams, VirtGpuError> {
        let mut params = [
            VirtGpuParam::new(VirtGpuParamId::Features3D),
            VirtGpuParam::new(VirtGpuParamId::CapsetFix),
            VirtGpuParam::new(VirtGpuParamId::ResourceBlob),
            VirtGpuParam::new(VirtGpuParamId::HostVisible),
            VirtGpuParam::new(VirtGpuParamId::CrossDevice),
            VirtGpuParam::new(VirtGpuParamId::ContextInit),
            VirtGpuParam::new(VirtGpuParamId::SupportedCapsetIds),
        ];
        for param in &mut params {
            param.set(drm_device.get_param(param.id).unwrap_or_default());
        }
        Ok(Self { params })
    }
}

impl std::ops::Index<VirtGpuParamId> for VirtGpuParams {
    type Output = VirtGpuParam;

    fn index(&self, index: VirtGpuParamId) -> &Self::Output {
        &self.params[index as usize]
    }
}

#[repr(C)]
#[derive(Default)]
pub struct VirtGpuCapset {
    id: virgl_renderer_capset,
    version: u32,
    data: VirglRendererCapsetVcl,
}

impl VirtGpuCapset {
    pub fn new(drm_device: &DrmDevice) -> Result<VirtGpuCapset, VirtGpuError> {
        let mut capset = Self {
            id: virgl_renderer_capset_VIRGL_RENDERER_CAPSET_VCL,
            version: 0,
            data: Default::default(),
        };

        let mut args = drm_virtgpu_get_caps {
            cap_set_id: capset.id,
            cap_set_ver: capset.version,
            addr: capset.data.as_mut_ptr() as u64,
            size: std::mem::size_of_val(&capset.data) as u32,
            pad: 0,
        };

        drm_device.ioctl(drm_ioctl_virtgpu_GET_CAPS, &mut args)?;

        Ok(capset)
    }

    pub fn get_host_platform_name(&self) -> &CStr {
        CStr::from_bytes_until_nul(&self.data.platform_name)
            .expect("Failed to create CStr for host platform name")
    }
}

#[repr(C)]
#[derive(Default)]
pub struct VirglRendererCapsetVcl {
    platform_name: [u8; 32],
}

impl VirglRendererCapsetVcl {
    pub fn as_mut_ptr(&mut self) -> *mut Self {
        self as _
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn finds_virtgpu() {
        let gpus = VirtGpu::all();
        assert!(gpus.is_ok());
        let gpus = gpus.unwrap();
        assert!(gpus.len() > 0);
        for gpu in &gpus {
            assert_eq!(
                gpu.capset.get_host_platform_name().to_string_lossy(),
                "virglrenderer vcomp"
            );
        }
    }
}
