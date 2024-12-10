// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use mesa3d_util::MesaError;
use remain::sorted;
use thiserror::Error;
use zerocopy::FromBytes;
use zerocopy::IntoBytes;

/// An error type based on magma_common_defs.h
#[sorted]
#[derive(Error, Debug)]
pub enum MagmaError {
    #[error("Access Denied")]
    AccessDenied,
    #[error("Bad State")]
    BadState,
    #[error("Connection Lost")]
    ConnectionLost,
    #[error("Context Killed")]
    ContextKilled,
    #[error("Internal Error")]
    InternalError,
    #[error("Invalid Arguments")]
    InvalidArgs,
    #[error("Memory Error")]
    MemoryError,
    #[error("A Mesa error was returned {0}")]
    MesaError(MesaError),
    #[error("Timed out")]
    TimedOut,
    #[error("Unimplemented")]
    Unimplemented,
}

impl From<MesaError> for MagmaError {
    fn from(e: MesaError) -> MagmaError {
        MagmaError::MesaError(e)
    }
}

pub type MagmaResult<T> = std::result::Result<T, MagmaError>;

#[repr(C)]
#[derive(Clone, Default, Debug, IntoBytes, FromBytes)]
pub struct MagmaPciInfo {
    pub vendor_id: u16,
    pub device_id: u16,
    pub subvendor_id: u16,
    pub subdevice_id: u16,
    pub revision_id: u8,
    pub padding: [u8; 7],
}

#[repr(C)]
#[derive(Clone, Default, Debug, IntoBytes, FromBytes)]
pub struct MagmaPciBusInfo {
    pub domain: u16,
    pub bus: u8,
    pub device: u8,
    pub function: u8,
    pub padding: [u8; 7],
}

pub const MAGMA_HEAP_DEVICE_LOCAL_BIT: u64 = 0x00000001;
#[repr(C)]
#[derive(Clone, Default, Debug, IntoBytes, FromBytes)]
pub struct MagmaHeap {
    pub heap_size: u64,
    pub heap_flags: u64,
}

impl MagmaHeap {
    pub(crate) fn is_device_local(&self) -> bool {
        self.heap_flags & MAGMA_HEAP_DEVICE_LOCAL_BIT != 0
    }
}

pub const MAGMA_MEMORY_PROPERTY_DEVICE_LOCAL_BIT: u32 = 0x00000001;
pub const MAGMA_MEMORY_PROPERTY_HOST_VISIBLE_BIT: u32 = 0x00000002;
pub const MAGMA_MEMORY_PROPERTY_HOST_COHERENT_BIT: u32 = 0x00000004;
pub const MAGMA_MEMORY_PROPERTY_HOST_CACHED_BIT: u32 = 0x00000008;
pub const MAGMA_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT: u32 = 0x00000010;
pub const MAGMA_MEMORY_PROPERTY_PROTECTED_BIT: u32 = 0x00000020;
#[repr(C)]
#[derive(Clone, Default, Debug, IntoBytes, FromBytes)]
pub struct MagmaMemoryType {
    pub property_flags: u32,
    pub heap_idx: u32,
}

impl MagmaMemoryType {
    pub(crate) fn is_device_local(&self) -> bool {
        self.property_flags & MAGMA_MEMORY_PROPERTY_HOST_COHERENT_BIT != 0
    }

    pub(crate) fn is_coherent(&self) -> bool {
        self.property_flags & MAGMA_MEMORY_PROPERTY_HOST_COHERENT_BIT != 0
    }

    pub(crate) fn is_cached(&self) -> bool {
        self.property_flags & MAGMA_MEMORY_PROPERTY_HOST_CACHED_BIT != 0
    }

    pub(crate) fn is_protected(&self) -> bool {
        self.property_flags & MAGMA_MEMORY_PROPERTY_PROTECTED_BIT != 0
    }
}

pub const MAGMA_MAX_MEMORY_TYPES: usize = 32;
pub const MAGMA_MAX_MEMORY_HEAPS: usize = 16;
#[repr(C)]
#[derive(Clone, Default, Debug, IntoBytes, FromBytes)]
pub struct MagmaMemoryProperties {
    pub memory_type_count: u32,
    pub memory_heap_count: u32,
    pub memory_types: [MagmaMemoryType; MAGMA_MAX_MEMORY_TYPES],
    pub memory_heaps: [MagmaHeap; MAGMA_MAX_MEMORY_HEAPS],
}

impl MagmaMemoryProperties {
    pub(crate) fn increment_heap_count(&mut self) {
        self.memory_heap_count += 1;
    }

    pub(crate) fn add_heap(&mut self, heap_size: u64, heap_flags: u64) {
        self.memory_heaps[self.memory_heap_count as usize].heap_size = heap_size;
        self.memory_heaps[self.memory_heap_count as usize].heap_flags = heap_flags;
    }

    pub(crate) fn add_memory_type(&mut self, property_flags: u32) {
        self.memory_types[self.memory_type_count as usize].property_flags = property_flags;
        self.memory_types[self.memory_type_count as usize].heap_idx = self.memory_heap_count;
        self.memory_type_count += 1;
    }

    pub(crate) fn get_memory_heap(&self, heap_idx: u32) -> &MagmaHeap {
        &self.memory_heaps[heap_idx as usize]
    }

    pub(crate) fn get_memory_type(&self, memory_type_idx: u32) -> &MagmaMemoryType {
        &self.memory_types[memory_type_idx as usize]
    }
}

#[repr(C)]
#[derive(Clone, Default, Debug, IntoBytes, FromBytes)]
pub struct MagmaHeapBudget {
    pub budget: u64,
    pub usage: u64,
}

// Acceptable buffer vendor flags if the vendor is AMD:
//  - MAGMA_BUFFER_FLAG_AMD_FLAG_OA: Ordered append, used by 3D/Compute engines
//  - MAGMA_BUFFER_FLAG_AMD_FLAG_GDS: Global on-chip data storage. Used to share
//                                    data across shader threads.
pub const MAGMA_BUFFER_FLAG_AMD_OA: u32 = 0x000000001;
pub const MAGMA_BUFFER_FLAG_AMD_GDS: u32 = 0x000000002;

#[repr(C)]
#[derive(Clone, Default, Debug, IntoBytes, FromBytes)]
pub struct MagmaCreateBufferInfo {
    pub memory_type_idx: u32,
    pub alignment: u32,
    pub common_flags: u32,
    pub vendor_flags: u32,
    pub size: u64,
}

// Same as PCI id
pub const MAGMA_VENDOR_ID_INTEL: u16 = 0x8086;
pub const MAGMA_VENDOR_ID_AMD: u16 = 0x1002;
pub const MAGMA_VENDOR_ID_MALI: u16 = 0x13B5;
pub const MAGMA_VENDOR_ID_QCOM: u16 = 0x5413;
