#!/bin/bash
#
# runInstallTest.sh - Install Test step for cray-zmqnet
#
# Copyright 2022-2023 Hewlett Packard Enterprise Development LP
#
# Unpublished Proprietary Information.
# This unpublished work is protected to trade secret, copyright and other laws.
# Except as permitted by contract or express written permission of Hewlett
# Packard Enterprise Development LP., no part of this work or its content may be
# used, reproduced or disclosed in any form.
#

# directory of this script
hpe="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
top_level="$hpe/.."

source "$hpe/external/cdst_build_library/build_lib"

source "$top_level/release_versioning"

zmqnet_version=
branch_name=$(get_branch_info)
branch_type=$(echo "$branch_name" | cut -d'/' -f1)
if [ "$branch_type" != "release" ]; then
  zmqnet_version="$major.$minor.$revision.$build_number"
else
  zmqnet_version="$major.$minor.$revision"
fi
install_dir="/opt/cray/pe/zmqnet/$zmqnet_version"

# Remove build dir
if [ -d ${install_dir} ]; then
   mv ${install_dir} ${install_dir}_orig
fi

# Install the package(s)
echo "############################################"
echo "#          Installing Packages             #"
echo "############################################"
arch=$(uname -m)
target_pm=$(get_pm)
if [[ "$target_pm" == "$cdst_pm_zypper" ]]; then
  sudo zypper --non-interactive --no-gpg-checks install "$hpe/rpmbuild/RPMS/$arch"/*.rpm
  check_exit_status 
elif [[ "$target_pm" == "$cdst_pm_yum" ]]; then
  sudo yum --assumeyes --nogpgcheck install "$hpe/rpmbuild/RPMS/$arch"/*.rpm
  check_exit_status
fi

echo "############################################"
echo "#              Release Notes               #"
echo "############################################"
# Print release notes file to stdout
if [ -f $install_dir/release_notes.md ]; then
  cat $install_dir/release_notes.md
else
  echo "$install_dir/release_notes.md was not generated."
  exit 1
fi

# Test Uninstall in Jenkins only
if [[ ! -z $BRANCH_NAME ]]; then
  echo "############################################"
  echo "#         Uninstalling Packages            #"
  echo "############################################"
  # Uninstall the package(s)
  if [[ "$target_pm" == "$cdst_pm_zypper" ]]; then
    sudo zypper --non-interactive remove cray-zmqnet
    check_exit_status
  elif [[ "$target_pm" == "$cdst_pm_yum" ]]; then
    sudo yum --assumeyes remove cray-zmqnet
    check_exit_status
  fi

  echo "############################################"
  echo "#     Check Uninstall was Successful       #"
  echo "############################################"
  # Check the uninstall was successful
  if [[ -d $install_dir ]]; then
    echo "There is an uninstall issue:"
    ls -R $install_dir
    exit 1
  else
    echo  "$install_dir was successfully cleaned."
  fi
fi

check_exit_status
