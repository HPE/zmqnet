/*
 * Copyright 2021 Hewlett Packard Enterprise Development LP.
 *
 * Unpublished Proprietary Information.
 * This unpublished work is protected to trade secret, copyright and other laws.
 * Except as permitted by contract or express written permission of Hewlett Packard Enterprise Development LP.,
 * no part of this work or its content may be used, reproduced or disclosed
 * in any form.
 */

#include "cti_argv.hpp"
#include "zmqnet.hpp"

struct BackendArgv : public cti::Argv {
	using Option    = cti::Argv::Option;
	using Parameter = cti::Argv::Parameter;

	static constexpr Parameter Sync { "sync", 's' };
	static constexpr Parameter TimeoutNodeId { "timeout-node-id", 't' };

	static constexpr GNUOption long_options[] = {
		Sync, TimeoutNodeId,
		long_options_done
	};
};

static constexpr auto TestStreamId = zmqnet::Stream::Id{1};
