// Copyright 2020 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::ffi::CString;

use crate::sys::platform::SharedMemory as SysUtilSharedMemory;
use crate::AsRawDescriptor;
use crate::FromRawDescriptor;
use crate::IntoRawDescriptor;
use crate::MesaResult;
use crate::OwnedDescriptor;
use crate::RawDescriptor;

pub struct SharedMemory(pub(crate) SysUtilSharedMemory);
impl SharedMemory {
    /// Creates a new shared memory object of the given size.
    ///
    /// |name| is purely for debugging purposes. It does not need to be unique, and it does
    /// not affect any non-debugging related properties of the constructed shared memory.
    pub fn new<T: Into<Vec<u8>>>(debug_name: T, size: u64) -> MesaResult<SharedMemory> {
        let debug_name = CString::new(debug_name)?;
        SysUtilSharedMemory::new(&debug_name, size).map(SharedMemory)
    }

    pub fn size(&self) -> u64 {
        self.0.size()
    }
}

impl AsRawDescriptor for SharedMemory {
    fn as_raw_descriptor(&self) -> RawDescriptor {
        self.0.as_raw_descriptor()
    }
}

impl IntoRawDescriptor for SharedMemory {
    fn into_raw_descriptor(self) -> RawDescriptor {
        self.0.into_raw_descriptor()
    }
}

impl From<SharedMemory> for OwnedDescriptor {
    fn from(sm: SharedMemory) -> OwnedDescriptor {
        // SAFETY:
        // Safe because we own the SharedMemory at this point.
        unsafe { OwnedDescriptor::from_raw_descriptor(sm.into_raw_descriptor()) }
    }
}
