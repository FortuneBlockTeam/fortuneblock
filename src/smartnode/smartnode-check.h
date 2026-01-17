// Copyright (c) 2026 The Fortuneblock developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "net.h"
#include "netaddress.h"
#include <netbase.h>

bool CheckSmartnodeInboundReachable(
    const CService& service,
    int64_t nTimeout);
