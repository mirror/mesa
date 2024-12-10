// Copyright 2025 Android Open Source Project
// SPDX-License-Identifier: MIT

use std::sync::Arc;

use mesa3d_util::MappedRegion;
use mesa3d_util::MesaError;
use mesa3d_util::MesaHandle;
use mesa3d_util::MesaResult;
use mesa3d_virtio_gpu::VirtGpuKumquat;

use crate::magma_defines::MagmaCreateBufferInfo;
use crate::magma_defines::MagmaHeapBudget;
use crate::magma_defines::MagmaMemoryProperties;
use crate::sys::platform::PlatformDevice;
use crate::sys::platform::PlatformPhysicalDevice;

pub trait AsVirtGpu {
    fn as_virtgpu(&self) -> Option<&VirtGpuKumquat> {
        None
    }
}

pub trait GenericPhysicalDevice {
    fn create_device(
        &self,
        physical_device: &Arc<dyn PhysicalDevice>,
    ) -> MesaResult<Arc<dyn Device>>;
}

pub trait GenericDevice {
    fn get_memory_properties(&self) -> MesaResult<MagmaMemoryProperties>;

    fn get_memory_budget(&self, _heap_idx: u32) -> MesaResult<MagmaHeapBudget>;

    fn create_context(&self, device: &Arc<dyn Device>) -> MesaResult<Arc<dyn Context>>;

    fn create_buffer(
        &self,
        device: &Arc<dyn Device>,
        create_info: &MagmaCreateBufferInfo,
    ) -> MesaResult<Arc<dyn Buffer>>;
}

pub trait GenericBuffer {
    fn map(&self) -> MesaResult<Arc<dyn MappedRegion>>;

    fn export(&self) -> MesaResult<MesaHandle>;
}

pub trait PhysicalDevice: PlatformPhysicalDevice + AsVirtGpu + GenericPhysicalDevice {}
pub trait Device: GenericDevice + PlatformDevice {}
pub trait Context {}
pub trait Buffer: GenericBuffer {}
