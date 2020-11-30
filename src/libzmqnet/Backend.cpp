/*
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

// External interface
#include "zmqnet.hpp"

// Internal helpers and classes
#include "libzmqnet.hpp"

zmqnet::Backend::Backend(std::string const& upstreamControlEndpoint, std::string const& upstreamDataEndpoint, int rank)
	: m_ctx{}
	, m_upstream{std::make_unique<net::UpstreamLink>(m_ctx, upstreamControlEndpoint, upstreamDataEndpoint)}
	, m_rank{rank}
{
	// Notify parent commnode that setup is completed
	log("rank %d: notifying parent ready from leaf node\n", m_rank);
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	response.setReadyAck();

	// Send acknowledgement
	net::send_MessageBuilder(m_upstream->data_socket, builder);
}

 zmqnet::Backend::~Backend()
 {
 	// Generate destructor for forward-declared net::UpstreamLink
 }

std::tuple<zmqnet::StreamId, std::string> zmqnet::Backend::nextCommand()
{
	while (true) {
		// Receive next network command
		log("rank %d: waiting for next command...\n", m_rank);
		auto reader = net::recv_MessageReader(m_upstream->control_socket);
		auto command = reader.getRoot<NetworkCommand>();

		// Detect exit case
		if (command.isExit()) {
			log("rank %d: got exit\n", m_rank);

			// Send exit acknowledgement to parent
			{ auto builder = capnp::MallocMessageBuilder{};
				auto response = builder.initRoot<NetworkResponse>();
				response.setExitAck();

				net::send_MessageBuilder(m_upstream->data_socket, builder);
			}
			m_upstream->control_socket.close();
			m_upstream->data_socket.close();

			return std::make_tuple(-1, std::string{});

		// Acknowledge filter case
		} else if (command.isNewStream()) {
			log("rank %d: filter response\n", m_rank);
			auto builder = capnp::MallocMessageBuilder{};
			auto response = builder.initRoot<NetworkResponse>();
			auto streamAck = response.initStreamAck();
			streamAck.setStreamId(command.getNewStream().getNewStreamId());

			net::send_MessageBuilder(m_upstream->data_socket, builder);

			continue;

		} else if (command.isStartLeaves()) {
			log("rank %d: unexpected: startLeaves\n", m_rank);
			continue;

		// Extract stream ID and user command
		} else {
			log("rank %d: user command\n", m_rank);
			assert(command.isUserCommand());
			auto userCommand = command.getUserCommand();
			return std::make_tuple(userCommand.getStreamId(), std::string{userCommand.getContent().cStr()});
		}
	}
}

void zmqnet::Backend::sendResult(zmqnet::StreamId const& streamId, char const* result_buffer, size_t result_len)
{
	// Notify parent commnode of result
	auto builder = capnp::MallocMessageBuilder{};
	auto response = builder.initRoot<NetworkResponse>();
	auto userResult = response.initUserResult();
	userResult.setStreamId(streamId);
	if (result_len > 0) {
		userResult.setContent(kj::ArrayPtr<kj::byte const>{(unsigned char const*)result_buffer, result_len});
	}

	// Send result
	net::send_MessageBuilder(m_upstream->data_socket, builder);
}
