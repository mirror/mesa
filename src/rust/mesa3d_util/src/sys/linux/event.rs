// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::convert::From;
use std::convert::TryFrom;
use std::os::fd::OwnedFd;

use nix::sys::eventfd::EfdFlags;
use nix::sys::eventfd::EventFd;
use nix::unistd::read;
use nix::unistd::write;

use crate::AsBorrowedDescriptor;
use crate::AsRawDescriptor;
use crate::MesaError;
use crate::MesaHandle;
use crate::MesaResult;
use crate::OwnedDescriptor;
use crate::MESA_FENCE_HANDLE_TYPE_EVENT_FD;

pub struct Event {
    descriptor: OwnedDescriptor,
}

impl Event {
    pub fn new() -> MesaResult<Event> {
        let owned: OwnedFd = EventFd::from_flags(EfdFlags::empty())?.into();
        Ok(Event {
            descriptor: owned.into(),
        })
    }

    pub fn signal(&mut self) -> MesaResult<()> {
        let _ = write(&self.descriptor, &1u64.to_ne_bytes())?;
        Ok(())
    }

    pub fn wait(&self) -> MesaResult<()> {
        read(self.descriptor.as_raw_descriptor(), &mut 1u64.to_ne_bytes())?;
        Ok(())
    }

    pub fn try_clone(&self) -> MesaResult<Event> {
        let clone = self.descriptor.try_clone()?;
        Ok(Event { descriptor: clone })
    }
}

impl TryFrom<MesaHandle> for Event {
    type Error = MesaError;
    fn try_from(handle: MesaHandle) -> Result<Self, Self::Error> {
        if handle.handle_type != MESA_FENCE_HANDLE_TYPE_EVENT_FD {
            return Err(MesaError::InvalidMesaHandle);
        }

        Ok(Event {
            descriptor: handle.os_handle,
        })
    }
}

impl From<Event> for MesaHandle {
    fn from(evt: Event) -> Self {
        MesaHandle {
            os_handle: evt.descriptor,
            handle_type: MESA_FENCE_HANDLE_TYPE_EVENT_FD,
        }
    }
}

impl AsBorrowedDescriptor for Event {
    fn as_borrowed_descriptor(&self) -> &OwnedDescriptor {
        &self.descriptor
    }
}
