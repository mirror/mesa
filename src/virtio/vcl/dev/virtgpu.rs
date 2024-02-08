/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::dev::drm::*;
use vcl_drm_gen::*;
use vcl_virglrenderer_gen::*;

use std::ffi::CStr;
use std::rc::Rc;
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
}

static VIRTGPU_ONCE: Once = Once::new();

static mut VIRTGPU: Option<VirtGpu> = None;

impl VirtGpu {
    pub fn init_once() -> Result<(), VirtGpuError> {
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

    pub fn get() -> &'static Self {
        debug_assert!(VIRTGPU_ONCE.is_completed());
        // SAFETY: no mut references exist at this point
        unsafe { VIRTGPU.as_ref().unwrap() }
    }

    pub fn new(drm_device: DrmDevice) -> Result<Self, VirtGpuError> {
        let drm_device = Rc::new(drm_device);

        let params = VirtGpuParams::new(&drm_device)?;

        // CONTEXT_INIT is needed to initialize a context for VCL.
        // Without this, it would go for a VIRGL context.
        if !params[VirtGpuParamId::ContextInit].is_set() {
            eprintln!("VCL can not use VirtIO-GPU without CONTEXT_INIT kernel parameter");
            return Err(VirtGpuError::Param);
        }

        drm_device.init_context()?;

        let capset = VirtGpuCapset::new(&drm_device)?;
        let ret = Self {
            drm_device,
            params,
            capset,
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
