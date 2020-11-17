/******************************************************************************\
 * argv_defs.hpp - Command line argument defines
 *
 * Copyright 2020 Hewlett Packard Enterprise Development LP.
 *
 * Unpublished Proprietary Information.
 * This unpublished work is protected to trade secret, copyright and other laws.
 * Except as permitted by contract or express written permission of Hewlett Packard Enterprise Development LP.,
 * no part of this work or its content may be used, reproduced or disclosed
 * in any form.
 *
 ******************************************************************************/

#pragma once

// Strong argv defs below
#include "cti_argv.hpp"

struct CommnodeArgv : public cti::Argv {
    using Option    = cti::Argv::Option;
    using Parameter = cti::Argv::Parameter;

    static constexpr Option Help   { "help",   'h' };
    static constexpr Option Packed { "packed", 'p' };

    static constexpr Parameter DealerEndpoint    { "dealer-endpoint",     'd' };
    static constexpr Parameter NodeConnectionMap { "node-connection-map", 'c' };
    static constexpr Parameter PackedRank { "packed-rank", 'r' };

    static constexpr GNUOption long_options[] = {
        Help, Packed,
        DealerEndpoint, NodeConnectionMap, PackedRank,
        long_options_done
    };
};

