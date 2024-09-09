/*
 * Copyright (c) 2024 The Fortuneblock developer
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 * 
 * FortunePayment.h

 */

#ifndef SRC_FORTUNE_PAYMENT_H_
#define SRC_FORTUNE_PAYMENT_H_

#include <string>
#include <amount.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <limits.h>
//using namespace std;

static const std::string DEFAULT_FORTUNE_ADDRESS = "FkqwU8tUU6U41PuGTPUqy93hML4QtCDPDX";
struct FortuneRewardStructure {
    int blockHeight;
    int rewardPercentage;
};

class FortunePayment {
public:
    FortunePayment(std::vector <FortuneRewardStructure> rewardStructures = {}, int startBlock = 0,
                   const std::string &address = DEFAULT_FORTUNE_ADDRESS) {
        this->fortuneAddress = address;
        this->startBlock = startBlock;
        this->rewardStructures = rewardStructures;
    }

    ~FortunePayment() {};

    CAmount getFortunePaymentAmount(int blockHeight, CAmount blockReward);

    void GetFortuneAddressByHeight(int blockHeight);

    void FillFortunePayment(CMutableTransaction &txNew, int nBlockHeight, CAmount blockReward, CTxOut &txoutFortuneRet);

    bool IsBlockPayeeValid(const CTransaction &txNew, const int height, const CAmount blockReward);

    int getStartBlock() { return this->startBlock; }

private:
    std::string fortuneAddress;
    int startBlock;
    std::vector <FortuneRewardStructure> rewardStructures;
};


#endif /* SRC_FORTUNE_PAYMENT_H_ */
