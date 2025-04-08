# Packaging definitions
%global pkgversion           %(%{_sourcedir}/get_package_data --crayversion)
%global branch               %(%{_sourcedir}/get_package_data --branch)
%global copyright_date       %(date +%%Y)
%global copyright            Certain components, files or programs contained within this package or product are Copyright 2020-%{copyright_date} Hewlett Packard Enterprise Development LP.

# RPM build time
%global release_date %(date +%%B\\ %%Y)

%global cray_product_prefix cray-
%global product zmqnet
%global cray_product %{product}
%global cray_name    %{cray_product_prefix}%{cray_product}

# Path definitions
%global cray_prefix /opt/cray/pe

%global external_build_dir %{cray_prefix}/%{cray_product}/%{pkgversion}
%global tests_source_dir   %{_topdir}/../tests/function
%global src_source_dir     %{_topdir}/../src

# pkconfig template
%global pkgconfig_template cray-zmqnet.pc.in

# modulefile definitions
%global modulefile_name       %{cray_name}
%global devel_modulefile_name %{cray_name}-devel

%global module_template_name       %{modulefile_name}.module.template
%global devel_module_template_name %{devel_modulefile_name}.module.template

# set_default modulefile definitions
%global set_default_command       set_default
%global set_default_template      set_default_template
%global set_default_path          admin-pe/set_default_files

# lmod modulefiles
%global lmod_template_zmqnet template_%{product}.lua
%global lmod_template_zmqnet_devel template_%{product}-devel.lua

# release notes
%global release_notes_file release_notes.md

# yaml file
%global yaml_template yaml.template
%global removal_date %(date '+%Y-%m-%d' -d "+5 years")

# copyright file
%global copyright_name PLACEHOLDER_COPYRIGHT

# attributions file
%global attributions_name PLACEHOLDER_ATTRIBUTIONS_zmqnet.txt

# cti version
%global cti_pkgversion_min %(%{_sourcedir}/get_package_data --ctiversionmin)
%global cti_pkgversion_max %(%{_sourcedir}/get_package_data --ctiversionmax)

# cdst-support version
%global cdst_support_pkgversion_min %(%{_sourcedir}/get_package_data --cdstversionmin)
%global cdst_support_pkgversion_max %(%{_sourcedir}/get_package_data --cdstversionmax)

# Disable debug package
%global debug_package %{nil}

# Disable /usr/lib/.build-id files
%global _build_id_links none

# Disable stripping of binaries
%global __os_install_post %{nil}

# Filter requires - these all come from the cdst-support rpm
# These should match what is excluded in the cdst-support rpm specfile!
%global privlibs             libboost.*
%global privlibs %{privlibs}|libmrnet
%global privlibs %{privlibs}|libbitset
%global privlibs %{privlibs}|libmi
%global privlibs %{privlibs}|libssh2
%global privlibs %{privlibs}|libarchive
%global privlibs %{privlibs}|libsymtabAPI
%global privlibs %{privlibs}|libpcontrol
%global privlibs %{privlibs}|libdynElf
%global privlibs %{privlibs}|libdynDwarf
%global privlibs %{privlibs}|libcommon
%global privlibs %{privlibs}|libxplat
%global privlibs %{privlibs}|libedit
%global privlibs %{privlibs}|libdw
%global privlibs %{privlibs}|libebl.*
%global privlibs %{privlibs}|libelf
%global privlibs %{privlibs}|libtbbmalloc*
%global privlibs %{privlibs}|libtbb.*
%global privlibs %{privlibs}|libstdc++.*
%global privlibs %{privlibs}|libzmqnet_libzmq.*
%global privlibs %{privlibs}|libzmqnet_libsodium.*
%global __requires_exclude ^(%{privlibs})\\.so*

# Dist tags for SuSE need to be manually set
%if 0%{?suse_version}
%if 0%{?sle_version} == 150600
%global dist .sles15sp6
%endif
%if 0%{?sle_version} == 150700
%global dist .sles15sp7
%endif
%global OS_HW_TAG 7.0
%global OS_WB_TAG sles15
%endif

%if "%{_arch}" == "aarch64"
%global SYS_HW_TAG AARCH64
%global SYS_WB_TAG AARCH64
%endif
%if "%{_arch}" == "x86_64"
%global SYS_HW_TAG HARDWARE
%global SYS_WB_TAG WHITEBOX
%endif
%if 0%{?rhel} == 8
%global OS_HW_TAG el8
%global OS_WB_TAG el8
%endif

Summary:    Cray zmqnet
Name:       %{cray_name}-%{pkgversion}
# BUILD_METADATA is set by Jenkins
Version:    %(echo ${BUILD_METADATA})
%if "%{branch}" == "release"
Release:    %(echo ${BUILD_NUMBER})%{dist}
%else
Release:    1%{dist}
%endif
Prefix:     %{cray_prefix}
License:    HPE Proprietary
Vendor:     Hewlett Packard Enterprise Development LP
Group:      Development/Tools/Debuggers
Provides:   %{cray_name} = %{pkgversion}
Requires:   set_default_3, cray-cdst-support >= %{cdst_support_pkgversion_min}, cray-cdst-support < %{cdst_support_pkgversion_max}, cray-cti >= %{cti_pkgversion_min}, cray-cti < %{cti_pkgversion_max}
Source0:    %{module_template_name}
Source1:    %{devel_module_template_name}
Source4:    %{release_notes_file}
Source5:    %{copyright_name}
Source6:    %{attributions_name}
Source7:    %{lmod_template_zmqnet}
Source2:    %{pkgconfig_template}
Source9:    %{yaml_template}

%description
ZMQNet is a scalable, high performance networking library, implementing a tree-type command distribution and data aggregation network.

%package -n %{cray_name}-devel-%{pkgversion}
Summary:    Cray zmqnet development files
Group:      Development
Provides:   %{cray_name}-devel = %{pkgversion}
Requires:   set_default_3, %{cray_name} = %{pkgversion}
Source8:    %{lmod_template_zmqnet_devel}
%description -n %{cray_name}-devel-%{pkgversion}
Development files for Cray zmqnet

%package -n %{cray_name}-tests-%{pkgversion}
Summary:    Cray zmqnet test binaries
Group:      Development
Provides:   %{cray_name}-tests = %{pkgversion}
Requires:   %{cray_name} = %{pkgversion}
%description -n %{cray_name}-tests-%{pkgversion}
Test files for Cray zmqnet

%prep
# Run q(uiet) with build directory name, c(reate) subdirectory, disable T(arball) unpacking
%setup -q -n %{name} -c -T

%build
# external build
%{__sed} 's|<VERSION>|%{pkgversion}|g;s|<RELEASE>|%{version}-%{release}|g;s|<date>\.<REVISION>|%{version}|g;s|<COPYRIGHT>|%{copyright}|g;s|<NAME>|%{cray_name}|g;s|<CRAY_PREFIX>|%{cray_prefix}|g;s|<DIST>|%{dist}.%{_target_cpu}|g;s|<ARCH>|%{_target_cpu}|g;s|<ATTRIBUTIONS_FILE_PATH>|%{cray_prefix}/%{product}/%{pkgversion}/%{attributions_name}|g;s|<cdst_version_range>|>= %{cdst_support_pkgversion_min}, < %{cdst_support_pkgversion_max}|g;s|<cti_version_range>|%{cti_pkgversion_min}, < %{cti_pkgversion_max}|g' %{SOURCE4} > ${RPM_BUILD_DIR}/%{release_notes_file}
%{__cp} -a %{SOURCE5} ${RPM_BUILD_DIR}/%{copyright_name}
%{__cp} -a %{SOURCE6} ${RPM_BUILD_DIR}/%{attributions_name}

%install
# copy files from external install

# Cray PE package root
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}

# Information files
%{__cp} -a ${RPM_BUILD_DIR}/%{release_notes_file} ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/%{release_notes_file}
%{__cp} -a ${RPM_BUILD_DIR}/%{copyright_name} ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/%{copyright_name}
%{__cp} -a ${RPM_BUILD_DIR}/%{attributions_name} ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/%{attributions_name}

# Support binaries
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/bin
%{__cp} -a %{external_build_dir}/support/bin/capnp* ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/bin

# Libraries
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/lib
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libcapnp-0.8-dev.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libcapnpc-0.8-dev.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libcapnpc.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libcapnp-json-0.8-dev.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libcapnp-json.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libcapnp-rpc-0.8-dev.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libcapnp-rpc.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libcapnp.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libkj-0.8-dev.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libkj-async-0.8-dev.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libkj-async.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libkj-http-0.8-dev.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libkj-http.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libkj.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libkj-test-0.8-dev.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libkj-test.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libkj-tls-0.8-dev.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libkj-tls.so ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libLrleBitset.so* ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libzmqnet_libzmq.so* ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/support/lib/libzmqnet_libsodium.so* ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{__cp} -a %{external_build_dir}/lib/libzmqnet.so* ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/lib

# Libraries static
# pkg-config
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/lib/pkgconfig
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/support/lib/pkgconfig/capnp-json.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/support/lib/pkgconfig/capnp.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/support/lib/pkgconfig/capnp-rpc.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/support/lib/pkgconfig/kj-async.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/support/lib/pkgconfig/kj-http.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/support/lib/pkgconfig/kj.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/support/lib/pkgconfig/kj-test.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/support/lib/pkgconfig/libLrleBitset.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/support/lib/pkgconfig/libzmq.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/support/lib/pkgconfig/libsodium.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{__cp} -a %{external_build_dir}/lib/pkgconfig/libzmqnet.pc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/lib/pkgconfig

# Headers
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/include
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include/capnp
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include/kj
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include/sodium

# /include/
%{__cp} -a %{external_build_dir}/include/zmqnet.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/include
%{__cp} -a %{external_build_dir}/include/zmqnet.hpp ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/include
%{__cp} -a %{external_build_dir}/support/include/capnp/* ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include/capnp
%{__cp} -a %{external_build_dir}/support/include/kj/* ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include/kj
%{__cp} -a %{external_build_dir}/support/include/lrle_bitset.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include
%{__cp} -a %{external_build_dir}/support/include/sodium/* ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include/sodium
%{__cp} -a %{external_build_dir}/support/include/sodium.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include
%{__cp} -a %{external_build_dir}/support/include/zmq.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include
%{__cp} -a %{external_build_dir}/support/include/zmq.hpp ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include
%{__cp} -a %{external_build_dir}/support/include/zmq_addon.hpp ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include
%{__cp} -a %{external_build_dir}/support/include/zmq_utils.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/support/include

# For building tests:
%{__cp} -a %{src_source_dir}/cti_argv.hpp ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/include

# libexec binaries
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/libexec
%{__cp} -a %{external_build_dir}/libexec/commnode ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/libexec

# tcl modulefiles
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/modulefiles/%{modulefile_name}
%{__sed} 's|<PREFIX>|%{external_build_dir}|g;s|<CRAY_NAME>|%{cray_name}|g;s|<VERSION>|%{pkgversion}|g;s|<COPYRIGHT>|%{copyright}|g' %{SOURCE0} > ${RPM_BUILD_ROOT}/%{prefix}/modulefiles/%{modulefile_name}/%{pkgversion}

%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/modulefiles/%{devel_modulefile_name}
%{__sed} 's|<PREFIX>|%{external_build_dir}|g;s|<CRAY_NAME>|%{devel_modulefile_name}|g;s|<CRAY_BASE_NAME>|%{cray_name}|g;s|<VERSION>|%{pkgversion}|g;s|<COPYRIGHT>|%{copyright}|g' %{SOURCE1} > ${RPM_BUILD_ROOT}/%{prefix}/modulefiles/%{devel_modulefile_name}/%{pkgversion}

# lmod modulefiles
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/lmod/modulefiles/core/%{cray_name}
%{__sed} 's|\[@%PREFIX_PATH%@\]|%{prefix}|g;s|\[@%MODULE_VERSION%@\]|%{pkgversion}|g;s|<COPYRIGHT>|%{copyright}|g' %{SOURCE7} > ${RPM_BUILD_ROOT}/%{prefix}/lmod/modulefiles/core/%{modulefile_name}/%{pkgversion}.lua

%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/lmod/modulefiles/core/%{devel_modulefile_name}
%{__sed} 's|\[@%PREFIX_PATH%@\]|%{prefix}|g;s|\[@%MODULE_VERSION%@\]|%{pkgversion}|g;s|<COPYRIGHT>|%{copyright}|g' %{SOURCE8} > ${RPM_BUILD_ROOT}/%{prefix}/lmod/modulefiles/core/%{devel_modulefile_name}/%{pkgversion}.lua

# set_default template
%{__sed} 's|\[product_name\]|%{cray_product}|g;s|\[version_string\]|%{pkgversion}|g;s|\[install_dir\]|%{prefix}|g;s|\[module_dir\]|%{prefix}/modulefiles|g;s|\[module_name_list\]|%{modulefile_name}|g;s|\[lmod_dir_list\]|%{prefix}/lmod/modulefiles/core|g;s|\[lmod_name_list\]|%{modulefile_name}|g' %{_sourcedir}/set_default/%{set_default_template} > ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/%{set_default_command}_%{cray_name}_%{pkgversion}

# set_default template - devel
%{__sed} 's|\[product_name\]|%{cray_product}|g;s|\[version_string\]|%{pkgversion}|g;s|\[install_dir\]|%{prefix}|g;s|\[module_dir\]|%{prefix}/modulefiles|g;s|\[module_name_list\]|%{devel_modulefile_name}|g;s|\[lmod_dir_list\]|%{prefix}/lmod/modulefiles/core|g;s|\[lmod_name_list\]|%{devel_modulefile_name}|g' %{_sourcedir}/set_default/%{set_default_template} > ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/%{set_default_command}_%{devel_modulefile_name}_%{pkgversion}

# set_default into admin-pe
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{set_default_path}
%{__install} -D ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/%{set_default_command}_%{cray_name}_%{pkgversion} ${RPM_BUILD_ROOT}/%{prefix}/%{set_default_path}/%{set_default_command}_%{cray_name}_%{pkgversion}
%{__install} -D ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/%{set_default_command}_%{devel_modulefile_name}_%{pkgversion} ${RPM_BUILD_ROOT}/%{prefix}/%{set_default_path}/%{set_default_command}_%{devel_modulefile_name}_%{pkgversion}

# yaml file
%{__mkdir} -p %{_rpmdir}/%{_arch}
%global start_rmLine %(sed -n /section-3/= %{SOURCE9})
%global end_rmLine %(sed -n /admin-pe/= %{SOURCE9} | tail -1)
%global devel_rpm_name %{cray_name}-devel-%{pkgversion}-%{version}-%{release}.%{_arch}
%global tests_rpm_name %{cray_name}-tests-%{pkgversion}-%{version}-%{release}.%{_arch}
%global rpm_list %{devel_rpm_name}.rpm,%{tests_rpm_name}.rpm
%if 0%{?rhel} == 8
%if "%{branch}" == "release"
%{__sed} 's|<PRODUCT>|%{cray_name}|g;s|<VERSION>|%{pkgversion}|g;s|<BUILD_METADATA>|%{version}|g;s|<RELEASE>|%{release}|g;s|<ARCH>|%{_arch}|g;s|<REMOVAL_DATE>|%{removal_date}|g;s|<SYS_HW_TAG>|%{SYS_HW_TAG}|g;s|<SYS_WB_TAG>|%{SYS_WB_TAG}|g;s|<OS_HW_TAG>|%{OS_HW_TAG}|g;s|<OS_WB_TAG>|%{OS_WB_TAG}|g;s|<RPM_LST>|%{rpm_list}|g;s|<DEVEL_RPM_NAME>|%{devel_rpm_name}|g;s|<TESTS_RPM_NAME>|%{tests_rpm_name}|g;s|<SET_DEFAULT_VALUE>|1|g' %{SOURCE9} > %{_rpmdir}/%{_arch}/%{cray_name}-%{pkgversion}-%{version}-%{release}.%{_arch}.rpm.yaml
%else
%{__sed} '%{start_rmLine},%{end_rmLine}d;s|section-5|section-3|g;s|<PRODUCT>|%{cray_name}|g;s|<VERSION>|%{pkgversion}|g;s|<BUILD_METADATA>|%{version}|g;s|<RELEASE>|%{release}|g;s|<ARCH>|%{_arch}|g;s|<REMOVAL_DATE>|%{removal_date}|g;s|<SYS_HW_TAG>|%{SYS_HW_TAG}|g;s|<SYS_WB_TAG>|%{SYS_WB_TAG}|g;s|<OS_HW_TAG>|%{OS_HW_TAG}|g;s|<OS_WB_TAG>|%{OS_WB_TAG}|g;s|<RPM_LST>|%{rpm_list}|g;s|<DEVEL_RPM_NAME>|%{devel_rpm_name}|g;s|<TESTS_RPM_NAME>|%{tests_rpm_name}|g;s|<SET_DEFAULT_VALUE>|0|g' %{SOURCE9} > %{_rpmdir}/%{_arch}/%{cray_name}-%{pkgversion}-%{version}-%{release}.%{_arch}.rpm.yaml
%endif
%endif

# Test files
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal/custom
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/data
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/scripts
%{__install} -d ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/src

# /tests/
%{__cp} -a %{tests_source_dir}/avocado_config_fullsuite.yaml ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/avocado_config.yaml ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/inproc.cpp ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/libfilter.cpp ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/libzmqnet_function_tests.cpp ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/libzmqnet_tests.hpp ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/Makefile ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/stop.c ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/test_backend.cpp ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/tests.py ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/cleanup.sh ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/node_cleanup.sh ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/test_tool_config.yaml ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
# test_tool.py pulled from cdst-test:
%{__cp} -a %{tests_source_dir}/cdst-test/test_tool.py ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests
%{__cp} -a %{tests_source_dir}/topotest.cpp ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests

# /tests/googletest/googletest/src/
%{__cp} -a %{tests_source_dir}/googletest/googletest/src/gtest-all.cc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src/
%{__cp} -a %{tests_source_dir}/googletest/googletest/src/gtest.cc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src/
%{__cp} -a %{tests_source_dir}/googletest/googletest/src/gtest-death-test.cc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src/
%{__cp} -a %{tests_source_dir}/googletest/googletest/src/gtest-filepath.cc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src/
%{__cp} -a %{tests_source_dir}/googletest/googletest/src/gtest-internal-inl.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src/
%{__cp} -a %{tests_source_dir}/googletest/googletest/src/gtest_main.cc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src/
%{__cp} -a %{tests_source_dir}/googletest/googletest/src/gtest-port.cc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src/
%{__cp} -a %{tests_source_dir}/googletest/googletest/src/gtest-printers.cc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src/
%{__cp} -a %{tests_source_dir}/googletest/googletest/src/gtest-test-part.cc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src/
%{__cp} -a %{tests_source_dir}/googletest/googletest/src/gtest-typed-test.cc ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/src/

# /tests/googletest/googletest/include/gtest/
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest-death-test.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest-message.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest-param-test.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest-param-test.h.pump ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest_pred_impl.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest-printers.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest_prod.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest-spi.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest-test-part.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/gtest-typed-test.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest

# /tests/googletest/googletest/include/gtest/internal/
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-death-test-internal.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-filepath.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-internal.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-linked_ptr.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-param-util-generated.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-param-util-generated.h.pump ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-param-util.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-port-arch.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-port.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-string.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-tuple.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-tuple.h.pump ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-type-util.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/gtest-type-util.h.pump ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal

# /tests/googletest/googletest/include/gtest/internal/custom/
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/custom/gtest.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal/custom
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/custom/gtest-port.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal/custom
%{__cp} -a %{tests_source_dir}/googletest/googletest/include/gtest/internal/custom/gtest-printers.h ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/googletest/googletest/include/gtest/internal/custom

# /tests/cdst_test/
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/README.md                   ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/README.md
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/README_config.md            ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/README_config.md
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/README_textcheck.md         ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/README_textcheck.md
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/__init__.py                 ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/__init__.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/requirements.txt            ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/requirements.txt
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/artifactory.py              ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/artifactory.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/bootstrap.py                ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/bootstrap.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/config.py                   ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/config.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/config_default.py           ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/config_default.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/config_nodes.py             ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/config_nodes.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/env.py                      ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/env.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/files.py                    ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/files.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/job_environment.py          ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/job_environment.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/log.py                      ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/log.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/modules.py                  ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/modules.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/remote_test_tool_startup.py ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/remote_test_tool_startup.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/remote_test_tool_ui.py      ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/remote_test_tool_ui.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/ssh.py                      ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/ssh.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/test_server.py              ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/test_server.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/test_server_commands.py     ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/test_server_commands.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/test_server_proto.py        ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/test_server_proto.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/textcheck.py                ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/textcheck.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/useful.py                   ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/useful.py

# /tests/cdst_test/data
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/data/hosts.yaml    ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/data/hosts.yaml
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/data/products.yaml ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/data/products.yaml

# /tests/cdst_test/scripts
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/scripts/avo_config.py        ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/scripts/avo_config.py
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/scripts/passwordedscp.expect ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/scripts/passwordedscp.expect
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/scripts/passwordedssh.expect ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/scripts/passwordedssh.expect
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/scripts/sshcopyid.expect     ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/scripts/sshcopyid.expect
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/scripts/uai.expect           ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/scripts/uai.expect
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/scripts/validate_ssh.sh      ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/scripts/validate_ssh.sh
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/scripts/detect_wlm.sh      ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/scripts/detect_wlm.sh

# /tests/cdst_test/src
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/src/Makefile    ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/src/Makefile
%{__cp} -a %{tests_source_dir}/cdst-test/cdst_test/src/hello_mpi.c ${RPM_BUILD_ROOT}/%{prefix}/%{cray_product}/%{pkgversion}/tests/cdst_test/src/hello_mpi.c

%post

# Add Shasta configuration files for PE content projection
if [ "${RPM_INSTALL_PREFIX}" = "%{prefix}" ]; then
    mkdir -p /etc/%{prefix}/admin-pe/bindmount.conf.d/
    mkdir -p /etc/%{prefix}/admin-pe/modulepaths.conf.d/
    echo "%{prefix}/%{product}/" > /etc/%{prefix}/admin-pe/bindmount.conf.d/%{cray_name}.conf
    echo "%{prefix}/modulefiles/%{modulefile_name}" > /etc/%{prefix}/admin-pe/modulepaths.conf.d/%{modulefile_name}.conf
    echo "%{prefix}/lmod/modulefiles/core/%{cray_name}" >> /etc/%{prefix}/admin-pe/modulepaths.conf.d/%{modulefile_name}.conf
fi

# Process relocations
if [ "${RPM_INSTALL_PREFIX}" != "%{prefix}" ]
then
    # tcl modulefile
    %{__sed} -i "s|^\([[:space:]]*set ZMQNET_BASEDIR[[:space:]]*\)\(.*\)|\1${RPM_INSTALL_PREFIX}/%{cray_product}/%{pkgversion}|" ${RPM_INSTALL_PREFIX}/modulefiles/%{modulefile_name}/%{pkgversion}
    # lua modulefile
    %{__sed} -i "s|^\([[:space:]]*local INSTALL_ROOT[[:space:]]=*\)\(.*\)|\1= \"${RPM_INSTALL_PREFIX}\"|" ${RPM_INSTALL_PREFIX}/lmod/modulefiles/core/%{modulefile_name}/%{pkgversion}.lua

    # set default command
    %{__sed} -i "s|^\(export CRAY_inst_dir=\).*|\1${RPM_INSTALL_PREFIX}|" ${RPM_INSTALL_PREFIX}/%{cray_product}/%{pkgversion}/%{set_default_command}_%{cray_name}_%{pkgversion}
else
    # Only call set_default if we are not relocating the rpm and there's not already a default set unless someone passes in CRAY_INSTALL_DEFAULT=1
    if [ ${CRAY_INSTALL_DEFAULT:-0} -eq 1 ] || [ ! -f ${RPM_INSTALL_PREFIX}/modulefiles/%{cray_name}/.version ]
    then
        # Set default - TCL
        ${RPM_INSTALL_PREFIX}/%{set_default_path}/%{set_default_command}_%{cray_name}_%{pkgversion}
    fi
fi

%post -n %{cray_name}-devel-%{pkgversion}

# Process relocations
if [ "${RPM_INSTALL_PREFIX}" != "%{prefix}" ]
then
    # tcl modulefile for the devel package
    %{__sed} -i "s|^\([[:space:]]*set ZMQNET_BASEDIR[[:space:]]*\)\(.*\)|\1${RPM_INSTALL_PREFIX}/%{cray_product}/%{pkgversion}|" ${RPM_INSTALL_PREFIX}/modulefiles/%{devel_modulefile_name}/%{pkgversion}
    # lua modulefile
    %{__sed} -i "s|^\([[:space:]]*local INSTALL_ROOT[[:space:]]=*\)\(.*\)|\1= \"${RPM_INSTALL_PREFIX}\"|" ${RPM_INSTALL_PREFIX}/lmod/modulefiles/core/%{devel_modulefile_name}/%{pkgversion}.lua
    # pkg-config pc files
    find ${RPM_INSTALL_PREFIX}/%{cray_product}/%{pkgversion}/lib/pkgconfig -name '*.pc' -exec %{__sed} -i "s|%{prefix}|${RPM_INSTALL_PREFIX}|g" {} \;
    # set default command
    %{__sed} -i "s|^\(export CRAY_inst_dir=\).*|\1${RPM_INSTALL_PREFIX}|" ${RPM_INSTALL_PREFIX}/%{cray_product}/%{pkgversion}/%{set_default_command}_%{devel_modulefile_name}_%{pkgversion}
else
    # Only call set_default if we are not relocating the rpm and there's not already a default set unless someone passes in CRAY_INSTALL_DEFAULT=1
    if [ ${CRAY_INSTALL_DEFAULT:-0} -eq 1 ] || [ ! -f ${RPM_INSTALL_PREFIX}/modulefiles/%{devel_modulefile_name}/.version ]
    then
        # set default
        ${RPM_INSTALL_PREFIX}/%{set_default_path}/%{set_default_command}_%{devel_modulefile_name}_%{pkgversion}
    fi
fi

%preun
default_link="${RPM_INSTALL_PREFIX}/%{cray_product}/default"

# Cleanup module .version if it points to this version
if [ -f ${RPM_INSTALL_PREFIX}/modulefiles/%{modulefile_name}/.version ]
then
  dotversion=`grep ModulesVersion ${RPM_INSTALL_PREFIX}/modulefiles/%{modulefile_name}/.version | cut -f2 -d'"'`

  if [ "$dotversion" == "%{pkgversion}" ]
  then
    %{__rm} -f ${RPM_INSTALL_PREFIX}/modulefiles/%{modulefile_name}/.version
    echo "Uninstalled version and .version file match = ${default_version}."
    echo "Removing %{modulefile_name} .version file."
    %{__rm} -f ${default_link}
  fi
fi

%postun
if [ $1 == 1 ]
then
  exit 0
fi

# If the install dir is empty
if [[ -z `ls ${RPM_INSTALL_PREFIX}/%{cray_product}` ]]; then
  %{__rm} -rf ${RPM_INSTALL_PREFIX}/%{cray_product}
  if [ -f /etc%{prefix}/admin-pe/bindmount.conf.d/%{cray_name}.conf ]; then
    %{__rm} -rf /etc%{prefix}/admin-pe/bindmount.conf.d/%{cray_name}.conf
  fi
  if [ -f /etc%{prefix}/admin-pe/modulepaths.conf.d/%{cray_name}.conf ]; then
    %{__rm} -rf /etc%{prefix}/admin-pe/modulepaths.conf.d/%{cray_name}.conf
  fi
  if [ -d ${RPM_INSTALL_PREFIX}/lmod/modulefiles/core/%{cray_name} ]; then
    %{__rm} -rf ${RPM_INSTALL_PREFIX}/lmod/modulefiles/core/%{cray_name}
  fi
  if [ -d ${RPM_INSTALL_PREFIX}/modulefiles/%{cray_name} ]; then
    %{__rm} -rf ${RPM_INSTALL_PREFIX}/modulefiles/%{cray_name}
  fi
fi

%postun -n %{cray_name}-devel-%{pkgversion}
if [ $1 == 1 ]
then
  exit 0
fi

# If the install dir is empty
if [[ -z `ls ${RPM_INSTALL_PREFIX}/%{cray_product}` ]]; then
  if [ -f /etc%{prefix}/admin-pe/modulepaths.conf.d/%{devel_modulefile_name}.conf ]; then
    %{__rm} -rf /etc%{prefix}/admin-pe/modulepaths.conf.d/%{devel_modulefile_name}.conf
  fi
  if [ -d ${RPM_INSTALL_PREFIX}/lmod/modulefiles/core/%{devel_modulefile_name} ]; then
    %{__rm} -rf ${RPM_INSTALL_PREFIX}/lmod/modulefiles/core/%{devel_modulefile_name}
  fi
  if [ -d ${RPM_INSTALL_PREFIX}/modulefiles/%{devel_modulefile_name} ]; then
    %{__rm} -rf ${RPM_INSTALL_PREFIX}/modulefiles/%{devel_modulefile_name}
  fi
fi

%postun -n %{cray_name}-tests-%{pkgversion}
if [ $1 == 1 ]
then
  exit 0
fi

# If the install dir exists
if [ -d %{prefix}/%{cray_product}/%{pkgversion}/tests ]; then
  %{__rm} -rf %{prefix}/%{cray_product}/%{pkgversion}/tests
fi

%files
%defattr(755, root, root)
%dir %{prefix}/%{cray_product}/%{pkgversion}
%dir %{prefix}/%{cray_product}/%{pkgversion}/support

# Information files
%attr(644, root, root) %{prefix}/%{cray_product}/%{pkgversion}/%{release_notes_file}
%attr(644, root, root) %{prefix}/%{cray_product}/%{pkgversion}/%{copyright_name}
%attr(644, root, root) %{prefix}/%{cray_product}/%{pkgversion}/%{attributions_name}

# Support binaries
%defattr(755, root, root)
%dir %{prefix}/%{cray_product}/%{pkgversion}/support/bin
%{prefix}/%{cray_product}/%{pkgversion}/support/bin/capnp*

# Libraries
%dir %{prefix}/%{cray_product}/%{pkgversion}/lib
%dir %{prefix}/%{cray_product}/%{pkgversion}/support/lib
%{prefix}/%{cray_product}/%{pkgversion}/lib/*.so
%{prefix}/%{cray_product}/%{pkgversion}/lib/*.so.*
%{prefix}/%{cray_product}/%{pkgversion}/support/lib/*.so
%{prefix}/%{cray_product}/%{pkgversion}/support/lib/*.so.*

# libexec binaries
%dir %{prefix}/%{cray_product}/%{pkgversion}/libexec
%{prefix}/%{cray_product}/%{pkgversion}/libexec/*

# tcl modulefiles
%dir %{prefix}/modulefiles/%{modulefile_name}
%{prefix}/modulefiles/%{modulefile_name}/*

# lmod modulefiles
%dir %{prefix}/lmod/modulefiles/core/%{cray_name}
%{prefix}/lmod/modulefiles/core/%{cray_name}/*

# set_default template
%attr(755, root, root) %{prefix}/%{cray_product}/%{pkgversion}/%{set_default_command}_%{cray_name}_%{pkgversion}
%attr(755, root, root) %{prefix}/%{set_default_path}/%{set_default_command}_%{cray_name}_%{pkgversion}

%files -n %{cray_name}-devel-%{pkgversion}

# Libraries static
# pkg-config
%dir %{prefix}/%{cray_product}/%{pkgversion}/lib/pkgconfig
%{prefix}/%{cray_product}/%{pkgversion}/lib/pkgconfig/*
%dir %{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig
%{prefix}/%{cray_product}/%{pkgversion}/support/lib/pkgconfig/*

# Headers
%dir %{prefix}/%{cray_product}/%{pkgversion}/include
%{prefix}/%{cray_product}/%{pkgversion}/include/*
%dir %{prefix}/%{cray_product}/%{pkgversion}/support/include
%{prefix}/%{cray_product}/%{pkgversion}/support/include/*

# tcl modulefiles
%{prefix}/modulefiles/%{devel_modulefile_name}/*

# lmod modulefiles
%{prefix}/lmod/modulefiles/core/%{devel_modulefile_name}/*

# set_default template - devel
%attr(755, root, root) %{prefix}/%{cray_product}/%{pkgversion}/%{set_default_command}_%{cray_name}-devel_%{pkgversion}
%attr(755, root, root) %{prefix}/%{set_default_path}/%{set_default_command}_%{cray_name}-devel_%{pkgversion}

%files -n %{cray_name}-tests-%{pkgversion}

# Test directories
%dir %{prefix}/%{cray_product}/%{pkgversion}/tests

# /tests/
%{prefix}/%{cray_product}/%{pkgversion}/tests/*
