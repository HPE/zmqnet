/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#pragma once

#include <uuid/uuid.h>

#include <vector>
#include <string>
#include <unordered_map>
#include <future>

#include <zmq.hpp>

static inline std::string generate_uuid()
{
	uuid_t id;
	uuid_generate(id);

	char uuid_str[37];
	uuid_unparse(id, uuid_str);

	return std::string{uuid_str};
}

class UpstreamLink
{
public:
	zmq::socket_t control_socket, data_socket;

	void connect(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint)
	{
		// Connect to upstream sockets
		control_socket.connect(upstreamControlEndpoint);
		data_socket.connect(upstreamDataEndpoint);

		// Subscribe to all topics
		control_socket.set(zmq::sockopt::subscribe, "");
	}

	UpstreamLink(zmq::context_t& ctx)
		: control_socket{ctx, zmq::socket_type::sub}
		, data_socket{ctx, zmq::socket_type::push}
	{}

	UpstreamLink(zmq::context_t& ctx,
		std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint)
		: UpstreamLink{ctx}
	{
		connect(upstreamControlEndpoint, upstreamDataEndpoint);
	}
};

class DownstreamLink
{
public:
	std::string controlEndpoint;
	std::string dataEndpoint;

	zmq::socket_t control_socket, data_socket;

	DownstreamLink(zmq::context_t& ctx,
		std::string const& controlEndpoint_, std::string const& dataEndpoint_)
		: control_socket{ctx, zmq::socket_type::pub}
		, data_socket{ctx, zmq::socket_type::pull}
	{
		// Bind to specified endpoints
		control_socket.bind(controlEndpoint_);
		data_socket.bind(dataEndpoint_);

		// Set real endpoint values
		controlEndpoint = control_socket.get(zmq::sockopt::last_endpoint);
		dataEndpoint = data_socket.get(zmq::sockopt::last_endpoint);
	}
};

class InprocDownstreamLink : public DownstreamLink
{
public:
	InprocDownstreamLink(zmq::context_t& ctx)
		: DownstreamLink
			{ ctx
			, "inproc://" + generate_uuid() // control endpoint
			, "inproc://" + generate_uuid() // data endpoint
		}
	{}
};

class IpcDownstreamLink : public DownstreamLink
{
public:
	IpcDownstreamLink(zmq::context_t& ctx)
		: DownstreamLink
			{ ctx
			, "ipc://ipc-" + generate_uuid() // control endpoint
			, "ipc://ipc-" + generate_uuid() // data endpoint
		}
	{}
};

class TcpDownstreamLink : public DownstreamLink
{
public:
	TcpDownstreamLink(zmq::context_t& ctx)
		: DownstreamLink
			{ ctx
			, "tcp://*:*" // control endpoint
			, "tcp://*:*" // data endpoint
		}
	{}
};

static inline auto recv_message(zmq::socket_t& socket, zmq::recv_flags const sockopt = zmq::recv_flags::none)
{
	while (true) {
		auto received_result = zmq::message_t{};
		auto maybe_bytes = socket.recv(received_result, sockopt);
		if (!maybe_bytes) {
			continue;
		}

		return received_result;
	}
}

static auto replace_endpoint_host(std::string endpoint, std::string const& hostname)
{
	// Extract port from generated endpoint
	endpoint = endpoint.substr(endpoint.find("://") + 3);
	endpoint = endpoint.substr(endpoint.find(":") + 1);

	return "tcp://" + hostname + ":" + std::move(endpoint);
};

// Replace the sockets' endpoints with e.g. known-accessible hostname from CTI
static auto formatEndpoints(TcpDownstreamLink const& downstream, std::string const& hostname)
{
	return std::make_tuple(
		replace_endpoint_host(downstream.controlEndpoint, hostname),
		replace_endpoint_host(downstream.dataEndpoint,    hostname)
	);
}

using StreamId = int;

struct Topology
{
	enum : int
		{ Fcfs
		, Layered
	} launch;

	int fanout;
};

class Frontend
{
public:
	TcpDownstreamLink m_downstream;

	Frontend(zmq::context_t& ctx, int const app_id, long int const session_id, long int const manifest_id,
		char const* const* leaf_argv, Topology const& topology);

	void registerStream(StreamId const& streamId, std::string const& filterName);

	std::string runCommand(StreamId const& streamId, std::string const& command);
	void runExit();
};

class Backend
{
public:
	static auto getParams(int argc, char const* const* argv)
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

	UpstreamLink m_upstream;
	int m_rank;

	Backend(zmq::context_t& ctx, std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank);

	std::tuple<StreamId, std::string> nextCommand();

	void sendResult(StreamId const& streamId, std::string const& result);
};

