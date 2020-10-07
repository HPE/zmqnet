/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <tuple>

#include <zmq.hpp>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "libzmqnet/NetworkSchema.capnp.h"

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

	return capnp::FlatArrayMessageReader(kj::arrayPtr(messageWords, messageWordsLen));
}

static inline auto recv_MessageReader(zmq::socket_t& socket, zmq::recv_flags const sockopt = zmq::recv_flags::none)
{
	auto message = recv_message(socket, sockopt);
	auto messageReader = make_MessageReader(message);

	return std::make_tuple(std::move(message), std::move(messageReader));
}

template <typename RootType>
static inline auto recv_MessageRoot(zmq::socket_t& socket, zmq::recv_flags const sockopt = zmq::recv_flags::none)
{
	auto [result, resultReader] = recv_MessageReader(socket, sockopt);
	auto rootReader = resultReader.getRoot<RootType>();
	return std::make_tuple(std::move(result), std::move(resultReader), rootReader);
}
