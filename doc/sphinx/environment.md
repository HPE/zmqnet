# ENVIRONMENT

ZMQNet supports the following runtime environment variables:

- `LIBZMQNET_CURVE`

	Disable encryption if set to 0, otherwise enabled. Encryption is enabled by default.

- `LIBZMQNET_STARTUP_TIMEOUT`

    During ZMQNet network startup, there are a number of "handshakes"
    that need to be done between the frontend, the commnodes, and the backends.
    This variable is the amount of seconds that a process (frontend, commnode, or backend)
    will wait for the other side of any given startup handshake before quitting.
    If set to -1, the startup handshake timeout will be disabled.
    If not set, the default value is 300 seconds.

- `LIBZMQNET_CLEANUP_TIMEOUT`

	Maximum amount of seconds to wait when trying to clean up the network.
	Backend processes will attempt to send pending messages up to this timeout before exiting.
	If not set, the default value is 10 seconds.

- `LIBZMQNET_DEBUG_COMMNODE_STARTUP_SLEEP`

	If set to a positive integer, ZMQNet commnodes will sleep for this many seconds on
	startup. Default: unset


- `LIBZMQNET_PORT_BASE`

	If set, this is used as the base port for allocating port connections.
	By default, this is unset, and ports are allocated arbitrarily by the operating system.

- `LIBZMQNET_PORT_MAX`

	When `LIBZMQNET_PORT_BASE` is set, this is the the maximum number of ports after the
	offset to try before giving up. Default: 10

