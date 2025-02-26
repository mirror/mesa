// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::path::Path;

use crate::AsBorrowedDescriptor;
use crate::MesaError;
use crate::MesaResult;
use crate::OwnedDescriptor;
use crate::RawDescriptor;
use crate::TubeType;

pub struct Stub(());
pub type Tube = Stub;
pub type Listener = Stub;

impl Tube {
    pub fn new<P: AsRef<Path>>(_path: P, _kind: TubeType) -> MesaResult<Tube> {
        Err(MesaError::Unsupported)
    }

    pub fn send(&self, _opaque_data: &[u8], _descriptors: &[RawDescriptor]) -> MesaResult<usize> {
        Err(MesaError::Unsupported)
    }

    pub fn receive(&self, _opaque_data: &mut [u8]) -> MesaResult<(usize, Vec<OwnedDescriptor>)> {
        Err(MesaError::Unsupported)
    }
}

impl AsBorrowedDescriptor for Tube {
    fn as_borrowed_descriptor(&self) -> &OwnedDescriptor {
        unimplemented!()
    }
}

impl Listener {
    /// Creates a new `Listener` bound to the given path.
    pub fn bind<P: AsRef<Path>>(_path: P) -> MesaResult<Listener> {
        Err(MesaError::Unsupported)
    }

    pub fn accept(&self) -> MesaResult<Tube> {
        Err(MesaError::Unsupported)
    }
}
