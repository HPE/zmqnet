# FRONTEND

The ZMQNet network frontend is the entry point to the ZMQNet network. The frontend is responsible for launching the network and initializing communications structures such as communicators and streams, as well as sending commands and receiving responses.

There are two types of frontends, synchronous and asynchronous. A synchronous frontend will send a single command, and wait until a response is received before returning. An asynchronous frontend sends commands, and uses callbacks to respond to responses asynchronously.

## Launch parameters

To launch a synchronous or asynchronous frontend, first launch or attach to a job using CTI to obtain an application ID `app_id`, and a ZMQNet topology object `topology`.

	auto app_id = /* CTI launch or attach */;
	auto topology = zmqnet::Topology{ ... };

Create the CTI session and manifest, and add at least the backend leaf binary and the filter function library to the CTI manifest.

	auto const session_id = cti_createSession(app_id);
	auto const manifest_id = cti_createManifest(session_id);
	cti_addManifestBinary(manifest_id, "./test_backend");
	cti_addManifestLibrary(manifest_id, "./libfilter.so");

Create an array of C string pointers that contain the process name and arguments for your leaf process.

	char const* leaf_argv[] = {
		"leaf_binary", "leaf_argument_1", ..., nullptr };

Now, you can launch either a synchronous or asynchronous ZMQNet network frontend.

## Synchronous frontend

### Initialization

Use the synchronous frontend constructor to launch the network.

	auto network = zmqnet::SyncFrontend
		{ app_id // CTI application ID
		, session_id // CTI session ID
		, manifest_id // CTI manifest ID. Must have added at least
			// the backend leaf binary and filter function library
		, topology // Network topology object
		, leaf_argv // Leaf binary argv array
		, nullptr // No additional environment variables
	};

The constructor will block until the whole network is initialized. This may take a while, depending on the size of the network and system configuraiton. Once the network is created, the communicators and streams can be initialized.

	// Create the communicator and stream
	auto communicator = network.getAllNodesCommunicator();
	auto stream = network.registerStream(...);

### Issuing commands and collecting results

With a synchronous frontend, issuing commands and collecting results runs synchronously and sequentially. There are two similar functions to run a command, one takes a C++ string, the other a C byte array. The result is a single ZMQNet byte array as produced by the filter function pipeline.

	zmqnet::ByteArray zmqnet::SyncFrontend::runCommand(
		Stream const& stream, // Stream object from `registerStream`
		char const* command_buffer, // Command byte array
		size_t command_len); // Byte array length
	zmqnet::ByteArray zmqnet::SyncFrontend::runCommand(
		Stream const& stream, // Stream object from `registerStream`
		std::string const& command); // Command string

The call to `runCommand` will block until the final result is received from the network.

	auto const result = network.runCommand(stream, "send", 5);
	printf("Result: %s\n",
		(char const*)result.asBytes().begin());

The network is stopped using the synchronous `runExit` command. It will block until all backend leaf processes and intermediate commnode processes have exited.

	network.runExit();

## Asynchronous frontend

### Initialization

Use the asynchronous frontend constructor to launch the network.

	auto network = zmqnet::AsyncFrontend
		{ app_id // CTI application ID
		, session_id // CTI session ID
		, manifest_id // CTI manifest ID. Must have added at least
			// the backend leaf binary and filter function library
		, topology // Network topology object
		, leaf_argv // Leaf binary argv array
		, nullptr // No additional environment variables
	};

The constructor will block until the whole network is initialized. This may take a while, depending on the size of the network and system configuraiton. Once the network is created, the communicators and streams can be initialized.

	// Create the communicator, callback, and stream
	auto communicator = network.getAllNodesCommunicator();
	auto callback = [&](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) { ... };
	network.registerStreamCallback(StreamId, callback);
	auto stream = network.registerStream(...);

### Issuing commands and collecting results

Issuing commands with an asynchronous frontend is similar to a synchronous frontend. Instead of a blocking `runCommand` function that produces a result, the asynchronous frontend provides a function `sendCommand` that does not wait for a result to be received. Results are received by invoking the callback registered for the stream.

Here is an example callback that uses a condition variable to notify the main thread that a result has been received asynchronously.

	auto result_mutex = std::mutex{};
	auto result_cv = std::condition_variable{};
	auto results_received = std::atomic<size_t>{0};

	auto StreamId = 1;

	// Create callback
	auto callback = [&](zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
		ASSERT_EQ(streamId, StreamId);

		// Parse and store result
		results_received = parse_string(
			(char const*)result.asBytes().begin());
		result_cv.notify_one();
	};

	// Register stream
	network.registerStreamCallback(StreamId, callback);
	auto communicator = network.getAllNodesCommunicator();
	auto stream = network.registerStream( allNodesCommunicator,
		TestStreamId, "libfilter.so", "filter_function_name",
		zmqnet::sync::WaitForAll{});

	// Send command
	network.sendCommand(stream, "backend_command");

	// Wait for command to run
	{ auto result_lock = std::unique_lock<std::mutex>{result_mutex};
		result_cv.wait(result_lock, [&results_received]() {
			return results_received.load() > 0;
		});
	}

	printf("Command had result %d\n", results_received.load());


