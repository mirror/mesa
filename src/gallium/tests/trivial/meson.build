# Copyright © 2018 Intel Corporation
# SPDX-License-Identifier: MIT

foreach t : ['tri', 'quad-tex']
  executable(
    t,
    '@0@.c'.format(t),
    include_directories : [
      inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux,
      inc_gallium_drivers, inc_gallium_winsys,
      include_directories('../../frontends/dri')
    ],
    link_with : [
      libgallium, libpipe_loader_static, libws_null, libwsw, libswdri, libswkmsdri
    ],
    dependencies : [
      idep_mesautil, driver_swrast, driver_r300, driver_r600, driver_radeonsi,
      driver_nouveau, driver_kmsro, driver_v3d, driver_vc4, driver_freedreno,
      driver_etnaviv, driver_tegra, driver_i915, driver_svga, driver_virgl,
      driver_panfrost, driver_iris, driver_lima, driver_zink, driver_d3d12,
      driver_asahi, driver_crocus
    ],
    install : false,
  )
endforeach
