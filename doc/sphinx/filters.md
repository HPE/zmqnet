# FILTERS

Filters implement the core operation of a ZMQNet network, data aggregation. Each intermediate commnode process in the communication tree will run a copy of the filter function each time it receives a batch of results from the nodes below it. A commnode's child can be either a backend, or another commnode. From the perspective of the filter, however, the same filter function is run at each level of the tree.

The key to a high-scaling communication tree is proper design and implementation of a data processing filter. Filters can be simple, such as addition of a list of incoming results, or complex, such as parsing and producing custom binary data. What is essential for scalability is that the filter reduces in some way the incoming data. By the time the frontend receives the final result, as much processing as possible should have been done by intermediate levels of filter functions. This tree-based parallelism can enable high performance while easing the conceptual burden of multiprocessing.

What is the shape of a ZMQNet filter function? It is a function that takes a vector of byte arrays, each one representing a message, and outputs a single byte array as a result.

	extern "C" {

	zmqnet::ByteArray filter_function_name(std::vector<zmqnet::ByteArray> const& inputs);

	} // extern "C"

Filter functions are compiled into a `.so` shared library for use by the ZMQNet network. As an implementation detail, all filter functions should be declared `extern "C"` to aid in dynamic loading by the commnode processes. A helper function `make_ByteArray` is provided to help produce the result array.

	auto result = zmqnet::make_ByteArray(/* bytes */, /* size */);

If no result is to be produced, the byte array can be empty.

	return zmqnet::ByteArray{};

A useful compile-time check for verifying that a filter function conforms to the correct filter function definition:

	static_assert(std::is_same<decltype(filter_function_name), zmqnet::FilterType>::value);

The contents of the input byte arrays and the output byte array will depend on the calling implementation. A simple filter that contencates all input strings can be implemented as

	extern "C" {

	zmqnet::ByteArray contencate_strings(std::vector<zmqnet::ByteArray> const& inputs)
	{
		auto result = std::string{};
		for (auto&& input : inputs) {
			auto inputStr = std::string{(char const*)input.begin()};
			result += inputStr + " ";
		}

		return zmqnet::make_ByteArray(result.c_str(),
			result.length() + 1);
	}
	static_assert(std::is_same<decltype(contencate_strings), zmqnet::FilterType>::value);

	} // extern "C"

Filter functions can of course be much more complex. For an example of a filter function aggregating Cap'n Proto objects, see the section `Filter example: Cap'n Proto`.


