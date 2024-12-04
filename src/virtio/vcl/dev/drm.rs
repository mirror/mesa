/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use vcl_drm_gen::*;
use vcl_sys_gen::*;
use vcl_virglrenderer_gen::*;

use std::backtrace::*;
use std::ffi::c_void;
use std::ffi::CStr;
use std::fs::File;
use std::fs::OpenOptions;
use std::os::unix::io::AsRawFd;
use std::*;

use crate::dev::debug::VclDebugFlags;
use crate::log;

use super::virtgpu::{VirtGpuError, VirtGpuParamId};

/// 4.1.2 PCI Device Discovery: https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html
const VIRT_PCI_VENDOR_ID: u16 = 0x1af4;
const VIRT_PCI_OFFSET: u16 = 0x1040;
const VIRTGPU_DEVICE_ID: u16 = 16;
const VIRTGPU_PCI_DEVICE_ID: u16 = VIRT_PCI_OFFSET + VIRTGPU_DEVICE_ID;

pub struct DrmDevice {
    pub file: File,
    pub name: String,
}

impl DrmDevice {
    pub fn new(ptr: drmDevicePtr) -> Self {
        let file = Self::open_file(ptr);
        let name = Self::get_name(&file);
        Self { file, name }
    }

    /// Returns all VirtIO-GPUs available in the system
    pub fn virtgpus() -> Result<Vec<DrmDevice>, VirtGpuError> {
        const MAX_DEVICES: usize = 4;
        let mut ptrs: [drmDevicePtr; MAX_DEVICES] = [ptr::null_mut(); MAX_DEVICES];

        let count = unsafe { drmGetDevices2(0, ptrs.as_mut_ptr(), MAX_DEVICES as i32) };
        if count <= 0 {
            // When count is negative, it can be interpreted as an OS error code
            if count < 0 {
                let ioerr = std::io::Error::from_raw_os_error(-count);
                log!(
                    VclDebugFlags::Error,
                    "Failed to open DRM devices: {}",
                    ioerr
                );
            } else {
                log!(VclDebugFlags::Error, "No DRM devices found");
            }
            return Err(VirtGpuError::DrmDevice);
        }

        let devs = ptrs
            .into_iter()
            .take(count as usize)
            .filter(DrmDevice::is_virtgpu)
            .map(DrmDevice::new)
            .collect();

        unsafe { drmFreeDevices(ptrs.as_mut_ptr(), count) };
        Ok(devs)
    }

    pub fn bustype(ptr: drmDevicePtr) -> u32 {
        unsafe { *ptr }.bustype as u32
    }

    pub fn vendor_id(ptr: drmDevicePtr) -> u16 {
        unsafe { *(*ptr).deviceinfo.pci }.vendor_id
    }

    pub fn device_id(ptr: drmDevicePtr) -> u16 {
        unsafe { *(*ptr).deviceinfo.pci }.device_id
    }

    pub fn available_nodes(ptr: drmDevicePtr) -> i32 {
        unsafe { *ptr }.available_nodes
    }

    pub fn is_virtgpu(ptr: &drmDevicePtr) -> bool {
        let is_bus_supported = match Self::bustype(*ptr) {
            DRM_BUS_PCI => {
                Self::vendor_id(*ptr) == VIRT_PCI_VENDOR_ID
                    && Self::device_id(*ptr) == VIRTGPU_PCI_DEVICE_ID
            }
            DRM_BUS_PLATFORM => true,
            _ => false,
        };

        let is_render_node = Self::available_nodes(*ptr) & (1 << DRM_NODE_RENDER) != 0;

        is_bus_supported && is_render_node
    }

    pub fn open_file(ptr: drmDevicePtr) -> File {
        let node_path = unsafe { (*ptr).nodes }.wrapping_add(DRM_NODE_RENDER as usize);
        let node_path = unsafe { CStr::from_ptr(*node_path) }
            .to_str()
            .expect("Failed to read DRM node path");

        let result = OpenOptions::new().read(true).write(true).open(&node_path);
        match result {
            Ok(file) => file,
            Err(e) => panic!("Failed to open DRM node {}: {}", node_path, e),
        }
    }

    pub fn get_name(file: &File) -> String {
        let drm_version = unsafe { drmGetVersion(file.as_raw_fd()) };
        let name = unsafe { CStr::from_ptr((*drm_version).name) }
            .to_str()
            .expect("Failed to read DRM version name");
        let ret = name.to_string();

        unsafe { drmFreeVersion(drm_version) };

        ret
    }

    pub fn ioctl<T: std::fmt::Debug>(&self, request: u64, arg: &mut T) -> Result<(), VirtGpuError> {
        log!(VclDebugFlags::Ioctl, "{:?}: ...", arg);
        let ret = unsafe { drmIoctl(self.file.as_raw_fd(), request, arg as *mut _ as _) };
        if ret != 0 {
            log!(
                VclDebugFlags::Error,
                "ioctl: {:?}: {}",
                arg,
                std::io::Error::last_os_error(),
            );
            let backtrace = Backtrace::capture();
            if backtrace.status() == BacktraceStatus::Captured {
                log!(VclDebugFlags::Error, "{}", backtrace);
            }
            Err(VirtGpuError::Ioctl(ret))
        } else {
            log!(VclDebugFlags::Ioctl, "{:?}: OK", arg);
            Ok(())
        }
    }

    pub fn get_param(&self, param: VirtGpuParamId) -> Result<u64, VirtGpuError> {
        let mut value = 0;
        let mut getparam = drm_virtgpu_getparam {
            param: param.id(),
            value: &mut value as *mut _ as _,
        };
        self.ioctl(drm_ioctl_virtgpu_GETPARAM as u64, &mut getparam)?;
        Ok(value)
    }

    pub fn init_context(&self) -> Result<(), VirtGpuError> {
        let mut ctx_set_param = drm_virtgpu_context_set_param {
            param: virtgpu_context_param_CAPSET_ID as u64,
            value: virgl_renderer_capset::VIRGL_RENDERER_CAPSET_VCL as _,
        };

        let mut ctx_init = drm_virtgpu_context_init {
            ctx_set_params: &mut ctx_set_param as *mut _ as _,
            pad: 0,
            num_params: 1,
        };

        if let Err(VirtGpuError::Ioctl(ret)) =
            self.ioctl(drm_ioctl_virtgpu_CONTEXT_INIT as u64, &mut ctx_init)
        {
            if std::io::Error::last_os_error().raw_os_error() != Some(EEXIST as _) {
                return Err(VirtGpuError::Ioctl(ret));
            }
        }

        Ok(())
    }

    pub fn exec_buffer(&self, buffer: &[u8]) -> Result<(), VirtGpuError> {
        let mut eb = drm_virtgpu_execbuffer {
            command: buffer.as_ptr() as _,
            size: buffer.len() as u32,
            num_bo_handles: 0,
            bo_handles: 0,
            fence_fd: -1,
            flags: 0,
            ring_idx: 0,
            num_in_syncobjs: 0,
            in_syncobjs: 0,
            num_out_syncobjs: 0,
            out_syncobjs: 0,
            syncobj_stride: 0,
        };
        self.ioctl(drm_ioctl_virtgpu_EXECBUFFER, &mut eb)
    }

    pub fn resource_create(&self, size: u32) -> Result<(u32, u32), VirtGpuError> {
        let mut create_cmd = drm_virtgpu_resource_create {
            target: 0, // 0 is PIPE_BUFFER
            format: 0,
            bind: VIRGL_BIND_CUSTOM,
            width: size,
            height: 1,
            depth: 1,
            array_size: 1,
            last_level: 0,
            nr_samples: 0,
            flags: 0,
            bo_handle: 0,
            res_handle: 0,
            size: size,
            stride: 0,
        };
        self.ioctl(drm_ioctl_virtgpu_RESOURCE_CREATE as u64, &mut create_cmd)?;
        Ok((create_cmd.res_handle, create_cmd.bo_handle))
    }

    pub fn transfer_get(&self, bo_handle: u32, size: usize) -> Result<(), VirtGpuError> {
        let mut get_cmd = drm_virtgpu_3d_transfer_from_host {
            bo_handle,
            level: 0,
            offset: 0,
            box_: drm_virtgpu_3d_box {
                x: 0,
                y: 0,
                z: 0,
                w: size as u32,
                h: 1,
                d: 1,
            },
            stride: 0,
            layer_stride: 0,
        };
        self.ioctl(drm_ioctl_virtgpu_TRANSFER_FROM_HOST, &mut get_cmd)
    }

    pub fn transfer_put(&self, bo_handle: u32, size: usize) -> Result<(), VirtGpuError> {
        let mut put_cmd = drm_virtgpu_3d_transfer_to_host {
            bo_handle,
            level: 0,
            offset: 0,
            box_: drm_virtgpu_3d_box {
                x: 0,
                y: 0,
                z: 0,
                w: size as u32,
                h: 1,
                d: 1,
            },
            stride: 0,
            layer_stride: 0,
        };
        self.ioctl(drm_ioctl_virtgpu_TRANSFER_TO_HOST, &mut put_cmd)
    }

    pub fn map(&self, bo_handle: u32, size: usize) -> Result<*mut c_void, VirtGpuError> {
        let mut map_arg = drm_virtgpu_map {
            offset: 0,
            handle: bo_handle,
            pad: 0,
        };
        self.ioctl(drm_ioctl_virtgpu_MAP as u64, &mut map_arg)?;

        let ptr = unsafe {
            mmap(
                ptr::null_mut(),
                size,
                (PROT_READ | PROT_WRITE) as i32,
                MAP_SHARED as i32,
                self.file.as_raw_fd(),
                map_arg.offset as i64,
            )
        };
        if ptr == unsafe { mem::transmute(MapResult::FAILED) } {
            log!(
                VclDebugFlags::Error,
                "Failed to map: {}",
                io::Error::last_os_error()
            );
            return Err(VirtGpuError::Map);
        }

        Ok(ptr)
    }

    pub fn wait(&self, bo_handle: u32) -> Result<(), VirtGpuError> {
        let mut wait_cmd = drm_virtgpu_3d_wait {
            handle: bo_handle,
            flags: 0,
        };
        self.ioctl(drm_ioctl_virtgpu_WAIT, &mut wait_cmd)
    }
}
