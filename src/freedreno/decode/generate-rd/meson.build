fs = import('fs')

if dep_libarchive.found()
  libgenerate_rd_resources = static_library(
    'generate_rd_resources',
    [
    ],
    include_directories: [
      inc_freedreno,
      inc_freedreno_rnn,
      inc_include,
      inc_src,
    ],
    c_args : [ no_override_init_args ],
    gnu_symbol_visibility: 'hidden',
    dependencies : [
      idep_nir,
    ],
    link_with: [
      libfreedreno_cffdec,
    ],
    build_by_default: false,
  )
endif
