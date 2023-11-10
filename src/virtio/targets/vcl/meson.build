# Copyright © 2023-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
# SPDX-License-Identifier: MIT

opencl_version = '1'

vcl_ld_args = []
vcl_ld_depends = []

if with_ld_version_script
  vcl_ld_args += ['-Wl,--version-script', join_paths(meson.current_source_dir(), 'vcl.sym')]
  vcl_ld_depends += files('vcl.sym')
endif

libvcl = shared_library(
  'VclOpenCL',
  'target.c',
  gnu_symbol_visibility : 'hidden',
  include_directories : [
    inc_include,
    inc_src,
    inc_gallium,
    inc_gallium_aux,
  ],
  link_whole : libvcl,
  link_with : [],
  dependencies : [
    idep_mesautil,
  ],
  link_args : [
    ld_args_build_id,
    ld_args_gc_sections,
    vcl_ld_args,
  ],
  link_depends : vcl_ld_depends,
  version : '@0@.0.0'.format(opencl_version),
  install : true,
)

_config = configuration_data()
_config.set('OPENCL_VERSION', opencl_version)
configure_file(
  configuration : _config,
  input : 'vcl.icd.in',
  output : 'vcl.icd',
  install : true,
  install_tag : 'runtime',
  install_dir : join_paths(get_option('sysconfdir'), 'OpenCL', 'vendors'),
)

# .so is hardcoded in the icd as well
devenv.prepend(
  'OCL_ICD_VENDORS',
  meson.current_build_dir() / ''
)
