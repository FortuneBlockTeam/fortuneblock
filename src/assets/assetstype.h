// Copyright (c) 2021-2023 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FORTUNEBLOCK_ASSETS_FEES_H
#define FORTUNEBLOCK_ASSETS_FEES_H

#include <amount.h>
#include <coins.h>
#include <key_io.h>

#define FTB_R 0x72 //R
#define FTB_T 0x74 //T
#define FTB_M 0x6d //M
#define MAX_UNIQUE_ID 0xffffffffffffffff

class CAssetTransfer {
public:
    std::string assetId;
    bool isUnique = false;
    uint64_t uniqueId = MAX_UNIQUE_ID; //default it to MAX_UNIQUE_ID
    CAmount nAmount;

    CAssetTransfer() {
        SetNull();
    }

    void SetNull() {
        assetId = "";
        isUnique = false;
        uniqueId = MAX_UNIQUE_ID;
        nAmount = 0;
    }

    SERIALIZE_METHODS(CAssetTransfer, obj) {
        READWRITE(obj.assetId, obj.isUnique);
        if (obj.isUnique) {
            READWRITE(obj.uniqueId);
        }
        READWRITE(obj.nAmount);
    }


    CAssetTransfer(const std::string &assetId, const CAmount &nAmount, const uint64_t &uniqueId);

    CAssetTransfer(const std::string &assetId, const CAmount &nAmount);

    void BuildAssetTransaction(CScript &script) const;
};

bool GetTransferAsset(const CScript &script, CAssetTransfer &assetTransfer);


#endif //FORTUNEBLOCK_ASSETS_FEES_H