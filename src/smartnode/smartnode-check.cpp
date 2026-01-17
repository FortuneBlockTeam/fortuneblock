// Copyright (c) 2026 The Fortuneblock developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "smartnode/smartnode-check.h"

bool CheckSmartnodeInboundReachable(
    const CService& service,
    int64_t nTimeout)
{
    SOCKET hSocket = CreateSocket(service);
    if (hSocket == INVALID_SOCKET) {
        return false;
    }

    bool fConnected =
        ConnectSocketDirectly(service, hSocket, nTimeout, true) &&
        IsSelectableSocket(hSocket);

    CloseSocket(hSocket);
    return fConnected;
}
