// Copyright 2024 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
#![allow(clippy::all)]
#![allow(dead_code)]
#![allow(non_camel_case_types)]

#[cfg(not(use_meson))]
include!(concat!(env!("OUT_DIR"), "/msm_bindings.rs"));

#[cfg(use_meson)]
pub use msm_bindings::*;
