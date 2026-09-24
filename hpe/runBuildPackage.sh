#!/bin/bash
#
# runBuildPackage.sh - Package steps for zmqet
#
# Copyright 2022-2023 Hewlett Packard Enterprise Development LP.
#
# Unpublished Proprietary Information.
# This unpublished work is protected to trade secret, copyright and other laws.
# Except as permitted by contract or express written permission of Hewlett Packard Enterprise Development LP.,
# no part of this work or its content may be used, reproduced or disclosed
# in any form.
#

hpe="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
source "$hpe/external/cdst_build_library/build_lib"

setup_modules

# Load dependencies to encode required versions
module load cray-cti-devel
check_exit_status

echo "############################################"
echo "#             Creating rpm                 #"
echo "############################################"
rpmbuilddir="$hpe/rpmbuild"
cd ${rpmbuilddir}
check_exit_status
rpmbuild -bb -D "_topdir ${rpmbuilddir}" SPECS/zmqnet.spec
check_exit_status

if [ -f "$hpe/rpmbuild/BUILD/release_notes.md" ]; then
  echo
  echo
  echo "Release Notes in rpmbuild/BUILD:"
  cat "$hpe/rpmbuild/BUILD/release_notes.md"
fi

echo "############################################"
echo "#          Done with packaging             #"
echo "############################################"

exit_with_status
