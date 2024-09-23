# STREAMS

Streams are the next level above a filter. Conceptually, streams are a tree that runs a filter function in series, feeding and aggregating results starting at leaf processes and feeding up to a single root process. At each level of the tree, a single configurable type of synchronization is applied,

* Immediate: do not wait, apply the filter function to each message as soon as it is received
	- `auto sync = zmqnet::sync::Immediate{};`
* Wait for all: wait for all child processes in a stream to send a message before applying the filter function
	- `auto sync = zmqnet::sync::WaitForAll{};`
* Timeout: wait for the given number of milliseconds to elapse, or for all messages to be received before applying the filter function
	- `auto sync = zmqnet::sync::Timeout{ .ms = /* milliseconds */ };`
* TimeoutAfterLast: Wait indefinitely for the first message, then after timeout has elapsed without a new message, send all pending messages.
	- If a new message was received during the timeout wait, it will reset the timeout. If no messages are received, it will continue to wait.
	- Useful for collecting results where nodes may intentionally not respond.
	- `auto sync = zmqnet::sync::TimeoutAfterLast{ .grace = /* milliseconds */ };`

The most common type of synchronization is "Wait for all", or "Timeout" with an application-specific timeout. "Immediate" can be used to implement more complex synchronization in a filter function with global state (recall that filters can return an empty result to indicate that no message is to be sent).

# COMMUNICATORS

Communicators are the next level above a stream. Communicators represent the actual structure of the communication tree, hosting one or multiple streams.

The default "all nodes" communicator contains every backend leaf process in the communication network. Streams created on the default "all nodes" communicator will send commands to every backend leaf process, and collect and aggregate results from every backend leaf process.

	auto allNodesCommunicator = network.getAllNodesCommunicator();

New communicators containing a subset of the backend leaf processes can also be created. Each leaf is identified by the MPI rank number of the corresponding job launch registered with CTI. A mapping between compute node hostname and rank numbers can be obtained with the CTI function `cti_getAppHostsPlacement`. For more information, refer to the CTI developer documentation.

	auto someNodes = std::set<size_t>{0, 2};
	auto someNodesCommunicator = network.registerCommunicator(someNodes);

# STREAMS AND COMMUNICATORS USAGE

Once a communicator is created or otherwise referrable, a stream can then be created.

For each stream, an application-specific `Stream::Id` integer is provided. This ID is used at the frontend and backend processes to distinguish between multiple types of possible streams. For example, your application may have a unique stream for each of a multiple of data types. Each of these streams should have their own unique stream ID.

If using a synchronous frontend, streams are registered using the communicator, the stream ID, the filter library name, the filter function, and stream synchronization mode.

	auto TestStreamId = 1;

	// Get the "all nodes" communicator
	auto allNodesCommunicator = network.getAllNodesCommunicator();

	auto stream = network.registerStream(
		allNodesCommunicator, // Send to all backends
		TestStreamId, // Unique stream ID
		"libfilter.so", // Filter library shipped by CTI manifest
		"filter_function_name", // Function name in libfilter.so
		zmqnet::sync::WaitForAll{} // Synchronization mode
	);

Streams for asynchronous frontends are registered identically, with the exception that a callback for a stream must be registered before registering the stream itself. The callback will be invoked when the frontend recieves a result from that stream.

	void callback(zmqnet::Stream::Id const& streamId, zmqnet::ByteArrayPtr result) {
		// Do something with `result`
	}

	network.registerStreamCallback(TestStreamId, callback);
	auto stream = network.registerStream(...);


