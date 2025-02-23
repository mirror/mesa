# Copyright 2019 Google, Inc
# SPDX-License-Identifier: MIT

rkt_registers = custom_target(
  'rkt_registers.h',
  input : ['gen_header.py', 'registers.xml'],
  output : 'rkt_registers.h',
  command : [prog_python, '@INPUT0@', '--rnn', '.', '--xml', '@INPUT1@', 'c-defines'],
  capture : true,
)

files_rkt = files(
  'rkt_device.c',
  'rkt_ml.c',
)

librkt = static_library(
  'rocket',
  [files_rkt, rkt_registers],
  include_directories : [inc_gallium_aux, inc_gallium, inc_include, inc_src],
  gnu_symbol_visibility : 'hidden',
  dependencies : [idep_mesautil, dep_libdrm],
)

driver_rkt = declare_dependency(
  compile_args : '-DGALLIUM_ROCKET',
  link_with : [librktwinsys, librkt]
)

shared_library('intercept',
   [files('intercept.c'), rkt_registers],
   include_directories : [inc_include],
   dependencies : [dep_libdrm],
   c_args: ['-Wno-error=missing-prototypes', '-g', '-O0']
)
