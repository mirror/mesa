# Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
# SPDX-License-Identifier: MIT

rust = import('rust')

cl_c_args = ['-DCL_TARGET_OPENCL_VERSION=300']

vcl_bindgen_args = [
  # needed because bindgen adds *mut void fields...
  '--raw-line', 'unsafe impl std::marker::Sync for _cl_icd_dispatch {}',
  # avoids warning such as: `extern` block uses type `u128`, which is not FFI-safe
  '--allowlist-type', 'cl_.*',
  # brings in CL_SUCCESS, CL_INVALID_VALUE, and so on..
  '--allowlist-var', 'CL_.*',
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

vcl_proc_macros_files = files(
  '../../gallium/frontends/rusticl/proc/lib.rs',
)

vcl_proc_macros = shared_library(
  'vcl_proc_macros',
  [vcl_proc_macros_files],
  rust_crate_type : 'proc-macro',
)

libmesa_rust_util_files = files(
  '../../gallium/frontends/rusticl/util/lib.rs',
  '../../gallium/frontends/rusticl/util/assert.rs',
  '../../gallium/frontends/rusticl/util/bitset.rs',
  '../../gallium/frontends/rusticl/util/feature.rs',
  '../../gallium/frontends/rusticl/util/properties.rs',
  '../../gallium/frontends/rusticl/util/ptr.rs',
  '../../gallium/frontends/rusticl/util/string.rs',
)

libmesa_rust_util = static_library(
  'mesa_rust_util',
  [libmesa_rust_util_files],
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'rlib',
)

vcl_drm_bindings_rs = rust.bindgen(
  input : 'vcl_drm_bindings.h',
  output : 'vcl_drm_bindings.rs',
  dependencies : [dep_libdrm],
  include_directories : [inc_include],
)

vcl_drm_gen = static_library(
  'vcl_drm_gen',
  vcl_drm_bindings_rs,
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'rlib',
  rust_args : [vcl_gen_args],
  dependencies : [dep_libdrm],
)

vcl_sys_bindings_c = meson.current_build_dir() / 'vcl_sys_bindings.c'

vcl_sys_bindings_rs = rust.bindgen(
  input : 'vcl_sys_bindings.h',
  output : 'vcl_sys_bindings.rs',
  include_directories: [inc_include],
  args : [
    '--experimental',
    '--wrap-static-fns',
    '--wrap-static-fns-path',
    vcl_sys_bindings_c,
  ],
)

# Fix include path
vcl_sys_static_c = custom_target('vcl_sys_static_gen',
  output: 'vcl_sys_bindings.c',
  input: vcl_sys_bindings_rs,
  command: [
    'sed',
    '-i',
    's,../../src/virtio/vcl/,,g',
    vcl_sys_bindings_c,
  ]
)

vcl_sys_static_gen = static_library(
  'vcl_sys_static_gen',
  sources: vcl_sys_static_c,
  c_args: ['-Wno-missing-prototypes']
)

vcl_sys_gen = static_library(
  'vcl_sys_gen',
  sources: vcl_sys_bindings_rs,
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'rlib',
  rust_args : [vcl_gen_args],
  link_with : vcl_sys_static_gen,
)


virglrenderer_c_args = ['-DVIRGL_RENDERER_UNSTABLE_APIS']

vcl_virglrenderer_bindings_rs = rust.bindgen(
  input : 'vcl_virglrenderer_bindings.h',
  output : 'vcl_virglrenderer_bindings.rs',
  include_directories : inc_virtio,
  c_args : [virglrenderer_c_args],
)

vcl_virglrenderer_gen = static_library(
  'vcl_virglrenderer_gen',
  vcl_virglrenderer_bindings_rs,
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'rlib',
  rust_args : [vcl_gen_args],
)

vcl_vtest_bindings_rs = rust.bindgen(
  input : 'vcl_vtest_bindings.h',
  output : 'vcl_vtest_bindings.rs',
  include_directories : inc_virtio,
  c_args : [virglrenderer_c_args],
)

vcl_vtest_gen = static_library(
  'vcl_vtest_gen',
  vcl_vtest_bindings_rs,
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'rlib',
  rust_args : [vcl_gen_args]
)

vcl_files = files(
  'lib.rs',
  'api/context.rs',
  'api/device.rs',
  'api/event.rs',
  'api/icd.rs',
  'api/platform.rs',
  'api/types.rs',
  'api/util.rs',
  'api.rs',
  'core/context.rs',
  'core/device.rs',
  'core/event.rs',
  'core/platform.rs',
  'core.rs',
  'dev/debug.rs',
  'dev/drm.rs',
  'dev/renderer.rs',
  'dev/virtgpu.rs',
  'dev/vtest.rs',
  'dev.rs',
  'protocol/context.rs',
  'protocol/cs_impl.rs',
  'protocol/cs.rs',
  'protocol/defines.rs',
  'protocol/device.rs',
  'protocol/event.rs',
  'protocol/handles.rs',
  'protocol/info.rs',
  'protocol/platform.rs',
  'protocol/structs.rs',
  'protocol/transport.rs',
  'protocol/types.rs',
  'protocol/util.rs',
  'protocol.rs',
)

libvcl = static_library(
  'vcl',
  [vcl_files],
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'staticlib',
  link_with : [
    vcl_opencl_gen,
    vcl_proc_macros,
    libmesa_rust_util,
    vcl_drm_gen,
    vcl_sys_gen,
    vcl_virglrenderer_gen,
    vcl_vtest_gen,
  ]
)

rust.test('vcl-unit-tests', libvcl)
