# Zmqnet

## 1.4.0 Open Source

### License changes

* This is the first version released under open-source.   This matches similar
  efforts gdb4hpc, atp, and cray-stat which depend on zmqnet.

### Build changes

* Replaced autotools with meson
* Enable build entirely outside of the HPE build environment

## 1.3.2

### Bug Fixes
 
* CPE-8343: Zero-initialize state struct before passing to `initstate_r`  
 
### Features
 
* Remove dependence on cdst-support package 
 
## 1.3.1
 
### Bug Fixes
 
* CPE-8040: Use random_r instead of crashing rand in ZMQ  
 
## [1.0.0]
 
### Bug Fixes
 
* CPE-7182: Remove TimeoutAfterLast from variant, create non-variant interface  
* Missing apollo install yamls PE-55041 
* PE-54822: Throw exception when fail to detect hostname  
* PE-53720: Include capnp tools in support directory  
* PE-53603: Show diagnostic when libsodium cannot be loaded  
* PE-51694: Make pings thread safe 
* PE-51165: Assign rank and PID in packed mode 
* PE-49776: Check TMPDIR before using it  
* PE-49611: Detect spurious timeouts and fix backend timeout setting  
* PE-48636: Remove message traversal limit  
* Fix install path exported by lmod modulefile on relocated install 
* PE-45208: Don't remove auth keyfile until setup completed  
* Fix rarely dropped messages during startup 
* Ensure nodes in newly created communicators are fully connected before returning new communicator to user 
 
### Features
 
* Build with g++ 14 on rhel 9.5 and sles15sp6 
* Add rhel 9.5, remove rhel 8.9 and sles15sp4 
* CPE-6082: Configurable backend timeout for flushing messages before exit  
* PE-5880: Add Timeout After Last synchronization option  
* Build rpms for rhel 8.10 
* Build on sles sp6 and rhel 9.4 
* PE-53605: Build and package libsodium dependency  
* PE-51960: Added support for rhel89/93 & gcc13  
* PE-44112: Add optional heartbeat monitor thread to AsyncFrontend  
* PE-50464: Implement configurable port base  
* Move to gcc-toolset-12 on redhat 
* PE-48567: ZMQNet documentation  
* Introduce forceDisconnect function to AsyncFrontend 
* PE-41205: Add option to disable encryption at build and run time  
