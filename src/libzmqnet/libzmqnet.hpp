/*
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 */

#pragma once

#include <uuid/uuid.h>

#include <vector>
#include <string>
#include <tuple>

// Logging

#include <sys/time.h>

// ZMQ and network

#include <zmq.hpp>

// Capnp and application protocol

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "libzmqnet/NetworkSchema.capnp.h"

// Logging

static inline auto get_milliseconds()
{
	struct timeval tv{};
	::gettimeofday(&tv, nullptr);
	return (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
}

static inline void log(const char* format, ...)
{
	static const auto start_ms = get_milliseconds();
	va_list argptr;
	va_start(argptr, format);
	fprintf(stderr, "[%5d %6ld] ", getpid(), get_milliseconds() - start_ms);
	vfprintf(stderr, format, argptr);
	va_end(argptr);
}

namespace net
{

// ZMQ and network

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

static inline auto replace_endpoint_host(std::string endpoint, std::string const& hostname)
{
	// Extract port from generated endpoint
	endpoint = endpoint.substr(endpoint.find("://") + 3);
	endpoint = endpoint.substr(endpoint.find(":") + 1);

	return "tcp://" + hostname + ":" + std::move(endpoint);
};

// Replace the sockets' endpoints with e.g. known-accessible hostname from CTI
static inline auto formatEndpoints(TcpDownstreamLink const& downstream, std::string const& hostname)
{
	return std::make_tuple(
		replace_endpoint_host(downstream.controlEndpoint, hostname),
		replace_endpoint_host(downstream.dataEndpoint,    hostname)
	);
}

// Capnp and application protocol

static inline void send_MessageBuilder(zmq::socket_t& socket, capnp::MessageBuilder& builder, zmq::send_flags const sockopt = zmq::send_flags::none)
{
	auto messageArray = capnp::messageToFlatArray(builder);
	auto const messageBytes = messageArray.asBytes();
	socket.send(zmq::message_t{messageBytes.begin(), messageBytes.size()}, sockopt);
}

static inline auto make_MessageReader(zmq::message_t& message)
{
	assert(message.size() % sizeof(capnp::word) == 0);
	auto const messageWords = message.data<capnp::word>();
	auto const messageWordsLen = message.size() / sizeof(capnp::word);

	return capnp::UnalignedFlatArrayMessageReader(kj::arrayPtr(messageWords, messageWordsLen));
}

// Store required message / Reader state in custom object to avoid dropped backing storage
// Note: Capnp Reader arenas apparently do not move correctly
class ReaderStorage
{
public:
	zmq::message_t message;
	capnp::UnalignedFlatArrayMessageReader reader;

	static auto make_Reader(zmq::message_t& message) {
		assert(message.size() % sizeof(capnp::word) == 0);
		auto const messageWords = message.data<capnp::word>();
		auto const messageWordsLen = message.size() / sizeof(capnp::word);
		auto messageArray = kj::arrayPtr(messageWords, messageWordsLen);
		return capnp::UnalignedFlatArrayMessageReader(messageArray);
	}

	ReaderStorage(zmq::message_t&& message_)
		: message{std::move(message_)}
		, reader{make_Reader(message)}
	{}

	template <typename T>
	auto getRoot() { return reader.getRoot<T>(); }
};

static inline auto recv_MessageReader(zmq::socket_t& socket, zmq::recv_flags const sockopt = zmq::recv_flags::none)
{
	auto message = net::recv_message(socket, sockopt);
	return ReaderStorage{std::move(message)};
}

} // net
