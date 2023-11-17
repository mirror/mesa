/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::dev::virtgpu::*;

pub struct Device {
    _gpu: VirtGpu,
}

impl Device {
    pub fn new(gpu: VirtGpu) -> Self {
        Self { _gpu: gpu }
    }

    pub fn all() -> Result<Vec<Device>, VirtGpuError> {
        Ok(VirtGpu::all()?.into_iter().map(Self::new).collect())
    }
}
