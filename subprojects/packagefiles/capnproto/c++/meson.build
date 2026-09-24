mod = import('unstable-external_project')

autoreconf = find_program('autoreconf')

# capnproto ships no configure script; setup-autotools.sh is a no-op stub that
# just tells you to run autoreconf.
run_command(
  autoreconf,
  '--force',
  '--install',
  '--verbose',
  meson.current_source_dir(),
  check: true,
)

p = mod.add_project('configure',
  configure_options: [
    '--prefix=@PREFIX@',
    '--libdir=@PREFIX@/@LIBDIR@',
    '--includedir=@PREFIX@/@INCLUDEDIR@',
    '--disable-static',
  ],
  cross_configure_options: ['--host=@HOST@'],
)

kj_dep = p.dependency('kj')
kj_async_dep = p.dependency('kj-async')
capnp_rpc_dep = p.dependency('capnp-rpc')

# external_project stages `make install` output under <builddir>/dist/<prefix>.
capnp_dist_dir = meson.current_build_dir() / 'dist' / get_option('prefix').strip('/')
capnp_prog_path = capnp_dist_dir / 'bin' / 'capnp'

# the staged binary is rpath'd to the final prefix, not to dist/
capnp_libdir_path = capnp_dist_dir / get_option('libdir')

# capnp.so links against kj.so but does not re-export its symbols, and
# consumers that use kj templates directly (e.g. kj/array.h) need -lkj too.
capnp_dep = declare_dependency(
  dependencies: [p.dependency('capnp'), kj_dep],
  variables: {
    'capnp': capnp_prog_path,
    'capnp_libdir': capnp_libdir_path,
  },
)

# p.dependency() smuggles the external project's stamp target in as `sources`,
# which only propagates to build targets -- a custom_target cannot consume it.
# This library exists purely to re-export that ordering edge as a real target.
capnp_build_stamp = static_library('capnp-build-stamp',
  'capnp-build-stamp.cpp',
  dependencies: capnp_dep,
)

meson.override_dependency('kj', kj_dep)
meson.override_dependency('capnp', capnp_dep)
meson.override_dependency('capnp-rpc', capnp_rpc_dep)

