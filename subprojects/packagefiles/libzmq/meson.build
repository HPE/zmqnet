project('libzmq', 'c', meson_version : '>=0.58.0')

autoreconf = find_program('autoreconf')
make = find_program('make')

run_command(
  autoreconf,
  '--force',
  '--install',
  '--verbose',
  check: true,
)

# cppzmq is configured as a CMake subproject, which runs (and needs libzmq's
# pkg-config file to already exist) at meson-configure time, not build time.
# So configure/build/install libzmq into a private prefix synchronously here
# instead of using unstable-external_project (which only builds at `ninja` time).
#
# The private prefix must live outside the main build tree: meson's CMake
# subproject interpreter (used for cppzmq) rejects absolute include paths
# that fall under the top build dir but outside its own build subdir, with
# "... is not in the subpath of ... OR one path is relative and the other
# is absolute". Placing it as a sibling of the build dir avoids that.
private_prefix = meson.global_build_root() + '-libzmq-install'

run_command(
  meson.current_source_dir() / 'configure',
  '--prefix=' + private_prefix,
  '--program-prefix=libzmqnet_',
  '--with-libsodium=yes',
  check: true,
)

run_command(make, check: true)
run_command(make, 'install', check: true)

meson.add_install_script(
  'install-libzmq.sh',
  meson.current_source_dir(),
)

libzmq_dep = declare_dependency(
  compile_args: ['-I' + private_prefix / 'include'],
  link_args: ['-L' + private_prefix / 'lib', '-lzmq'],
  variables: {'prefix': private_prefix},
)
meson.override_dependency('libzmq', libzmq_dep)
