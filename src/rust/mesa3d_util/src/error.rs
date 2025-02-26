// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::ffi::NulError;
use std::io::Error as IoError;
use std::num::TryFromIntError;
use std::str::Utf8Error;
use thiserror::Error;

#[cfg(any(target_os = "android", target_os = "linux"))]
use nix::Error as NixError;

#[cfg(any(target_os = "windows"))]
use winapi::shared::ntdef::NTSTATUS;

use remain::sorted;

/// An error generated while using this crate.
#[sorted]
#[derive(Error, Debug)]
pub enum MesaError {
    /// An error with the MesaHandle
    #[error("invalid Mesa handle")]
    InvalidMesaHandle,
    /// An input/output error occured.
    #[error("an input/output error occur: {0}")]
    IoError(IoError),
    /// Nix crate error.
    #[cfg(any(target_os = "android", target_os = "linux"))]
    #[error("The errno is {0}")]
    NixError(NixError),
    #[cfg(any(target_os = "windows"))]
    #[error("Failed with NTSTATUS 0x{0:x}")]
    NtStatus(NTSTATUS),
    #[error("Nul Error occured {0}")]
    NulError(NulError),
    /// Violation of the Rutabaga spec occured.
    #[error("violation of the rutabaga spec: {0}")]
    SpecViolation(&'static str),
    /// An attempted integer conversion failed.
    #[error("int conversion failed: {0}")]
    TryFromIntError(TryFromIntError),
    /// The command is unsupported.
    #[error("the requested function is not implemented")]
    Unsupported,
    /// Utf8 error.
    #[error("an utf8 error occured: {0}")]
    Utf8Error(Utf8Error),
}

#[cfg(any(target_os = "android", target_os = "linux"))]
impl From<NixError> for MesaError {
    fn from(e: NixError) -> MesaError {
        MesaError::NixError(e)
    }
}

impl From<NulError> for MesaError {
    fn from(e: NulError) -> MesaError {
        MesaError::NulError(e)
    }
}

impl From<IoError> for MesaError {
    fn from(e: IoError) -> MesaError {
        MesaError::IoError(e)
    }
}

impl From<TryFromIntError> for MesaError {
    fn from(e: TryFromIntError) -> MesaError {
        MesaError::TryFromIntError(e)
    }
}

impl From<Utf8Error> for MesaError {
    fn from(e: Utf8Error) -> MesaError {
        MesaError::Utf8Error(e)
    }
}

/// The result of an operation in this crate.
pub type MesaResult<T> = std::result::Result<T, MesaError>;
