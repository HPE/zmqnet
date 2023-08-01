# BACKEND

## Initialization

The first step to initialize a backend is to parse parameters. During the frontend launch process, the application-provided leaf process arguments are added to generated backend arguments. These generated arguments contain the necessary information for the backend to connect to the network.

The function `getParams` is provided to automatically separate out the network connection parameters from the application-provided leaf arguments. Then, the backend can connect to the network using the provided parameters, and the leaf process argument array can be used as normal.

For example, if the leaf process argument array provided to the frontend initialization was `{"a.out", "arg1", nullptr}`, here `leaf_argv` would contain `{"a.out", "arg1", nullptr}` after being extracted using `getParams`.

	auto const [beParams, leaf_argc, leaf_argv] = zmqnet::SyncBackend::getParams(argc, argv);

	// Run network initialization.
	auto leafnode = zmqnet::SyncBackend{beParams};

## Synchronous backend

Once the synchronous backend is created and connected to the network, the backend can then receive commands using the `nextCommand` function. Each incoming command is identified by the application-specific stream ID as provided to the stream creation function in the frontend. A negative stream ID indicates that the network is exiting, and that the backend should also exit.

Results are sent using the `sendResult` function. This function should be invoked in response to a command sent on the same stream.

	// Connect synchronous backend
	auto leafnode = zmqnet::SyncBackend{beParams};

	// Receive incoming commands
	while (true) {
		auto const [stream, command] = leafnode.nextCommand();

		// Network exiting
		if (stream.id < 0) {
			::exit(0);

		// Received valid command
		} else {
			printf("Got command %s from stream ID %d\n",
				command.begin(), stream.id);
			
			// Send result on the same stream
			auto const result = std::string{"result string"};
			leafnode.sendResult(stream,
				result.c_str(), result.length() + 1);
		}
	}

## Asynchronous backend

The asynchronous backend constructor is invoked identically to the synchronous backend constructor.

	auto leafnode = zmqnet::AsyncBackend{beParams};

After the network is connected, the asynchronous callbacks for each application-specific stream ID are registered. Command callbacks take a pair of stream ID and `zmqnet::ByteArray`, where the stream ID corresponds to the stream on which the incoming command is received, and the `ByteArray` is the raw contents of the command.  The callback result is a new `ByteArray` that is then sent as a message to the parent node. If the result array is empty, then no message is sent. This can be accomplished by returning a default-constructed `zmqnet::ByteArray{}`.

After all callbacks are registered, the `notifyReady` function is used to tell the frontend that this leaf process is ready to receive asynchronous commands.

Finally, the `waitForExit` function will block until the frontend shuts down the communication network.

	// Connect the backend to the network
	auto StreamId = 1;
	auto leafnode = zmqnet::AsyncBackend{beParams};

	// Register the asynchronous callback
	leafnode.registerCallback(StreamId,
		[](zmqnet::Stream::Id const& streamId, zmqnet::ByteArray command) {
			fprintf(stderr, "Got command %s from stream %d\n", 
				command.begin(), streamId);
			assert(streamId == TestStreamId);
			auto const result = std::string{"result string"};
			return zmqnet::make_ByteArray(result.c_str(), result.length() + 1);
		});

	// Notify the frontend that all callbacks are registered
	leafnode.notifyReady();

	// Wait for the network to exit
	// Callbacks are invoked asynchronously as commands are received
	leafnode.waitForExit();
	exit(0);


