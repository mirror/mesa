# Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
# SPDX-License-Identifier: MIT

vcl_files = files(
  'lib.rs'
)

libvcl = static_library(
  'vcl',
  [vcl_files],
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'staticlib'
)
