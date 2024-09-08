# Copyright © 2021 Collabora
# SPDX-License-Identifier: MIT

coredumpdec = executable(
  'panfrostdump',
  files('panfrostdump.c'),
  c_args : [c_msvc_compat_args, no_override_init_args, compile_args_panfrost],
  gnu_symbol_visibility : 'hidden',
  include_directories : [inc_include, inc_src],
  dependencies: [libpanfrost_dep],
  build_by_default : true,
  install: true
)

panfrost_texfeatures = executable(
  'panfrost_texfeatures',
  files('panfrost_texfeatures.c'),
  c_args : [c_msvc_compat_args, no_override_init_args, compile_args_panfrost],
  gnu_symbol_visibility : 'hidden',
  include_directories : [inc_include, inc_src],
  dependencies: [libpanfrost_dep],
  build_by_default : true,
  install: true
)

_panthor_uapi_bindings = rust.bindgen(
  input : [files('../../../include/drm-uapi/panthor_drm.h')],
  output : 'panthor_drm_bindings.rs',
  c_args : [
    pre_args,
  ],
  args : [
    '--raw-line', '#![allow(non_camel_case_types)]',
    '--raw-line', '#![allow(non_snake_case)]',
    '--raw-line', '#![allow(non_upper_case_globals)]',
    '--no-prepend-enum-name',
  ],
  dependencies : [dep_libdrm],
)

_panthor_uapi_bindings_gen = static_library(
  'panthor_uapi_bindings',
  _panthor_uapi_bindings,
  gnu_symbol_visibility : 'hidden',
  rust_abi : 'rust',
)

_libpanfrost_bindings = rust.bindgen(
  input : [files('../../../src/panfrost/lib/genxml/decode.h')],
  output : 'libpanfrost_bindings.rs',
  c_args : [
    pre_args,
  ],
  args : [
    '--raw-line', '#![allow(non_camel_case_types)]',
    '--raw-line', '#![allow(non_snake_case)]',
    '--raw-line', '#![allow(non_upper_case_globals)]',
    '--allowlist-function', 'pan.*',
    '--no-prepend-enum-name',
  ],
  dependencies : [libpanfrost_dep],
)

_libpanfrost_bindings_gen = static_library(
  'libpanfrost_bindings',
  _libpanfrost_bindings,
  gnu_symbol_visibility : 'hidden',
  rust_abi : 'rust',
)


executable(
  'panthor_coredump',
  files('panthor_coredump/main.rs'),
  rust_crate_type: 'bin',
  install: true,
  link_with: [_panthor_uapi_bindings_gen, _libpanfrost_bindings_gen, libpanfrost_decode, libpanfrost_bifrost, libpanfrost_valhall_disasm, libpanfrost_midgard],
  rust_args: ['-C', 'default-linker-libraries'],
  dependencies: [idep_compiler_rs,],
)
