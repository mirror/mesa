/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use vcl_drm_gen::*;

use std::os::unix::io::AsRawFd;
use std::{ffi::CStr, fs::File, ptr};

use super::virtgpu::VirtGpuError;

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
                eprintln!("Failed to open DRM devices: {}", ioerr);
            } else {
                eprintln!("No DRM devices found");
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

        match File::open(&node_path) {
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
}
