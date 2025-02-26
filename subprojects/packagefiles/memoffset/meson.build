# Copyright © 2024 Google
# SPDX-License-Identifier: MIT

project(
  'memoffset',
  'rust',
  version : '0.9.0',
  license :'MIT OR Apache-2.0',
)

lib = static_library(
  'memoffset',
  'src/lib.rs',
  override_options : ['rust_std=2021', 'build.rust_std=2021'],
  rust_abi : 'rust',
  native : true,
)

dep_memoffset = declare_dependency(
  link_with : [lib]
)
