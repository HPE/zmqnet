#!/bin/bash

# This is used by cleanup.sh. find exits with 1 if it encounters a directory
# that it doesn't have permission to enter. This will definitely happen on
# a compute node's /tmp. This is okay for us, but when running as an srun
# job, the failure triggers all other ranks to be immediately killed, so
# we need to combine with /bin/true.
# You can't use || in commands passed to srun directly on the command
# line, (it gets evaluated on the client side) so we need to wrap it up
# in this little script.
find /tmp -name "cti*" -user $(id -u) -exec rm -rf {} + || /bin/true
