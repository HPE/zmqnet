/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#pragma once

#include <uuid/uuid.h>

#include <vector>
#include <string>
#include <unordered_map>
#include <future>
#include <memory>

#include <zmq.hpp>

#include <kj/array.h>

// Forward-declares

namespace net
{
	struct shared_semaphore;

	class TcpDownstreamLink;
	class UpstreamLink;
}

namespace zmqnet
{

using StreamId = int;
using ByteArray = kj::Array<kj::byte>;

static inline auto make_ByteArray(char const* buffer, size_t buffer_len)
{
	return kj::heapArray((unsigned char const*)buffer, buffer_len);
}

struct Topology
{
	enum : int
		{ Fcfs
		, Layered
		, Packed
	} launch;

	int fanout;
};

class Frontend
{
protected:
	zmq::context_t m_ctx;
	std::unique_ptr<net::TcpDownstreamLink> m_downstream;

public:
	Frontend(int const app_id, long int const session_id, long int const manifest_id,
		char const* const* leaf_argv, Topology const& topology);
	~Frontend();

protected:
	void createStream(StreamId const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName);
	void sendCommand(StreamId const& streamId, char const* command_buffer, size_t command_len);
	void sendExit();
};

class SyncFrontend : public Frontend
{
public:
	using Frontend::Frontend;

	void registerStream(StreamId const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName);
	ByteArray runCommand(StreamId const& streamId, char const* command_buffer, size_t command_len);
	void runExit();
};

class AsyncFrontend : public Frontend
{
public:
	using CallbackFunc = std::function<void(StreamId const&, ByteArray)>;

private:
	zmq::context_t m_ctx;
	std::unordered_map<StreamId, std::future<bool>> m_callbackWorkers;
	std::unique_ptr<net::shared_semaphore> m_stream_registering;
	std::future<zmq::socket_t> m_responseDistributor;

public:
	AsyncFrontend(int const app_id, long int const session_id, long int const manifest_id,
		char const* const* leaf_argv, Topology const& topology);
	~AsyncFrontend();

	void registerStream(StreamId const& streamId, std::string const& filterLibraryName, std::string const& filterFunctionName,
		CallbackFunc callbackFunc);
	void sendCommand(StreamId const& streamId, char const* command_buffer, size_t command_len);
	void runExit();
};

class Backend
{
public:
	template <typename CharPtr>
	static auto getParams(int argc, CharPtr argv)
	{
		if (argc < 4) {
			throw std::runtime_error("expected 3 network param arguments");
		}
		return std::make_tuple(
			std::string{argv[1]}, // upstream control endpoint
			std::string{argv[2]}, // upstream data endpoint
			std::stoi(argv[3]),   // rank number
			argc - 3, // 3 less arguments
			argv + 3);
	}

protected:
	zmq::context_t m_ctx;
	std::unique_ptr<net::UpstreamLink> m_upstream;
	int m_rank;

	void notifyReady();

public:
	Backend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank);
	~Backend();

	void sendResult(StreamId const& streamId, char const* result_buffer, size_t result_len);
};

class SyncBackend : public Backend
{
public:
	SyncBackend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank);
	std::tuple<StreamId, ByteArray> nextCommand();
};


class AsyncBackend : public Backend
{
public:
	using CallbackFunc = std::function<ByteArray(StreamId const&, ByteArray)>;

private:
	zmq::context_t m_ctx;

	std::string m_collectorEndpoint;
	zmq::socket_t m_collectorSocket;
	std::future<zmq::socket_t> m_responseCollector;

	std::unordered_map<StreamId, std::future<bool>> m_commandWorkers;
	std::future<zmq::socket_t> m_commandDistributor;

public:
	AsyncBackend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank);
	~AsyncBackend();

	void registerCallback(StreamId const& streamId, CallbackFunc callbackFunc);
	void notifyReady();
	void waitForExit();
};

} // zmqnet
