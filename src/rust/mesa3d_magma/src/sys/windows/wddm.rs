// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use mesa3d_util::MesaResult;
use std::sync::Arc;

use crate::magma::MagmaPhysicalDevice;
use crate::magma_defines::MagmaCreateBufferInfo;
use crate::magma_defines::MagmaHeap;
use crate::magma_defines::MagmaHeapBudget;
use crate::magma_defines::MagmaMemoryProperties;
use crate::magma_defines::MagmaPciBusInfo;
use crate::magma_defines::MagmaPciInfo;
use crate::sys::windows::d3dkmt_common;

pub trait VendorPrivateData {
    fn createallocation_pdata(&self) -> Vec<u32> {
        Vec::new()
    }

    fn allocationinfo2_pdata(
        &self,
        create_info: &MagmaCreateBufferInfo,
        mem_props: &MagmaMemoryProperties,
    ) -> Vec<u32> {
        Vec::new()
    }
}

pub fn enumerate_devices() -> MesaResult<Vec<MagmaPhysicalDevice>> {
    let mut devices: Vec<MagmaPhysicalDevice> = Vec::new();
    let adapters = d3dkmt_common::enumerate_adapters()?;

    for adapter in adapters {
        let pci_info = adapter.pci_info();
        let pci_bus_info = adapter.pci_bus_info();

        devices.push(MagmaPhysicalDevice::new(
            Arc::new(adapter),
            pci_info,
            pci_bus_info,
        ));
    }

    Ok(devices)
}
