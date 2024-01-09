/*
 * Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

use crate::api::icd::CLObjectBase;
use crate::{dev::virtgpu::*, impl_cl_type_trait};

use vcl_opencl_gen::*;

impl_cl_type_trait!(cl_device_id, Device, CL_INVALID_DEVICE);

pub struct Device {
    base: CLObjectBase<CL_INVALID_DEVICE>,
    pub gpu: VirtGpu,
}

impl Device {
    pub fn new(gpu: VirtGpu) -> Self {
        Self {
            base: CLObjectBase::new(),
            gpu: gpu,
        }
    }

    pub fn all() -> Result<Vec<Device>, VirtGpuError> {
        Ok(VirtGpu::all()?.into_iter().map(Self::new).collect())
    }
}
