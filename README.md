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

## Open sourcing notes

This version continues is still set up to build with HPE pipelines.   For outside builds,
these files can be ignored:

* all git submodules
* the hpe directory
* .github/workflows/build_pipeline.yaml
* top level files container_def.yaml, build.sh, rpmBuild.sh, preBuild.sh, postBuild.sh


