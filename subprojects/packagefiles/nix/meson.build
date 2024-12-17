# Copyright © 2024 Google
# SPDX-License-Identifier: MIT

project(
  'nix',
  'rust',
  version : '0.28.0',
  license :'MIT',
)

memoffset = subproject('memoffset').get_variable('lib')
libc = subproject('libc').get_variable('lib')
cfg_if = subproject('cfg-if').get_variable('lib')
bitflags = subproject('bitflags').get_variable('lib')

nix_args = []
if host_machine.system() == 'linux' or host_machine.system() == 'android'
  nix_args += [
  '--cfg', 'linux_android',
  '--cfg', 'feature="event"',
  '--cfg', 'feature="feature"',
  '--cfg', 'feature="fs"',
  '--cfg', 'feature="mman"',
  '--cfg', 'feature="socket"',
  '--cfg', 'feature="uio"',
  '--cfg', 'feature="ioctl"',
  '--cfg', 'feature="dir"',
  '--cap-lints', 'warn',
]
endif

lib = static_library(
  'nix',
  'src/lib.rs',
  override_options : ['rust_std=2021', 'build.rust_std=2021'],
  link_with : [memoffset, libc, cfg_if, bitflags],
  rust_abi : 'rust',
  native : true,
  rust_args: nix_args,
)

dep_nix = declare_dependency(
  link_with : [lib, memoffset, libc, cfg_if, bitflags]
)
