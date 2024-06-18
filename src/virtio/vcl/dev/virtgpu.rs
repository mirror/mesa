/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::CLResult;
use crate::dev::debug::*;
use crate::dev::drm::*;
use crate::dev::renderer::*;
use crate::log;
use crate::protocol::VclCsEncoder;

use vcl_drm_gen::*;
use vcl_opencl_gen::*;
use vcl_virglrenderer_gen::*;

use std::ffi::c_void;
use std::ptr;
use std::rc::Rc;
use std::slice;

#[derive(Debug)]
pub enum VirtGpuError {
    DrmDevice,
    Ioctl(i32),
    Param,
    Map,
}

impl From<VirtGpuError> for cl_int {
    fn from(e: VirtGpuError) -> Self {
        match e {
            VirtGpuError::DrmDevice => CL_DRM_DEVICE_FAILED_MESA,
            VirtGpuError::Ioctl(_) => CL_VIRTGPU_IOCTL_FAILED_MESA,
            VirtGpuError::Param => CL_VIRTGPU_PARAM_FAILED_MESA,
            VirtGpuError::Map => CL_VIRTGPU_MAP_FAILED_MESA,
        }
    }
}

pub struct VirtGpu {
    pub drm_device: Rc<DrmDevice>,
    pub params: VirtGpuParams,
    pub capset: VirglRendererCapset,
}

impl VirtGpu {
    pub fn new(drm_device: DrmDevice) -> CLResult<Self> {
        let drm_device = Rc::new(drm_device);

        let params = VirtGpuParams::new(&drm_device)?;

        // CONTEXT_INIT is needed to initialize a context for VCL.
        // Without this, it would go for a VIRGL context.
        if !params[VirtGpuParamId::ContextInit].is_set() {
            log!(
                VclDebugFlags::Error,
                "VCL can not use VirtIO-GPU without CONTEXT_INIT kernel parameter"
            );
            return Err(VirtGpuError::Param.into());
        }

        drm_device.init_context()?;

        let capset = VirglRendererCapset::new(&drm_device)?;

        let ret = Self {
            drm_device,
            params,
            capset,
        };

        Ok(ret)
    }

    /// Returns all VirtIO-GPUs available in the system
    pub fn all() -> CLResult<Vec<VirtGpu>> {
        let gpus: Vec<_> = DrmDevice::virtgpus()?
            .into_iter()
            .filter_map(|drm_device| VirtGpu::new(drm_device).ok())
            .collect();
        Ok(gpus)
    }
}

impl VclRenderer for VirtGpu {
    fn submit(&self, encoder: VclCsEncoder) -> CLResult<()> {
        if encoder.is_empty() {
            return Ok(());
        }
        self.drm_device.exec_buffer(encoder.get_slice())?;
        Ok(())
    }

    fn create_buffer(&self, size: usize) -> CLResult<VclBuffer> {
        Ok(VclBuffer::new_for_virtgpu(self, size)?)
    }

    fn transfer_get(&self, resource: &mut dyn VclResource) -> CLResult<()> {
        self.drm_device
            .transfer_get(resource.get_bo_handle(), resource.len())?;
        self.drm_device.wait(resource.get_bo_handle())?;
        resource.map(0, resource.len())?;
        Ok(())
    }

    fn transfer_put(&self, resource: &mut dyn VclResource) -> CLResult<()> {
        self.drm_device
            .transfer_put(resource.get_bo_handle(), resource.len())?;
        self.drm_device.wait(resource.get_bo_handle())?;
        Ok(())
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
            log!(
                VclDebugFlags::Info,
                "virtgpu {:?}: {}",
                param.id,
                param.is_set()
            );
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

impl VirglRendererCapset {
    pub fn new(drm_device: &DrmDevice) -> Result<Self, VirtGpuError> {
        let mut capset = Self {
            id: virgl_renderer_capset::VIRGL_RENDERER_CAPSET_VCL,
            version: 0,
            data: Default::default(),
        };

        let mut args = drm_virtgpu_get_caps {
            cap_set_id: capset.id as _,
            cap_set_ver: capset.version,
            addr: capset.data.as_mut_ptr() as u64,
            size: std::mem::size_of_val(&capset.data) as u32,
            pad: 0,
        };

        drm_device.ioctl(drm_ioctl_virtgpu_GET_CAPS, &mut args)?;

        Ok(capset)
    }
}

pub struct VirtGpuResource {
    pub res_handle: u32,
    bo_handle: u32,
    buf: *mut c_void,
    pub size: usize,

    drm_device: Rc<DrmDevice>,
}

impl VirtGpuResource {
    pub fn new(gpu: &VirtGpu, size: usize) -> Result<Self, VirtGpuError> {
        let (res_handle, bo_handle) = gpu.drm_device.resource_create(size as u32)?;
        Ok(VirtGpuResource {
            res_handle,
            bo_handle,
            buf: std::ptr::null_mut(),
            size,
            drm_device: gpu.drm_device.clone(),
        })
    }
}

impl VclResource for VirtGpuResource {
    fn transfer_get(&self) -> CLResult<()> {
        // We need to trigger a transfer to put the resource in a busy state
        self.drm_device.transfer_get(self.bo_handle, self.size)?;
        Ok(())
    }

    fn transfer_put(&self) -> CLResult<()> {
        self.drm_device.transfer_put(self.bo_handle, self.size)?;
        Ok(())
    }

    fn wait(&self) -> CLResult<()> {
        // The wait will make sure the transfer has finished before mapping
        self.drm_device.wait(self.bo_handle)?;
        Ok(())
    }

    fn map(&mut self, offset: usize, size: usize) -> CLResult<&[u8]> {
        if self.buf == ptr::null_mut() {
            self.buf = self.drm_device.map(self.bo_handle, self.size)?;
        }
        assert!(offset + size <= self.size);
        Ok(unsafe { slice::from_raw_parts((self.buf as *const u8).add(offset), size) })
    }

    fn map_mut(&mut self, offset: usize, size: usize) -> CLResult<&mut [u8]> {
        if self.buf == ptr::null_mut() {
            self.buf = self.drm_device.map(self.bo_handle, self.size)?;
        }
        assert!(offset + size <= self.size);
        Ok(unsafe { slice::from_raw_parts_mut((self.buf as *mut u8).add(offset), size) })
    }

    fn len(&self) -> usize {
        self.size
    }

    fn get_ptr(&self) -> *const c_void {
        self.buf
    }

    fn get_handle(&self) -> i32 {
        self.res_handle as _
    }

    fn get_bo_handle(&self) -> u32 {
        self.bo_handle
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

    #[test]
    fn resource() -> CLResult<()> {
        let drm_devices = DrmDevice::virtgpus()?;
        assert!(!drm_devices.is_empty(), "Failed to find VirtIO-GPUs");

        let first_drm_device = drm_devices.into_iter().nth(0).unwrap();
        let virtgpu = VirtGpu::new(first_drm_device)?;

        let mut resource = VirtGpuResource::new(&virtgpu, 1024)?;
        resource.map(0, 1024)?;
        Ok(())
    }
}
