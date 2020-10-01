/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#pragma once

#include <uuid/uuid.h>

#include <vector>
#include <string>

#include <zmq.hpp>

static inline std::string generate_uuid()
{
	uuid_t id;
	uuid_generate(id);

	char uuid_str[37];
	uuid_unparse(id, uuid_str);

	return std::string{uuid_str};
}

static inline std::string replace_endpoint_host(std::string&& endpoint, std::string const& hostname)
{
	// Extract port from generated endpoint
	endpoint = endpoint.substr(endpoint.find("://") + 3);
	endpoint = endpoint.substr(endpoint.find(":") + 1);

	return "tcp://" + hostname + ":" + std::move(endpoint);
};

template <typename SockOpt>
static inline void send_string(zmq::socket_t& socket, std::string const& message, SockOpt&& sockopt)
{
	socket.send(zmq::message_t{message.c_str(), message.length()}, std::forward<SockOpt>(sockopt));
}

template <typename SockOpt>
static inline std::string recv_string(zmq::socket_t& socket, SockOpt&& sockopt)
{
	while (true) {
		auto received_result = zmq::message_t{};
		auto maybe_bytes = socket.recv(received_result, std::forward<SockOpt>(sockopt));
		if (!maybe_bytes) {
			continue;
		}

		return received_result.to_string();
	}
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

template <typename DownstreamLinkImpl>
class ConsumerNode
{
public:
	DownstreamLinkImpl downstreamLink;

	ConsumerNode(zmq::context_t& ctx)
		: downstreamLink{ctx}
	{}

	auto getEndpoints() const
	{
		return std::make_pair(downstreamLink.controlEndpoint, downstreamLink.dataEndpoint);
	}

	void children_ready_loop(size_t num_children)
	{
		for (size_t connected_children = 0; connected_children < num_children;) {
			auto const ready_result = recv_string(downstreamLink.data_socket, zmq::recv_flags::none);
			assert(ready_result == "ready");
			connected_children++;
		}
	}

	auto send_command(std::string const& command)
	{
		send_string(downstreamLink.control_socket, command, zmq::send_flags::none);
	}

	auto recv_results(size_t num_children)
	{
		auto children_results = std::vector<std::string>{};

		while (children_results.size() < num_children) {
			children_results.emplace_back(recv_string(downstreamLink.data_socket, zmq::recv_flags::none));
		}

		return children_results;
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

	// Replace the sockets' endpoints with e.g. known-accessible hostname from CTI
	static auto formatEndpoints(ConsumerNode<TcpDownstreamLink> const& node, std::string const& hostname)
	{
		auto [controlEndpoint, dataEndpoint] = node.getEndpoints();

		return std::make_pair(
			replace_endpoint_host(std::move(controlEndpoint), hostname),
			replace_endpoint_host(std::move(dataEndpoint),    hostname)
		);
	}
};

class ProducerNode
{
public:
	UpstreamLink upstreamLink;

	void connect(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint)
	{
		upstreamLink.connect(upstreamControlEndpoint, upstreamDataEndpoint);
	}

	ProducerNode(zmq::context_t& ctx)
		: upstreamLink{ctx}
	{}

	ProducerNode(zmq::context_t& ctx,
		std::string const& controlEndpoint, std::string const& dataEndpoint)
		: ProducerNode{ctx}
	{
		connect(controlEndpoint, dataEndpoint);
	}

	void send_ready()
	{
		send_string(upstreamLink.data_socket, "ready", zmq::send_flags::none);
	}

	auto recv_command()
	{
		return recv_string(upstreamLink.control_socket, zmq::recv_flags::none);
	}

	auto send_result(std::string const& result)
	{
		send_string(upstreamLink.data_socket, result, zmq::send_flags::none);
	}
};

using LeafNode = ProducerNode;

class Frontend
{
public:
	ConsumerNode<TcpDownstreamLink> m_downstream;

	Frontend(zmq::context_t& ctx, int const app_id, int const manifest_id,
		int const fanout, char const* const* leaf_argv);

	std::string runCommand(std::string const& command);
	void runExit();
};

class Commnode
{
public:
	ProducerNode m_upstream;
	ConsumerNode<TcpDownstreamLink> m_downstream;

	int m_numChildren;
	std::vector<pid_t> m_localChildPids;

	Commnode(zmq::context_t& ctx, std::string const& dealerEndpoint, char const* const* leaf_argv);

	~Commnode();

	std::string nextCommand();

	std::vector<std::string> collectResults();
	void sendResult(std::string const& result);
};
