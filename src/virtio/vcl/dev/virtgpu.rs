/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::dev::drm::*;
use vcl_drm_gen::*;
use vcl_virglrenderer_gen::*;

use std::ffi::CStr;

#[derive(Debug)]
pub enum VirtGpuError {
    DrmDevice,
    Ioctl,
}

pub struct VirtGpu {
    pub drm_device: DrmDevice,
    pub capset: VirtGpuCapset,
}

impl VirtGpu {
    pub fn new(drm_device: DrmDevice) -> Result<Self, VirtGpuError> {
        let capset = VirtGpuCapset::new(&drm_device)?;
        Ok(Self { drm_device, capset })
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
