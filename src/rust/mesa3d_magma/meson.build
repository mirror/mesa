# Copyright © 2024 Google
# SPDX-License-Identifier: MIT

magma_generated_libs = []

dep_log = dependency('log',
  version: '>= 0.4.22',
  fallback: ['log', 'dep_log'],
  required: true,
)

if host_machine.system() == 'linux' or host_machine.system() == 'android'
  drm_bindings = rust.bindgen(
    input : drm_h,
    output : 'drm_bindings.rs',
    include_directories : [inc_include],
    args : [
      '--with-derive-default',
      '--allowlist-var', 'DRM_.+',
      '--allowlist-var', 'drm_.+',
      '--allowlist-type', 'drm_.+',
      '--no-prepend-enum-name',
      '--no-doc-comments',
      '--no-layout-tests'
    ],
  )

  amdgpu_bindings = rust.bindgen(
    input : amdgpu_drm_h,
    output : 'amdgpu_bindings.rs',
    include_directories : [inc_include],
    args : [
      '--with-derive-default',
      '--allowlist-var', 'DRM_AMDGPU_.+',
      '--allowlist-var', 'AMDGPU_.+',
      '--allowlist-type', 'drm_amdgpu_.+',
      '--no-prepend-enum-name',
      '--no-doc-comments',
      '--no-layout-tests'
    ],
  )

  msm_bindings = rust.bindgen(
    input : msm_drm_h,
    output : 'msm_bindings.rs',
    include_directories : [inc_include],
    args : [
      '--with-derive-default',
      '--allowlist-var', 'DRM_MSM_.+',
      '--allowlist-var', 'MSM_.+',
      '--allowlist-type', 'drm_msm_.+',
      '--no-prepend-enum-name',
      '--no-doc-comments',
      '--no-layout-tests'
    ],
  )

  virtgpu_bindings = rust.bindgen(
    input : virtgpu_drm_h,
    output : 'virtgpu_bindings.rs',
    include_directories : [inc_include],
    args : [
      '--with-derive-default',
      '--allowlist-var', 'DRM_VIRTGPU_.+',
      '--allowlist-var', 'VIRTGPU_.+',
      '--allowlist-type', 'drm_virtgpu_.+',
      '--no-prepend-enum-name',
      '--no-doc-comments',
      '--no-layout-tests'
    ]
  )

  drm_bindings_gen = static_library(
    'drm_bindings',
    drm_bindings,
    gnu_symbol_visibility : 'hidden',
    rust_abi : 'rust',
  )

  amdgpu_bindings_gen = static_library(
    'amdgpu_bindings',
    amdgpu_bindings,
    gnu_symbol_visibility : 'hidden',
    rust_abi : 'rust',
  )

  msm_bindings_gen = static_library(
    'msm_bindings',
    msm_bindings,
    gnu_symbol_visibility : 'hidden',
    rust_abi : 'rust',
  )

  virtgpu_bindings_gen = static_library(
    'virtgpu_bindings',
    virtgpu_bindings,
    gnu_symbol_visibility : 'hidden',
    rust_abi : 'rust',
  )

  magma_generated_libs += [drm_bindings_gen, amdgpu_bindings_gen,
                           msm_bindings_gen, virtgpu_bindings_gen]
elif host_machine.system() == 'windows'
  inc_d3dkmt = include_directories('headers/windows')
  virtgpu_bindings = rust.bindgen(
    input : 'headers/windows/d3dkmt_wrapper.h',
    output : 'd3dkmt_bindings.rs',
    include_directories : [inc_include],
    args : [
      '--with-derive-default',
      '--no-prepend-enum-name',
      '--no-doc-comments',
      '--no-layout-tests'
    ],
  )

  virtgpu_bindings_gen = static_library(
    'virtgpu_bindings',
    virtgpu_bindings,
    gnu_symbol_visibility : 'hidden',
    rust_abi : 'rust',
  )
endif

magma_args = []
magma_args += ['--cfg', 'use_meson']

libmesa_magma = static_library(
  'mesa3d_magma',
  'src/lib.rs',
  gnu_symbol_visibility : 'hidden',
  rust_abi : 'rust',
  rust_args: magma_args,
  link_with: [magma_generated_libs, libmesa_rust_util, libmesa_virtio_gpu],
  dependencies: [dep_mesa3d_util, dep_log]
)
