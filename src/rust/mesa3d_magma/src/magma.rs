// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Magma: Rust implementation of Fuchsia's driver model.
//!
//! Design found at <https://fuchsia.dev/fuchsia-src/development/graphics/magma/concepts/design>.

use std::sync::Arc;

use mesa3d_util::OwnedDescriptor;
use zerocopy::FromBytes;
use zerocopy::IntoBytes;

use crate::magma_defines::MagmaCreateBufferInfo;
use crate::magma_defines::MagmaError;
use crate::magma_defines::MagmaHeapBudget;
use crate::magma_defines::MagmaMemoryProperties;
use crate::magma_defines::MagmaPciBusInfo;
use crate::magma_defines::MagmaPciInfo;
use crate::magma_defines::MagmaResult;
use crate::magma_defines::MAGMA_HEAP_DEVICE_LOCAL_BIT;

use crate::traits::Buffer;
use crate::traits::Context;
use crate::traits::Device;
use crate::traits::PhysicalDevice;

use crate::encoder::enumerate_devices as encoder_enumerate_devices;
use crate::sys::platform::enumerate_devices as platform_enumerate_devices;

const VIRTGPU_KUMQUAT_ENABLED: &str = "VIRTGPU_KUMQUAT";

#[repr(C)]
#[derive(Clone)]
pub struct MagmaPhysicalDevice {
    physical_device: Arc<dyn PhysicalDevice>,
    pci_info: MagmaPciInfo,
    pci_bus_info: MagmaPciBusInfo,
}

#[derive(Clone)]
pub struct MagmaDevice {
    device: Arc<dyn Device>,
}

#[derive(Clone)]
pub struct MagmaContext {
    context: Arc<dyn Context>,
}

#[derive(Clone)]
pub struct MagmaBuffer {
    buffer: Arc<dyn Buffer>,
}

pub fn magma_enumerate_devices() -> MagmaResult<Vec<MagmaPhysicalDevice>> {
    let devices = match std::env::var(VIRTGPU_KUMQUAT_ENABLED) {
        Ok(_) => encoder_enumerate_devices()?,
        Err(_) => platform_enumerate_devices()?,
    };

    Ok(devices)
}

impl MagmaPhysicalDevice {
    pub(crate) fn new(
        physical_device: Arc<dyn PhysicalDevice>,
        pci_info: MagmaPciInfo,
        pci_bus_info: MagmaPciBusInfo,
    ) -> MagmaPhysicalDevice {
        MagmaPhysicalDevice {
            physical_device,
            pci_info,
            pci_bus_info,
        }
    }

    pub fn create_device(&self) -> MagmaResult<MagmaDevice> {
        let device = self.physical_device.create_device(&self.physical_device)?;
        Ok(MagmaDevice { device })
    }
}

pub struct MagmaSemaphore {
    semaphore: OwnedDescriptor,
}

struct MagmaExecResource {
    buffer: MagmaBuffer,
    offset: u64,
    length: u64,
}

struct MagmaExecCommandBuffer {
    resource_idx: u32,
    unused: u32,
    start_offset: u64,
}

struct MagmaCommandDescriptor {
    flags: u64,
    command_buffers: Vec<MagmaExecCommandBuffer>,
    resources: Vec<MagmaExecResource>,
    wait_semaphores: Vec<MagmaSemaphore>,
    signal_semaphores: Vec<MagmaSemaphore>,
}

struct MagmaInlineCommandBuffer {
    data: Vec<u8>,
    wait_semaphores: Vec<MagmaSemaphore>,
    signal_semaphores: Vec<MagmaSemaphore>,
}

impl MagmaDevice {
    pub fn get_memory_properties(&self) -> MagmaResult<MagmaMemoryProperties> {
        let mut mem_props = self.device.get_memory_properties()?;
        // Strip away any device specific heap flags
        for heap in &mut mem_props.memory_heaps {
            heap.heap_flags = heap.heap_flags & MAGMA_HEAP_DEVICE_LOCAL_BIT;
        }

        Ok(mem_props)
    }

    pub fn get_memory_budget(&self, heap_idx: u32) -> MagmaResult<MagmaHeapBudget> {
        let budget = self.device.get_memory_budget(heap_idx)?;
        Ok(budget)
    }

    pub fn create_context(&self) -> MagmaResult<MagmaContext> {
        let context = self.device.create_context(&self.device)?;
        Ok(MagmaContext { context })
    }

    pub fn create_buffer(&self, create_info: &MagmaCreateBufferInfo) -> MagmaResult<MagmaBuffer> {
        let buffer = self.device.create_buffer(&self.device, create_info)?;
        Ok(MagmaBuffer { buffer })
    }

    pub fn map_buffer(
        _buffer: MagmaBuffer,
        _hw_va: u64,
        _offset: u64,
        _length: u64,
        _map_flags: u64,
    ) -> MagmaResult<()> {
        Err(MagmaError::Unimplemented)
    }

    pub fn flush() -> MagmaResult<()> {
        Err(MagmaError::Unimplemented)
    }

    pub fn raw_handle() -> MagmaResult<u64> {
        Err(MagmaError::Unimplemented)
    }
}

impl MagmaContext {
    pub fn execute_command(
        _connection: &MagmaPhysicalDevice,
        _command_descriptor: u64,
    ) -> MagmaResult<u64> {
        Err(MagmaError::Unimplemented)
    }

    pub fn execute_immediate_commands(
        _connection: &MagmaPhysicalDevice,
        _wait_semaphores: Vec<MagmaSemaphore>,
        _signal_semaphore: Vec<MagmaSemaphore>,
    ) -> MagmaResult<u64> {
        Err(MagmaError::Unimplemented)
    }

    pub fn raw_handle() -> MagmaResult<u64> {
        Err(MagmaError::Unimplemented)
    }
}

#[cfg(test)]
mod tests {
    use crate::magma::*;

    #[test]
    fn create_connection() {
        let devices = magma_enumerate_physical_devices().unwrap();
        for device in devices {
            println!("Found device {:#?}", device);
        }
    }
}
