// Copyright 2025 Android Open Source Project
// SPDX-License-Identifier: MIT

use std::sync::Arc;

use mesa3d_util::MesaError;
use mesa3d_util::MesaResult;
use mesa3d_virtio_gpu::VirtGpuKumquat;

use crate::magma::MagmaPhysicalDevice;
use crate::magma_defines::MagmaCreateBufferInfo;
use crate::magma_defines::MagmaError;
use crate::magma_defines::MagmaHeapBudget;
use crate::magma_defines::MagmaMemoryProperties;
use crate::magma_defines::MagmaPciBusInfo;
use crate::magma_defines::MagmaPciInfo;
use crate::sys::platform::PlatformPhysicalDevice;
use crate::traits::AsVirtGpu;
use crate::traits::Buffer;
use crate::traits::Context;
use crate::traits::Device;
use crate::traits::GenericDevice;
use crate::traits::GenericPhysicalDevice;
use crate::traits::PhysicalDevice;

pub struct MagmaEncoder {
    virtgpu: VirtGpuKumquat,
}

impl MagmaEncoder {
    pub fn new() -> MesaResult<MagmaEncoder> {
        Ok(MagmaEncoder {
            virtgpu: VirtGpuKumquat::new("/tmp/kumquat-gpu-0")?,
        })
    }
}

impl AsVirtGpu for MagmaEncoder {
    fn as_virtgpu(&self) -> Option<&VirtGpuKumquat> {
        Some(&self.virtgpu)
    }
}

impl PlatformPhysicalDevice for MagmaEncoder {}
impl PhysicalDevice for MagmaEncoder {}

impl GenericPhysicalDevice for MagmaEncoder {
    fn create_device(
        &self,
        physical_device: &Arc<dyn PhysicalDevice>,
    ) -> MesaResult<Arc<dyn Device>> {
        Err(MesaError::Unsupported)
    }
}

impl GenericDevice for MagmaEncoder {
    fn get_memory_properties(&self) -> MesaResult<MagmaMemoryProperties> {
        Err(MesaError::Unsupported)
    }

    fn get_memory_budget(&self, _heap_idx: u32) -> MesaResult<MagmaHeapBudget> {
        Err(MesaError::Unsupported)
    }

    fn create_context(&self, device: &Arc<dyn Device>) -> MesaResult<Arc<dyn Context>> {
        Err(MesaError::Unsupported)
    }

    fn create_buffer(
        &self,
        device: &Arc<dyn Device>,
        create_info: &MagmaCreateBufferInfo,
    ) -> MesaResult<Arc<dyn Buffer>> {
        Err(MesaError::Unsupported)
    }
}

pub fn enumerate_devices() -> MesaResult<Vec<MagmaPhysicalDevice>> {
    let mut pci_info: MagmaPciInfo = Default::default();
    let mut pci_bus_info: MagmaPciBusInfo = Default::default();
    let mut devices: Vec<MagmaPhysicalDevice> = Vec::new();

    let enc = MagmaEncoder::new()?;
    // TODO): Get data from the server

    devices.push(MagmaPhysicalDevice::new(
        Arc::new(enc),
        pci_info,
        pci_bus_info,
    ));

    Ok(devices)
}
