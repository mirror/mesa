/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::dev::drm::*;

#[derive(Debug)]
pub enum VirtGpuError {
    DrmDevice,
}

pub struct VirtGpu {
    pub drm_device: DrmDevice,
}

impl VirtGpu {
    pub fn new(drm_device: DrmDevice) -> Self {
        Self { drm_device }
    }

    /// Returns all VirtIO-GPUs available in the system
    pub fn all() -> Result<Vec<VirtGpu>, VirtGpuError> {
        Ok(DrmDevice::virtgpus()?
            .into_iter()
            .map(VirtGpu::new)
            .collect())
    }
}
