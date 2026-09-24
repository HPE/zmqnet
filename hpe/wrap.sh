#!/bin/bash

set -ex

# custom meson wrap script for working with hpe build
# pipelines.   Pipelines can't access source from git
# but can get to submodules, so we'll manually copy the
# submodule over to subprojects and apply an overlay
# if necesary

# directory of this script
hpe="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

subproject="$hpe/../subprojects"

for module in "$@"; do
    src="$hpe/external/$module"
    if [ ! -d "$src" ]; then
	echo "submodule $module is not checked out"
	exit 1
    fi

    if [ -d "$subproject/$module" ]; then
	echo "submodule $module is already cached, skipping"
	continue
    fi

    echo "copying external/$module to $subproject"
    cp -r "$src" $subproject

    if [ ! -f $subproject/$module.wrap ]; then
	continue
    fi

    patch=$(grep '^patch_directory *=' $subproject/$module.wrap | sed 's/.*= *//')

    if [ -n "$patch" ]; then
	echo "apply the patch"
	cp -r "$subproject/packagefiles/$patch/." "$subproject/$module"
    fi
    
done
    
