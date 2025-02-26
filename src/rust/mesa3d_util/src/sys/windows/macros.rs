// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[allow(unused_imports)]
use crate::MesaError;
#[allow(unused_imports)]
use winapi::shared::ntdef::NTSTATUS;

#[macro_export]
macro_rules! check_ntstatus {
    ($x: expr) => {{
        match $x {
            STATUS_SUCCESS => Ok(()),
            e => Err(MesaError::NtStatus(e)),
        }
    }};
}

#[macro_export]
macro_rules! check_ntstatus_maybe_pending {
    ($x: expr) => {{
        match $x {
            STATUS_SUCCESS => Ok(STATUS_SUCCESS),
            STATUS_PENDING => Ok(STATUS_PENDING),
            e => Err(MesaError::NtStatus(e)),
        }
    }};
}

#[macro_export]
macro_rules! log_ntstatus {
    ($x: expr) => {{
        match $x {
            STATUS_SUCCESS => (),
            e => error!("error status returned"),
        }
    }};
}
