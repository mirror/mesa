// Copyright 2024 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! System definitions and bindings for interacting with D3DKMT.

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(unused_imports)]
#![allow(dead_code)]

include!(concat!(env!("OUT_DIR"), "/d3dkmt_bindings.rs"));
