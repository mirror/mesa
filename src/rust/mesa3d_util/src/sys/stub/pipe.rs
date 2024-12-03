// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::AsBorrowedDescriptor;
use crate::AsRawDescriptor;
use crate::MesaError;
use crate::MesaResult;
use crate::OwnedDescriptor;
use crate::RawDescriptor;

pub struct ReadPipeStub(());
pub struct WritePipeStub(());

pub type ReadPipe = ReadPipeStub;
pub type WritePipe = WritePipeStub;

pub fn create_pipe() -> MesaResult<(ReadPipe, WritePipe)> {
    Err(MesaError::Unsupported)
}

impl ReadPipe {
    pub fn read(&self, _data: &mut [u8]) -> MesaResult<usize> {
        Err(MesaError::Unsupported)
    }
}

impl AsBorrowedDescriptor for ReadPipe {
    fn as_borrowed_descriptor(&self) -> &OwnedDescriptor {
        unimplemented!()
    }
}

impl WritePipe {
    pub fn new(_descriptor: RawDescriptor) -> WritePipe {
        unimplemented!()
    }

    pub fn write(&self, _data: &[u8]) -> MesaResult<usize> {
        Err(MesaError::Unsupported)
    }
}

impl AsBorrowedDescriptor for WritePipe {
    fn as_borrowed_descriptor(&self) -> &OwnedDescriptor {
        unimplemented!()
    }
}

impl AsRawDescriptor for WritePipe {
    fn as_raw_descriptor(&self) -> RawDescriptor {
        unimplemented!()
    }
}
