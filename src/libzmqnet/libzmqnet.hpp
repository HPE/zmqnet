/*
 * Copyright 2011-2020 Hewlett Packard Enterprise Development LP.
 */

#include <sys/time.h>

#include <tuple>

#include <zmq.hpp>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "libzmqnet/NetworkSchema.capnp.h"

static inline auto get_milliseconds()
{
	struct timeval tv{};
	::gettimeofday(&tv, nullptr);
	return (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
}

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
	auto message = recv_message(socket, sockopt);
	return ReaderStorage{std::move(message)};
}
