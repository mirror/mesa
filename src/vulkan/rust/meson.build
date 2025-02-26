# Copyright © 2022 Collabora, Ltd.
# SPDX-License-Identifier: MIT

add_languages('rust', required: true)
rust = import('unstable-rust')

assert(
  meson.version().version_compare('>= 1.0.0'),
  'Meson 1.0 is required for Rust Vulkan bindings'
)

_vulkan_h_rs = rust.bindgen(
  input : ['vulkan_h_bindgen.h'],
  output : 'vulkan_h.rs',
  include_directories : [inc_include, inc_src],
  c_args : [
    pre_args,
  ],
  args : [
    '--allowlist-type', 'PFN_vk.*',
    '--allowlist-type', 'Vk.*',
    '--allowlist-var', 'VK_.*',
    '--no-prepend-enum-name',
    '--raw-line', '#![allow(non_camel_case_types)]',
    '--raw-line', '#![allow(non_snake_case)]',
    '--raw-line', '#![allow(non_upper_case_globals)]',
    '--size_t-is-usize',
  ],
)

libvulkan_h_rs = static_library(
  'vulkan_h',
  _vulkan_h_rs,
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'rlib',
)

_vulkan_runtime_rs = rust.bindgen(
  input : ['vulkan_runtime_bindgen.h'],
  output : 'vulkan_runtime.rs',
  c_args : [
    pre_args,
  ],
  args : [
    '--allowlist-function', 'vk_.*',
    '--allowlist-type', 'vk_.*',
    '--allowlist-var', 'vk_.*',
    '--blocklist-type', 'PFN_vk.*',
    '--blocklist-type', 'Vk.*',
    '--no-prepend-enum-name',
    '--raw-line', '#![allow(non_camel_case_types)]',
    '--raw-line', '#![allow(non_snake_case)]',
    '--raw-line', '#![allow(non_upper_case_globals)]',
    '--raw-line', 'extern crate vulkan_h;',
    '--raw-line', 'use vulkan_h::*;',
    '--size_t-is-usize',
  ],
  dependencies : [
    idep_mesautil,
    idep_vulkan_runtime_headers,
    idep_vulkan_runtime,
    idep_vulkan_util_headers,
  ],
)

libvulkan_runtime_rs = static_library(
  'vulkan_runtime',
  _vulkan_runtime_rs,
  gnu_symbol_visibility : 'hidden',
  link_with : [libvulkan_h_rs],
  rust_crate_type : 'rlib',
)

libvk_rs = static_library(
  'vk_rs',
  'vk.rs',
  gnu_symbol_visibility : 'hidden',
  rust_crate_type : 'rlib',
  link_with : [
    libvulkan_h_rs,
    libvulkan_runtime_rs,
  ]
)
