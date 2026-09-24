# Copyright 2020 Hewlett Packard Enterprise Development LP.
#
# Meson port of the autotools build described by configure.ac / Makefile.am.
#
# Build with gcc-toolset-14, e.g.:
#   scl enable gcc-toolset-14 -- meson setup build --prefix=<prefix>
#   scl enable gcc-toolset-14 -- ninja -C build

project('zmqnet', 'c', 'cpp',
  version: run_command(
    'bash', '-c',
    'source ' + meson.current_source_dir() / 'release_versioning'
    + ' && echo "$major.$minor"',
    check: true,
  ).stdout().strip(),
  meson_version: '>=0.58.0',
  license: 'HPE',
  default_options: [
    'cpp_std=c++17',
    'warning_level=1', # -Wall
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
prefix = get_option('prefix')

libdir = get_option('libdir')
if libdir.endswith('lib')
  libexecdir = libdir + 'exec'
  rel_rpath = '$ORIGIN/..' / libdir  
else  
  libexecdir = libdir.replace('lib/', 'libexec/')
  rel_rpath = '$ORIGIN/../..' / libdir
endif  

# gcc-toolset-14 provides GCC 14; warn if the build is not using it.
if cxx.get_id() == 'gcc' and cxx.version().version_compare('<14')
  warning('C++ compiler is GCC @0@; gcc-toolset-14 is expected'.format(cxx.version()))
endif

########################################
### Version information              ###
########################################
# libtool "current:revision:age" from release_versioning
version_vals = run_command(
    'bash', '-c',
    'source ' + meson.current_source_dir() / 'release_versioning'
    + ' && echo "$current.$revision.$age"',
    check: true,
  ).stdout().strip().split('.')

lt_current  = version_vals[0].to_int()
lt_revision = version_vals[1].to_int()
lt_age      = version_vals[2].to_int()

# libtool -version-info maps to soname .so.(current - age) and
# the full library version (current - age).age.revision
zmqnet_soversion = '@0@'.format(lt_current - lt_age)
zmqnet_libversion = '@0@.@1@.@2@'.format(lt_current - lt_age, lt_age, lt_revision)

########################################
### Build configuration              ###
########################################
# Equivalent of --enable-encryption (default: enabled).
enable_encryption = true

########################################
### Dependencies                     ###
########################################
libzmq_dep = dependency('libzmq', fallback : ['libzmq', 'libzmq_dep'], required: true)

capnproto_proj = subproject('capnproto')
capnp_dep = capnproto_proj.get_variable('capnp_dep')
capnp_build_stamp = capnproto_proj.get_variable('capnp_build_stamp')

common_tools_fe_dep = dependency('common_tools_fe')
common_tools_be_dep = dependency('common_tools_be')

dl_dep = cc.find_library('dl')
uuid_dep = cc.find_library('uuid')
threads_dep = dependency('threads')

#cppzmq_proj = subproject('cppzmq')
#cppzmq_dep = cppzmq_proj.get_variable('cppzmq_dep')

# resolves at configure time; forwards to the subproject's capnp at build time
capnp_prog = find_program('tools/capnp')
capnp_real = capnp_dep.get_variable('capnp')
capnp_libdir = capnp_dep.get_variable('capnp_libdir')

########################################
### config.h                         ###
########################################
conf = configuration_data()
conf.set_quoted('PACKAGE', meson.project_name())
conf.set_quoted('PACKAGE_NAME', meson.project_name())
conf.set_quoted('PACKAGE_TARNAME', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('PACKAGE_STRING', meson.project_name() + ' ' + meson.project_version())
conf.set_quoted('PACKAGE_BUGREPORT', '')
conf.set_quoted('PACKAGE_URL', '')
conf.set_quoted('VERSION', meson.project_version())
conf.set10('HAVE_ENCRYPTION', enable_encryption)
conf.set10('HAVE_LIBDL', dl_dep.found())

foreach h : ['dlfcn.h', 'inttypes.h', 'memory.h', 'stdint.h', 'stdlib.h',
             'strings.h', 'string.h', 'sys/stat.h', 'sys/types.h', 'unistd.h']
  conf.set10('HAVE_' + h.to_upper().underscorify(), cc.has_header(h))
endforeach

configure_file(output: 'config.h', configuration: conf)
add_project_arguments('-DHAVE_CONFIG_H', language: ['c', 'cpp'])

########################################
### Include directories              ###
########################################
inc_src = include_directories('src')
inc_public = include_directories('include')
inc_root = include_directories('.')

########################################
### src/lrle: libLrleBitset          ###
########################################
lrle_bitset_lib = library('LrleBitset',
  'src/lrle/lrle_bitset.cpp',
  include_directories: [inc_root, inc_src, inc_public],
  cpp_args: ['-fno-rtti', '-fno-exceptions'],
  pic: true,
  install: true,
)

libLrleBitset_dep = declare_dependency(
  link_with: lrle_bitset_lib,
  include_directories: [inc_src],
)
meson.override_dependency('libLrleBitset', libLrleBitset_dep)

install_headers('src/lrle/lrle_bitset.h')

pkgconfig = import('pkgconfig')
pkgconfig.generate(lrle_bitset_lib,
  filebase: 'libLrleBitset',
  name: 'libLrleBitset',
  description: 'libLrleBitset',
)

########################################
### subprojects/cppzmq               ###
########################################

cmake = import('cmake')
opts = cmake.subproject_options()
opts.set_install(true)
opts.add_cmake_defines({
  'CPPZMQ_BUILD_TESTS': false,
  'ENABLE_DRAFTS': true,
  'CMAKE_PREFIX_PATH': libzmq_dep.get_variable('prefix', default_value: ''),
})

cppzmq_proj = cmake.subproject('cppzmq', options: opts)
raw_cppzmq_dep = cppzmq_proj.dependency('cppzmq')

# Wrap the dependency to export -DZMQ_BUILD_DRAFT_API to all consumers
cppzmq_dep = declare_dependency(
  compile_args: ['-DZMQ_BUILD_DRAFT_API'],
  dependencies: [raw_cppzmq_dep],
)
meson.override_dependency('cppzmq', cppzmq_dep)

meson.add_install_script('tools/install-cppzmq-headers.sh')

########################################
### src/libzmqnet: libzmqnet         ###
########################################
network_schema = custom_target('NetworkSchema.capnp',
  input: 'src/libzmqnet/NetworkSchema.capnp',
  output: ['NetworkSchema.capnp.c++', 'NetworkSchema.capnp.h'],
  command: [
    capnp_prog, 'compile', '-oc++:@OUTDIR@',
    '--src-prefix=' + meson.current_source_dir() / 'src' / 'libzmqnet',
    '@INPUT@',
  ],
  env: {'CAPNP_REAL': capnp_real, 'CAPNP_LIBDIR': capnp_libdir},
  depends: [capnp_build_stamp],
)

zmqnet_lib = library('zmqnet',
  [
    'src/libzmqnet/Frontend.cpp',
    'src/libzmqnet/AsyncFrontend.cpp',
    'src/libzmqnet/Backend.cpp',
    network_schema,
  ],
  include_directories: [inc_root, inc_src, inc_public],
  cpp_args: ['-fno-rtti'],
  dependencies: [
    capnp_dep, libzmq_dep, cppzmq_dep, libLrleBitset_dep,
    common_tools_fe_dep, common_tools_be_dep, uuid_dep, dl_dep, threads_dep,
  ],
  install_rpath: '$ORIGIN' + ':' + common_tools_fe_dep.get_variable('libdir'),
  pic: true,
  version: zmqnet_libversion,
  soversion: zmqnet_soversion,
  install: true,
)

zmqnet_dep = declare_dependency(
  link_with: zmqnet_lib,
  include_directories: [inc_src, inc_public],
  sources: [network_schema[1]],
  variables: {
     'libdir' : prefix / get_option('libdir'),
     'capnp_path': meson.current_source_dir() / 'tools',
  },
)
meson.override_dependency('zmqnet', zmqnet_dep)

uuid_dep = cc.find_library('uuid', required: true)


########################################
### src: commnode                    ###
########################################
executable('commnode',
  'src/commnode.cpp',
  include_directories: [inc_root, inc_src, inc_public],
  cpp_args: ['-fno-rtti'],
  dependencies: [
    zmqnet_dep, libLrleBitset_dep, libzmq_dep, cppzmq_dep, capnp_dep,
    common_tools_be_dep, dl_dep, uuid_dep, threads_dep,
  ],
  install_rpath: rel_rpath + ':' + common_tools_fe_dep.get_variable('libdir'),
  pie: true,
  install: true,
  install_dir: libexecdir,
)

########################################
### Public headers                   ###
########################################
install_headers('include/zmqnet.hpp', 'include/zmqnet.h')

########################################
### pkg-config file                  ###
########################################
pc_conf = configuration_data()
pc_conf.set('prefix', prefix)
pc_conf.set('exec_prefix', prefix)
pc_conf.set('libdir', '${exec_prefix}/' + libdir)
pc_conf.set('includedir', '${prefix}/' + get_option('includedir'))
pc_conf.set('PACKAGE_VERSION', meson.project_version())

configure_file(
  input: 'libzmqnet.pc.in',
  output: 'libzmqnet.pc',
  configuration: pc_conf,
  install: true,
  install_dir: libdir / 'pkgconfig',
)

########################################
### tests/unit                       ###
########################################
cmake = import('cmake')
googletest_subproject = cmake.subproject('googletest')
gtest_dep = googletest_subproject.dependency('gtest')
gmock_dep = googletest_subproject.dependency('gmock')
googletest_dep = [gtest_dep, gmock_dep]

#gtest_root = 'tests/support/googletest/googletest'
#gtest_inc = include_directories(gtest_root / 'include', gtest_root)

unit_tests = executable('libzmqnet_unit_tests',
  [
    'tests/unit/libzmqnet_unit_tests.cpp',
  ],
  include_directories: [inc_root, inc_src, inc_public,
                        include_directories('tests/unit')],
  dependencies: [zmqnet_dep, libLrleBitset_dep, libzmq_dep, cppzmq_dep,
                 capnp_dep, common_tools_fe_dep, common_tools_be_dep,
                 threads_dep, googletest_dep],
  link_args: ['-Wl,-E'],
  build_rpath: libdir,
  build_by_default: false,
  install: false,
)

test('libzmqnet_unit_tests', unit_tests)

pandoc = find_program(['pandoc', 'doc/local_pandoc.sh'])
shell = find_program('sh')

manpage_markdown = custom_target('zmqnet.3.md',
  input: [
    'doc/manpage_header.md',
    'doc/sphinx/description.md',
    'doc/sphinx/environment.md',
    'doc/sphinx/cti.md',
    'doc/sphinx/topology.md',
    'doc/sphinx/filters.md',
    'doc/sphinx/streams_and_communicators.md',
    'doc/sphinx/frontend.md',
    'doc/sphinx/backend.md',
    'doc/sphinx/examples/capnp.md',
    'doc/sphinx/see_also.md',
  ],
  output: 'zmqnet.3.md',
  command: [shell, '-c', 'cat "$@" > "$0"', '@OUTPUT@', '@INPUT@'],
  build_by_default: false,
)

manpages = custom_target('manpages',
  input: manpage_markdown,
  output: 'zmqnet.3',
  command: [
    pandoc,
    '--standalone',
    '--from=markdown',
    '--to=man',
    '--output=@OUTPUT@',
    '@INPUT@',
  ],
  build_by_default: true,
  install: true,
  install_dir: get_option('mandir') / 'man3',
)
