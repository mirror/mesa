// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![allow(clippy::all)]
#![allow(non_upper_case_globals)]
#![allow(unused_imports)]
#![allow(dead_code)]
#![allow(non_camel_case_types)]

#[cfg(not(use_meson))]
include!(concat!(env!("OUT_DIR"), "/amdgpu_bindings.rs"));

#[cfg(use_meson)]
pub use amdgpu_bindings::*;
