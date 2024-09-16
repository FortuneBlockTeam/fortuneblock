/*
 * Copyright (c) 2024 The Fortuneblock developer
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 * 
 * FortunePayment.cpp
 */

#include <fortune_payment.h>
#include <rpc/server.h>

#include <util/system.h>
#include <chainparams.h>
#include <boost/foreach.hpp>
#include <key_io.h>
#include <validation.h>
#include <uint256.h>

CAmount FortunePayment::getFortunePaymentAmount(int blockHeight, CAmount blockReward) {
    if (blockHeight <= startBlock) {
        return 0;
    }
    for (int i = 0; i < rewardStructures.size(); i++) {
        FortuneRewardStructure rewardStructure = rewardStructures[i];
        if (rewardStructure.blockHeight == INT_MAX || blockHeight <= rewardStructure.blockHeight) {
            return blockReward * rewardStructure.rewardPercentage / 100;
        }
    }
    return 0;
}

void FortunePayment::GetFortuneAddressByHeight(int blockHeight)
{
    
    if (blockHeight <= 0 || blockHeight > ChainActive().Height()) {
        LogPrintf("Invalid block height: %d\n", blockHeight);
        fortuneAddress = DEFAULT_FORTUNE_ADDRESS;
        return;
    }

    CBlockIndex* pblockindex = ChainActive()[blockHeight];
    if (!pblockindex) {
        LogPrintf("Block index not found for height: %d\n", blockHeight);
        fortuneAddress = DEFAULT_FORTUNE_ADDRESS;
        return;
    }
    int luckyHeight = static_cast<int>(pblockindex->GetBlockHash().GetUint64(0) % ChainActive().Height());

    fortuneAddress = GetBlockCoinbaseMinerAddress(luckyHeight);

}

void FortunePayment::FillFortunePayment(CMutableTransaction &txNew, int nBlockHeight, CAmount blockReward,
                                        CTxOut &txoutFortuneRet) {

    CAmount fortunePayment = getFortunePaymentAmount(nBlockHeight, blockReward);
    txoutFortuneRet = CTxOut();

    GetFortuneAddressByHeight(nBlockHeight - 1);

    CTxDestination fortuneAddr = DecodeDestination(fortuneAddress);
    if (!IsValidDestination(fortuneAddr))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           strprintf("Invalid Fortune Address: %s", fortuneAddress.c_str()));
    CScript payee = GetScriptForDestination(fortuneAddr);

    txNew.vout[0].nValue -= fortunePayment;
    txoutFortuneRet = CTxOut(fortunePayment, payee);
    txNew.vout.push_back(txoutFortuneRet);
    LogPrintf("FortunePayment::FillFortunePayment -- Fortune payment %lld to %s\n", fortunePayment,
              fortuneAddress.c_str());
}

bool FortunePayment::IsBlockPayeeValid(const CTransaction &txNew, const int height, const CAmount blockReward) {

    GetFortuneAddressByHeight(height);

    CScript payee = GetScriptForDestination(DecodeDestination(fortuneAddress));
    const CAmount fortuneReward = getFortunePaymentAmount(height, blockReward);
    BOOST_FOREACH(
    const CTxOut &out, txNew.vout) {
        if (out.scriptPubKey == payee && out.nValue >= fortuneReward) {
            return true;
        }
    }

    return false;
}



