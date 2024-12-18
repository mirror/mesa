# Copyright © 2023 Valve Corporation
#
# SPDX-License-Identifier: MIT

dgc_include_dir = dir_source_root + '/src/freedreno/vulkan/dgc'

dgc_includes = files(
  'tu_dgc.h',
)

preprocess_spv = custom_target(
    'preprocess.spv.h',
    input : 'preprocess.comp',
    output : 'preprocess.spv.h',
    command : [
      prog_glslang, '-V', '-I' + dgc_include_dir, '--target-env', 'spirv1.5',
      '-x', '-o', '@OUTPUT@', '@INPUT@', glslang_depfile, glslang_quiet,
    ],
    depfile : f'preprocess.d',
    depend_files: dgc_includes
  )
