/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::{api::icd::DISPATCH, dev::virtgpu::*};

use vcl_opencl_gen::{cl_device_id, cl_icd_dispatch};

pub struct Device {
    _dispatch: &'static cl_icd_dispatch,
    pub gpu: VirtGpu,
}

impl Device {
    pub fn new(gpu: VirtGpu) -> Self {
        Self {
            _dispatch: &DISPATCH,
            gpu: gpu,
        }
    }

    pub fn all() -> Result<Vec<Device>, VirtGpuError> {
        Ok(VirtGpu::all()?.into_iter().map(Self::new).collect())
    }

    pub fn as_ptr(&self) -> cl_device_id {
        self as *const Self as _
    }
}
