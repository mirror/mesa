# Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
# SPDX-License-Identifier: MIT

rust = import('unstable-rust')

cl_c_args = ['-DCL_TARGET_OPENCL_VERSION=100']

vcl_bindgen_args = [
  # needed because bindgen adds *mut void fields...
  '--raw-line', 'unsafe impl std::marker::Sync for _cl_icd_dispatch {}',
  # avoids warning such as: `extern` block uses type `u128`, which is not FFI-safe
  '--allowlist-type', 'cl_.*',
]

vcl_opencl_bindings_rs = rust.bindgen(
  input : [
    'vcl_opencl_bindings.h',
    opencl_headers,
  ],
  output : 'vcl_opencl_bindings.rs',
  include_directories : [inc_include],
  c_args : [cl_c_args],
  args : [vcl_bindgen_args],
)

vcl_gen_args = [
  '-Anon_camel_case_types',
  '-Anon_snake_case',
  '-Anon_upper_case_globals',
]

vcl_opencl_gen = static_library(
  'vcl_opencl_gen',
  vcl_opencl_bindings_rs,
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'rlib',
  rust_args : [vcl_gen_args],
)

vcl_files = files(
  'lib.rs',
  'api.rs',
  'api/icd.rs'
)

libvcl = static_library(
  'vcl',
  [vcl_files],
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'staticlib',
  link_with : [vcl_opencl_gen]
)
