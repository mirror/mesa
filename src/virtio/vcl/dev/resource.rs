/*
 * Copyright © 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use std::{ffi::c_void, os::fd::AsRawFd, rc::Rc};

use crate::dev::debug::VclDebugFlags;
use crate::dev::drm::DrmDevice;
use crate::dev::virtgpu::*;
use crate::log;

use vcl_drm_gen::*;
use vcl_mman_gen::*;
use vcl_virglrenderer_gen::*;

pub struct VirtGpuResource {
    pub res_handle: u32,
    bo_handle: u32,
    buf: *mut c_void,
    size: usize,

    drm_device: Rc<DrmDevice>,
}

impl VirtGpuResource {
    pub fn new(drm_device: Rc<DrmDevice>, size: usize) -> Result<Self, VirtGpuError> {
        let mut create_cmd = drm_virtgpu_resource_create {
            target: 0, // 0 is PIPE_BUFFER
            format: 0,
            bind: VIRGL_BIND_CUSTOM,
            width: size as u32,
            height: 1,
            depth: 1,
            array_size: 1,
            last_level: 0,
            nr_samples: 0,
            flags: 0,
            bo_handle: 0,
            res_handle: 0,
            size: size as u32,
            stride: 0,
        };

        drm_device.ioctl(drm_ioctl_virtgpu_RESOURCE_CREATE as u64, &mut create_cmd)?;

        Ok(VirtGpuResource {
            res_handle: create_cmd.res_handle,
            bo_handle: create_cmd.bo_handle,
            buf: std::ptr::null_mut(),
            size: create_cmd.width as usize,
            drm_device,
        })
    }

    pub fn wait(&self) -> Result<(), VirtGpuError> {
        let mut wait_cmd = drm_virtgpu_3d_wait {
            handle: self.bo_handle,
            flags: 0,
        };
        self.drm_device
            .ioctl(drm_ioctl_virtgpu_WAIT, &mut wait_cmd)?;
        Ok(())
    }

    pub fn transfer_get(&self) -> Result<(), VirtGpuError> {
        let mut get_cmd = drm_virtgpu_3d_transfer_from_host {
            bo_handle: self.bo_handle,
            level: 0,
            offset: 0,
            box_: drm_virtgpu_3d_box {
                x: 0,
                y: 0,
                z: 0,
                w: self.size as u32,
                h: 1,
                d: 1,
            },
            stride: 0,
            layer_stride: 0,
        };

        self.drm_device
            .ioctl(drm_ioctl_virtgpu_TRANSFER_FROM_HOST, &mut get_cmd)
    }

    fn map_impl(&self) -> Result<*mut c_void, VirtGpuError> {
        let mut map_arg = drm_virtgpu_map {
            offset: 0,
            handle: self.bo_handle,
            pad: 0,
        };
        self.drm_device
            .ioctl(drm_ioctl_virtgpu_MAP as u64, &mut map_arg)?;

        let ptr = unsafe {
            vcl_mman_gen::mmap(
                std::ptr::null_mut(),
                self.size,
                (PROT_READ | PROT_WRITE) as i32,
                MAP_SHARED as i32,
                self.drm_device.file.as_raw_fd(),
                map_arg.offset as i64,
            )
        };
        if ptr == map_result_FAILED as _ {
            log!(
                VclDebugFlags::Error,
                "Failed to map: {}",
                std::io::Error::last_os_error()
            );
            return Err(VirtGpuError::Map);
        }

        Ok(ptr)
    }

    pub fn map(&mut self, offset: usize, size: usize) -> Result<&[u8], VirtGpuError> {
        // We need to trigger a transfer to put the resource in a busy state
        self.transfer_get()?;
        // The wait will make sure the transfer has finished before mapping
        self.wait()?;

        if self.buf == std::ptr::null_mut() {
            self.buf = self.map_impl()?;
        }
        assert!(offset + size <= self.size);
        Ok(unsafe { std::slice::from_raw_parts((self.buf as *mut u8).add(offset), size) })
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn resource() -> Result<(), VirtGpuError> {
        VirtGpu::init_once()?;
        let gpu = VirtGpu::get();
        let mut resource = VirtGpuResource::new(gpu.drm_device.clone(), 1024)?;
        resource.map(0, 1024)?;
        Ok(())
    }
}
