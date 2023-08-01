# Filter example: Cap'n Proto

Filter functions can of course be much more complex than the simple examples in this guide. Cap'n Proto is an efficient way to build and read cross-platform data structures. An example filter function here is using Cap'n Proto objects sent over ZMQNet, without needing to copy the received messages before processing them.

	// Process and store received Capnp object Readers
	static void process_and_store_reader(ExampleObj::Reader const& reader);
	// Fill in an outgoing MessageReader with content
	static void fill_builder(capnp::MallocMessageBuilder& builder);

	// Don't limit traversal depth
	static const auto defaultReaderOptions = capnp::ReaderOptions
		{ .traversalLimitInWords = 1 << 30
	};

	extern "C" {

	zmqnet::ByteArray capnpFilter(std::vector<zmqnet::ByteArray> const& inputs)
	{

		// Iterate over all received messages
		for (auto&& content : inputs) {
			auto messageBuf = content.begin();
			auto messageLen = content.size();

			// Parse buffer as Capnp message reader
			auto const messageWords = reinterpret_cast<capnp::word const*>(messageBuf);
			auto const messageWordsLen = messageLen / sizeof(capnp::word);
			assert(messageLen % sizeof(capnp::word) == 0);
			auto messageReader = capnp::FlatArrayMessageReader(kj::arrayPtr(messageWords, messageWordsLen), defaultReaderOptions);

			// Perform aggregation on received ExampleObj
			auto reader = messageReader.getRoot<ExampleObj>();
			process_and_store_reader(reader);
		}

		// Build result message
		auto messageBuilder = capnp::MallocMessageBuilder{};
		fill_builder(messageBuilder);

		// Encode for ZMQNet
		auto resultWords = capnp::messageToFlatArray(messageBuilder);
		auto const resultBytes = (kj::byte*)resultWords.begin();
		auto const resultBytesLen = resultWords.size() * sizeof(capnp::word);
		return zmqnet::ByteArray(resultBytes, resultBytesLen);
	}
	static_assert(std::is_same<decltype(capnpFilter), zmqnet::FilterType>::value);

	} // extern "C"


