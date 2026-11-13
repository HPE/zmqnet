![build](https://github.hpe.com/hpcde/cdst-zmqnet/actions/workflows/build_pipeline.yaml/badge.svg)

# ZMQNET

ZMQNet is a scalable, high performance networking library. It implements a tree-type command distribution and data processing model. Commands are sent from the root of the tree (the frontend) and distributed to leaves of the tree (backends). Intermediate processes, commnodes, are automatically managed by ZMQNet. Results are created by backends, collected and processed by commnodes, and communicated to the frontend.

ZMQNet is designed to run alongside an existing HPC job, building a tree communication and aggregation structure to support job analysis, debugging, and correctness tools.

In this guide, the "frontend application" will refer to the calling code making use of the ZMQNet library that issues commands and collects results. The "network frontend", by contrast, will refer to the ZMQNet network frontend instance itself. Likewise, the "backend application" will be hosting the "network backend" to receive commands and produce results.

ZMQNet is based on the ZMQ message-queue library. It uses several features of ZMQ beyond scalable message distribution, most notably client authentication and traffic encryption.

ZMQNet also uses the CTI Common Tools Interface library to automatically manage file shipping and application lifecycle. CTI is an interface library that provides a single interface for launching and attaching to running HPC jobs across several different workload managers, including Slurm, PBS / PALS, and Flux. To its end of supporting job analysis tools, ZMQNet requires a launch alongside a running HPC job. Calling code will use CTI to launch a new HPC job, or attach to an existing job.

- [cti](doc/sphinx/cti.md)
- [topology](doc/sphinx/topology.md)
- [filters](doc/sphinx/filters.md)
- [streams_and_communicators](doc/sphinx/streams_and_communicators.md)
- [frontend](doc/sphinx/frontend.md)
- [backend](doc/sphinx/backend.md)
- examples
  - [capnp](doc/sphinx/examples/capnp.md)
- illustrations
  - [Communication Network](<./doc/sphinx/imgs/Communication Network.png>)
  - [Frontend](doc/sphinx/imgs/Frontend.png)
  - [Backend](doc/sphinx/imgs/Backend.png)
  - [commnode2](doc/sphinx/imgs/commnode2.png)

## Building

### Prequisites:

- CTI either via open source install or from the cray-cti package.
- meson
- libuuid-devel
- openssl-devel
- libsodium-devel

These packages need newer versions than are installed on some
supported OS's, and are built as meson subprojects if
necessary.

- zeromq-devel
- capnproto-devel
- cppzmq-devel

### Building

The package can be build wiht the usual:

```bash
meson setup build
meson compile -C build
meson install -C build
```

sequence.

### Using libzmq and Cap'n Proto from Spack

The libzmq and Cap'n Proto (`capnp`) dependencies are discovered through
pkg-config. Activate a Spack environment with them installed and included in
its view before configuring Meson. When Cap'n Proto comes from pkg-config, the
`capnp` schema compiler is taken from the same installation. Check that the
environment exposes them with:

```bash
spack env activate tools
pkg-config --modversion libzmq capnp
pkg-config --variable=prefix libzmq capnp
```

zmqnet needs the bundled Cap'n Proto commit (it uses
`capnp::UnalignedFlatArrayMessageReader`, and dependents use the HPE
`kj::Array::forceReleaseAsBytes()` patch), so the Spack recipe pins
`capnproto@=0.8-dev`.

For an existing build configured by the bootstrap script, clear its saved
libzmq forced fallback while leaving cppzmq unchanged. `--clearcache` drops
dependency lookups cached by earlier configurations:

```bash
meson setup ./build --reconfigure --clearcache --wrap-mode=nofallback \
  --force-fallback-for=cppzmq --buildtype=debugoptimized \
  -Dprefix=/lus/scratch/jvogt/spack-tools/ -Dlibdir=lib/zmqnet
```

`force_fallback_for` persists across reconfiguration and overrides
`--wrap-mode=nofallback` for the listed dependencies. The installation prefix
does not select the dependency prefix; pkg-config's search path does.

Outside Spack, the default wrap mode still builds the bundled libzmq and
capnproto subprojects if pkg-config cannot find installed copies. When switching back from the
command above, use `--wrap-mode=default` to allow that fallback again.
Other subprojects are unchanged.

### Using Cap'n Proto from Spack

Cap'n Proto is also discovered through pkg-config, with a bundled subproject
fallback when no installation is found and the wrap mode allows it. In an
activated Spack environment, check:

```bash
pkg-config --modversion capnp
pkg-config --variable=exec_prefix capnp
```

The schema compiler (`bin/capnp`) and its C++ plugin (`bin/capnpc-c++`) must be
installed alongside the library. The build selects the compiler from that
installation rather than an unrelated compiler on `PATH`. Use the patched
Cap'n Proto package from the tools Spack repository to retain the
`kj::Array<T>::forceReleaseAsBytes()` API needed by downstream packages.
The zmqnet recipe pins `capnproto@0.8-dev` to the bundled commit
`0c9fabb2da55733d7c52d4318d05e24e2bffacaa`: newer releases such as 0.10.2
also lack `capnp::UnalignedFlatArrayMessageReader`, which zmqnet uses.
After replacing a dependency version in an existing build, add `--clearcache`
to the reconfigure command above to discard cached dependency paths (Meson
1.3 or newer).

The reconfigure command above uses installed libzmq and Cap'n Proto. For a
standalone build, the default wrap mode continues to support both bundled
fallbacks; use `--wrap-mode=default` when reconfiguring a build that previously
used `nofallback`.

## Open sourcing notes

This version continues is still set up to build with HPE pipelines.   For outside builds,
these files can be ignored:

* all git submodules
* the hpe directory
* .github/workflows/build_pipeline.yaml
* top level files container_def.yaml, build.sh, rpmBuild.sh, preBuild.sh, postBuild.sh
