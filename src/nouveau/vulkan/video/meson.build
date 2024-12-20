# Copyright © 2024 Collabora, Ltd.
# SPDX-License-Identifier: MIT

add_languages('rust', required: true)
rust = import('rust')

rc = meson.get_compiler('rust')

# XXX: our dependencies have to inherit from NVK's dependencies somehow.
libnvk_video_deps = [
  idep_mesautil,
  idep_nouveau_ws,
  idep_nvidia_headers,
  idep_vulkan_runtime,
  idep_vulkan_util,
  idep_vulkan_runtime_headers,
  idep_vulkan_util_headers,
  idep_vulkan_wsi,
  idep_vulkan_wsi_headers,
  idep_nil,
  idep_nir,
  idep_nir_headers,
  idep_nak,
]

# XXX: Same here, this should inherit from NVK in general, there's nothing extra
# in particular that is needed here.
libnvk_video_rust_args = [
  '-Aclippy::identity_op',
  '-Aclippy::len_zero',
  '-Aclippy::manual_range_contains',
  # normally this is a good one, but we use it where the "better" code is worse
  '-Aclippy::needless_range_loop',
  '-Aclippy::redundant_field_names',
  '-Aclippy::upper_case_acronyms',
  '-Aclippy::vec_box',
  '-Aclippy::write_with_newline',
  '-Anon_snake_case',
]

# XXX: ideally the .c wrapper file should be somewhere in /build instead of
# commited into the tree.
libnvk_video_bindings = rust.bindgen(
  input: ['video_bindings.h'],
  output: 'video_bindings.rs',
  c_args: [
    pre_args,
  ],
  include_directories: [inc_include, inc_src, include_directories('.')],
  args: [
    '--raw-line', '#![allow(non_camel_case_types)]',
    '--raw-line', '#![allow(non_snake_case)]',
    '--raw-line', '#![allow(non_upper_case_globals)]',
    '--emit-diagnostics',
    '--no-prepend-enum-name',
    '--experimental',
    '--wrap-static-fns',
    '--wrap-static-fns-path=/tmp/nvk_video_extern.c',
    # We need a suffix, otherwise the generated functions will conflict with
    # other libraries, like nil_rs.
    '--wrap-static-fns-suffix=_nvk_video',
    '--no-layout-tests',
    '--generate-inline-functions',
    '--with-derive-default',
    '--allowlist-type', 'Std.*|vk_video.*|.*nv.*|Vk.*|vk.*',
    '--allowlist-function', '.*vk_find.*|.*vk_video.*|nvk.*',
    '--allowlist-var', '.*SUBC.*',
  ],
  dependencies: [libnvk_video_deps],
)

libnvk_video_bindings_gen = static_library(
  'nvk_video_bindings',
  libnvk_video_bindings,
  gnu_symbol_visibility: 'hidden',
  rust_abi: 'rust',
)

dep_paste = dependency(
  'paste',
  version: '>= 1.0.14',
  fallback: ['paste', 'dep_paste'],
  required: true,
)

libnvk_video_rust_deps = [
  dep_paste,
  idep_nvidia_headers_rs,
  idep_nv_push_rs,
]

_libnvk_video = static_library(
  '_nvk_video',
  files('lib.rs'),
  gnu_symbol_visibility: 'hidden',
  rust_abi: 'c',
  rust_args: libnvk_video_rust_args,
  link_with: [
    libnvk_video_bindings_gen,
  ],
  dependencies: libnvk_video_rust_deps,
)

_libnvk_video = static_library(
  'nvk_video',
  [
    'extern.c',
    'video_bindings.h',
  ],
  include_directories: [
    inc_include,
    inc_src,
    inc_mapi,
    inc_mesa,
    inc_gallium,
    inc_util,
  ],
  dependencies: libnvk_video_deps,
  link_with: [
    _libnvk_video,
  ],
  c_args: [no_override_init_args + '-Wno-missing-prototypes'],
  gnu_symbol_visibility: 'hidden',
)

idep_nvk_video = declare_dependency(
  include_directories: include_directories('.'),
  link_with: _libnvk_video,
)