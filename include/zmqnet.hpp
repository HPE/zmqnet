/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#pragma once

#include <uuid/uuid.h>

#include <memory>
#include <string>
#include <optional>

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

	UpstreamLink(zmq::context_t& ctx,
		std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint)
		: control_socket{ctx, zmq::socket_type::sub}
		, data_socket{ctx, zmq::socket_type::push}
	{
		// Connect to upstream sockets
		control_socket.connect(upstreamControlEndpoint);
		data_socket.connect(upstreamDataEndpoint);

		// Subscribe to all topics
		control_socket.set(zmq::sockopt::subscribe, "");
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

template <typename DownstreamLinkImpl>
class Rootnode
{
public:
	DownstreamLinkImpl downstreamLink;

	Rootnode(zmq::context_t& ctx)
		: downstreamLink{ctx}
	{}

	auto getControlEndpoint() const { return downstreamLink.controlEndpoint; }
	auto getDataEndpoint()    const { return downstreamLink.dataEndpoint; }

	void children_ready_loop(size_t num_children)
	{
		for (size_t connected_children = 0; connected_children < num_children;) {
			auto received_result = zmq::message_t{};
			auto maybe_bytes = downstreamLink.data_socket.recv(received_result, zmq::recv_flags::none);
			if (!maybe_bytes) {
				continue;
			}

			assert(received_result.to_string_view() == "ready");
			connected_children++;
		}
	}

	auto send_command(std::string const& command)
	{
		downstreamLink.control_socket.send(zmq::message_t{command.c_str(), command.length()}, zmq::send_flags::none);
	}

	auto recv_results(size_t num_children)
	{
		auto children_results = std::vector<std::string>{};

		while (children_results.size() < num_children) {
			auto received_result = zmq::message_t{};
			auto maybe_bytes = downstreamLink.data_socket.recv(received_result, zmq::recv_flags::none);

			if (!maybe_bytes) {
				continue;
			}

			children_results.emplace_back(received_result.to_string_view());
		}

		return children_results;
	}
};

template <typename DownstreamLinkImpl>
class Commnode : public Rootnode<DownstreamLinkImpl>
{
public:
	UpstreamLink upstreamLink;

	Commnode(zmq::context_t& ctx,
		std::string const& controlEndpoint_, std::string const& dataEndpoint_)
		: Rootnode<DownstreamLinkImpl>{ctx}
		, upstreamLink{ctx, controlEndpoint_, dataEndpoint_}
	{}

	void send_ready()
	{
		upstreamLink.data_socket.send(zmq::str_buffer("ready"), zmq::send_flags::none);
	}

	auto recv_command()
	{
		while (true) {
			auto recieved_command = zmq::message_t{};
			auto maybe_bytes = upstreamLink.control_socket.recv(recieved_command, zmq::recv_flags::none);

			if (!maybe_bytes) {
				continue;
			}

			return recieved_command.to_string();
		}
	}

	auto send_result(std::string const& result)
	{
		upstreamLink.data_socket.send(zmq::message_t{result.c_str(), result.length()}, zmq::send_flags::none);
	}
};
