#!/bin/bash

# This is a script to attempt the clean up the extremely small /tmp directories on slice's
# compute nodes. It should be removed when CTI has a proper cleanup feature.

# Running this script as root is too dangerous!
if [[ $EUID == 0 ]]; then
    echo "Run this as non-root!"
    exit -1
fi

get_nodelist () {
    # Find list of nodes to clean up to pass to --nodelist. If we are in an allocation,
    # the --nodelist argument is unecessary. If not, to keep it simple, we just grab
    # whatever nodes are idle and assume that will be enough.

    if [[ -z "$SLURM_JOB_ID" ]]; then
        # Get list of idle nodes.
        #
        # Explanation of commands:
        # sinfo -o "%t %N"
        #   List node states, formatted to only include the state (idle, alloc, drain, etc) and the hostnames.
        # grep idle
        #   Filter to only include the idle nodes.
        # cut -d " " -f 2
        #   Extract the hostname portion of the sinfo line. The sinfo line will look something like
        #   "idle nid000002". Use the space character to delimit fields (-d " ") and grab the 2nd
        #   field (-f 2).
        node_list=$(sinfo -o "%t %N" | grep idle | cut -d " " -f 2)
        echo "--nodelist=$node_list"
    else
        # --nodelist not needed in an allocation
        echo ""
    fi
}

# Clean up uan's /tmp
rm -rf /tmp/cti*

nodelist_arg=$(get_nodelist)
echo "Nodelist: $nodelist_arg"

if [[ -z $nodelist_arg ]] && [[ -z $SLURM_JOB_ID ]]; then
    echo "No nodes?"
    exit -2
fi

echo "Before:"
srun $nodelist_arg df |& grep " /tmp"

# Clean up the nodes. Remove any files/directories that start with cti.
# It is expected that we will hit some permission denied errors.
srun $nodelist_arg ./node_cleanup.sh

echo "After:"
srun $nodelist_arg df |& grep " /tmp"
