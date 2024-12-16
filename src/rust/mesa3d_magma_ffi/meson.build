# Copyright © 2024 Google
# SPDX-License-Identifier: MIT

libmesa_magma = static_library(
  'mesa3d_magma_ffi',
  'src/lib.rs',
  gnu_symbol_visibility : 'hidden',
  rust_abi : 'c',
  link_with: [libmesa_magma, libmesa_rust_util],
  dependencies: [dep_mesa3d_util, dep_log]
)
