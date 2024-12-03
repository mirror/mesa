// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::MesaError;
use crate::MesaResult;
use crate::OwnedDescriptor;
use crate::WaitEvent;
use crate::WaitTimeout;

pub struct Stub(());
pub type WaitContext = Stub;

impl WaitContext {
    pub fn new() -> MesaResult<WaitContext> {
        Err(MesaError::Unsupported)
    }

    pub fn add(&mut self, _connection_id: u64, _descriptor: &OwnedDescriptor) -> MesaResult<()> {
        Err(MesaError::Unsupported)
    }

    pub fn wait(&mut self, _timeout: WaitTimeout) -> MesaResult<Vec<WaitEvent>> {
        Err(MesaError::Unsupported)
    }

    pub fn delete(&mut self, _descriptor: &OwnedDescriptor) -> MesaResult<()> {
        Err(MesaError::Unsupported)
    }
}
