// Copyright (c) 2018-2020 The Dash Core developers
// Copyright (c) 2020-2023 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FORTUNEBLOCK_DETERMINISTICMNS_H
#define FORTUNEBLOCK_DETERMINISTICMNS_H

#include <arith_uint256.h>
#include <consensus/params.h>
#include <evo/evodb.h>
#include <evo/providertx.h>
#include <saltedhasher.h>
#include <scheduler.h>
#include <sync.h>

#if defined(MAC_OSX)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <immer/map.hpp>

#include <unordered_map>
#include <utility>

#if defined(MAC_OSX)
#pragma clang diagnostic pop
#endif

class CConnman;

class CBlock;

class CBlockIndex;

class CValidationState;

class CSimplifiedMNListDiff;

extern RecursiveMutex cs_main;

namespace llmq {
    class CFinalCommitment;
} // namespace llmq

class CDeterministicMNState {
private:
    int nPoSeBanHeight{-1};

    friend class CDeterministicMNStateDiff;

public:
    int nRegisteredHeight{-1};
    int nLastPaidHeight{0};
    int nPoSePenalty{0};
    int nPoSeRevivedHeight{-1};
    uint16_t nRevocationReason{CProUpRevTx::REASON_NOT_SPECIFIED};

    // the block hash X blocks after registration, used in quorum calculations
    uint256 confirmedHash;
    // sha256(proTxHash, confirmedHash) to speed up quorum calculations
    // please note that this is NOT a double-sha256 hash
    uint256 confirmedHashWithProRegTxHash;
    //collateral amount used in quorum calculations
    CAmount nCollateralAmount;

    CKeyID keyIDOwner;
    CBLSLazyPublicKey pubKeyOperator;
    CKeyID keyIDVoting;
    CService addr;
    CScript scriptPayout;
    CScript scriptOperatorPayout;

    CDeterministicMNState() = default;

    explicit CDeterministicMNState(const CProRegTx &proTx) :
            keyIDOwner(proTx.keyIDOwner),
            keyIDVoting(proTx.keyIDVoting),
            addr(proTx.addr),
            scriptPayout(proTx.scriptPayout) {
        pubKeyOperator.Set(proTx.pubKeyOperator);
    }

    template<typename Stream>
    CDeterministicMNState(deserialize_type, Stream &s) {
        s >> *this;
    }

    SERIALIZE_METHODS(CDeterministicMNState, obj
    )
    {
        READWRITE(obj.nRegisteredHeight, obj.nLastPaidHeight, obj.nPoSePenalty, obj.nPoSeRevivedHeight,
                  obj.nPoSeBanHeight, obj.nRevocationReason, obj.confirmedHash, obj.confirmedHashWithProRegTxHash,
                  obj.keyIDOwner, obj.pubKeyOperator, obj.keyIDVoting, obj.addr,
                  obj.scriptPayout, obj.scriptOperatorPayout, obj.nCollateralAmount);

    }

    void ResetOperatorFields() {
        pubKeyOperator.Set(CBLSPublicKey());
        addr = CService();
        scriptOperatorPayout = CScript();
        nRevocationReason = CProUpRevTx::REASON_NOT_SPECIFIED;
    }

    void BanIfNotBanned(int height) {
        if (!IsBanned()) {
            nPoSeBanHeight = height;
        }
    }

    int GetBannedHeight() const {
        return nPoSeBanHeight;
    }

    bool IsBanned() const {
        return nPoSeBanHeight != -1;
    }

    void Revive(int nRevivedHeight) {
        nPoSePenalty = 0;
        nPoSeBanHeight = -1;
        nPoSeRevivedHeight = nRevivedHeight;
    }

    void UpdateConfirmedHash(const uint256 &_proTxHash, const uint256 &_confirmedHash) {
        confirmedHash = _confirmedHash;
        CSHA256 h;
        h.Write(_proTxHash.begin(), _proTxHash.size());
        h.Write(_confirmedHash.begin(), _confirmedHash.size());
        h.Finalize(confirmedHashWithProRegTxHash.begin());
    }

public:
    std::string ToString() const;

    void ToJson(UniValue &obj) const;
};

using CDeterministicMNStatePtr = std::shared_ptr<CDeterministicMNState>;
using CDeterministicMNStateCPtr = std::shared_ptr<const CDeterministicMNState>;

class CDeterministicMNStateDiff {
public:
    enum Field : uint32_t {
        Field_nRegisteredHeight = 0x0001,
        Field_nLastPaidHeight = 0x0002,
        Field_nPoSePenalty = 0x0004,
        Field_nPoSeRevivedHeight = 0x0008,
        Field_nPoSeBanHeight = 0x0010,
        Field_nRevocationReason = 0x0020,
        Field_confirmedHash = 0x0040,
        Field_confirmedHashWithProRegTxHash = 0x0080,
        Field_keyIDOwner = 0x0100,
        Field_pubKeyOperator = 0x0200,
        Field_keyIDVoting = 0x0400,
        Field_addr = 0x0800,
        Field_scriptPayout = 0x1000,
        Field_scriptOperatorPayout = 0x2000,
        Field_nCollateralAmount = 0x4000,
    };

#define DMN_STATE_DIFF_ALL_FIELDS \
    DMN_STATE_DIFF_LINE(nRegisteredHeight) \
    DMN_STATE_DIFF_LINE(nLastPaidHeight) \
    DMN_STATE_DIFF_LINE(nPoSePenalty) \
    DMN_STATE_DIFF_LINE(nPoSeRevivedHeight) \
    DMN_STATE_DIFF_LINE(nPoSeBanHeight) \
    DMN_STATE_DIFF_LINE(nRevocationReason) \
    DMN_STATE_DIFF_LINE(confirmedHash) \
    DMN_STATE_DIFF_LINE(confirmedHashWithProRegTxHash) \
    DMN_STATE_DIFF_LINE(keyIDOwner) \
    DMN_STATE_DIFF_LINE(pubKeyOperator) \
    DMN_STATE_DIFF_LINE(keyIDVoting) \
    DMN_STATE_DIFF_LINE(addr) \
    DMN_STATE_DIFF_LINE(scriptPayout) \
    DMN_STATE_DIFF_LINE(scriptOperatorPayout) \
    DMN_STATE_DIFF_LINE(nCollateralAmount)

public:
    uint32_t fields{0};
    // we reuse the state class, but only the members as noted by fields are valid
    CDeterministicMNState state;

public:
    CDeterministicMNStateDiff() = default;

    CDeterministicMNStateDiff(const CDeterministicMNState &a, const CDeterministicMNState &b) {
#define DMN_STATE_DIFF_LINE(f) if (a.f != b.f) { state.f = b.f; fields |= Field_##f; }
        DMN_STATE_DIFF_ALL_FIELDS
#undef DMN_STATE_DIFF_LINE
    }

    SERIALIZE_METHODS(CDeterministicMNStateDiff, obj
    )
    {
        READWRITE(VARINT(obj.fields));
#define DMN_STATE_DIFF_LINE(f) if (obj.fields & Field_##f) READWRITE(obj.state.f);
        DMN_STATE_DIFF_ALL_FIELDS
#undef DMN_STATE_DIFF_LINE
    }

    void ApplyToState(CDeterministicMNState &target) const {
#define DMN_STATE_DIFF_LINE(f) if (fields & Field_##f) target.f = state.f;
        DMN_STATE_DIFF_ALL_FIELDS
#undef DMN_STATE_DIFF_LINE
    }
};

class CDeterministicMN {
private:
    uint64_t internalId{std::numeric_limits<uint64_t>::max()};

public:
    CDeterministicMN() = delete; // no default constructor, must specify internalId
    explicit CDeterministicMN(uint64_t _internalId) : internalId(_internalId) {
        // only non-initial values
        assert(_internalId != std::numeric_limits<uint64_t>::max());
    }

    // TODO: can be removed in a future version
    CDeterministicMN(CDeterministicMN mn, uint64_t _internalId) : CDeterministicMN(std::move(mn)) {
        // only non-initial values
        assert(_internalId != std::numeric_limits<uint64_t>::max());
        internalId = _internalId;
    }

    template<typename Stream>
    CDeterministicMN(deserialize_type, Stream &s) {
        s >> *this;
    }

    uint256 proTxHash;
    COutPoint collateralOutpoint;
    uint16_t nOperatorReward{0};
    std::shared_ptr<const CDeterministicMNState> pdmnState;

    template<typename Stream, typename Operation>
    inline void SerializationOp(Stream &s, Operation ser_action, bool oldFormat) {
        READWRITE(proTxHash);
        if (!oldFormat) {
            READWRITE(VARINT(internalId));
        }
        READWRITE(collateralOutpoint);
        READWRITE(nOperatorReward);
        READWRITE(pdmnState);
    }

    template<typename Stream>
    void Serialize(Stream &s) const {
        const_cast<CDeterministicMN *>(this)->SerializationOp(s, CSerActionSerialize(), false);
    }

    template<typename Stream>
    void Unserialize(Stream &s, bool oldFormat = false) {
        SerializationOp(s, CSerActionUnserialize(), oldFormat);
    }

    [[nodiscard]] uint64_t GetInternalId() const;

    [[nodiscard]] std::string ToString() const;

    void ToJson(UniValue &obj) const;
};

using CDeterministicMNCPtr = std::shared_ptr<const CDeterministicMN>;

class CDeterministicMNListDiff;

template<typename Stream, typename K, typename T, typename Hash, typename Equal>
void SerializeImmerMap(Stream &os, const immer::map <K, T, Hash, Equal> &m) {
    WriteCompactSize(os, m.size());
    for (typename immer::map<K, T, Hash, Equal>::const_iterator mi = m.begin(); mi != m.end(); ++mi)
        Serialize(os, (*mi));
}

template<typename Stream, typename K, typename T, typename Hash, typename Equal>
void UnserializeImmerMap(Stream &is, immer::map <K, T, Hash, Equal> &m) {
    m = immer::map<K, T, Hash, Equal>();
    unsigned int nSize = ReadCompactSize(is);
    for (unsigned int i = 0; i < nSize; i++) {
        std::pair <K, T> item;
        Unserialize(is, item);
        m = m.set(item.first, item.second);
    }
}

// For some reason the compiler is not able to choose the correct Serialize/Deserialize methods without a specialized
// version of SerReadWrite. It otherwise always chooses the version that calls a.Serialize()
template<typename Stream, typename K, typename T, typename Hash, typename Equal>
inline void SerReadWrite(Stream &s, const immer::map <K, T, Hash, Equal> &m, CSerActionSerialize ser_action) {
    ::SerializeImmerMap(s, m);
}

template<typename Stream, typename K, typename T, typename Hash, typename Equal>
inline void SerReadWrite(Stream &s, immer::map <K, T, Hash, Equal> &obj, CSerActionUnserialize ser_action) {
    ::UnserializeImmerMap(s, obj);
}

class CDeterministicMNList {
public:
    using MnMap = immer::map<uint256, CDeterministicMNCPtr>;
    using MnInternalIdMap = immer::map<uint64_t, uint256>;
    using MnUniquePropertyMap = immer::map <uint256, std::pair<uint256, uint32_t>>;

private:
    uint256 blockHash;
    int nHeight{-1};
    uint32_t nTotalRegisteredCount{0};
    MnMap mnMap;
    MnInternalIdMap mnInternalIdMap;

    // map of unique properties like address and keys
    // we keep track of this as checking for duplicates would otherwise be painfully slow
    MnUniquePropertyMap mnUniquePropertyMap;

public:
    CDeterministicMNList() = default;

    explicit CDeterministicMNList(const uint256 &_blockHash, int _height, uint32_t _totalRegisteredCount) :
            blockHash(_blockHash),
            nHeight(_height),
            nTotalRegisteredCount(_totalRegisteredCount) {
    }

    template<typename Stream, typename Operation>
    inline void SerializationOpBase(Stream &s, Operation ser_action) {
        READWRITE(blockHash);
        READWRITE(nHeight);
        READWRITE(nTotalRegisteredCount);
    }

#if defined(MAC_OSX)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

    template<typename Stream>
    void Serialize(Stream &s) const {
        const_cast<CDeterministicMNList *>(this)->SerializationOpBase(s, CSerActionSerialize());
        // Serialize the map as a vector
        WriteCompactSize(s, mnMap.size());
        for (const auto &p: mnMap) {
            s << *p.second;
        }
    }

    template<typename Stream>
    void Unserialize(Stream &s) {
        mnMap = MnMap();
        mnUniquePropertyMap = MnUniquePropertyMap();
        mnInternalIdMap = MnInternalIdMap();

        SerializationOpBase(s, CSerActionUnserialize());

        size_t cnt = ReadCompactSize(s);
        for (size_t i = 0; i < cnt; i++) {
            AddMN(std::make_shared<CDeterministicMN>(deserialize, s), false);
        }
    }

    [[nodiscard]] size_t GetAllMNsCount() const {
        return mnMap.size();
    }

    [[nodiscard]] size_t GetValidMNsCount() const {
        size_t count = 0;
        for (const auto &p: mnMap) {
            if (IsMNValid(p.second)) {
                count++;
            }
        }
        return count;
    }

    template<typename Callback>
    void ForEachMN(bool onlyValid, int height, Callback &&cb) const {
        for (const auto &p: mnMap) {
            if (!onlyValid || IsMNValid(p.second, height)) {
                cb(p.second);
            }
        }
    }

    template<typename Callback>
    void ForEachMN(bool onlyValid, Callback &&cb) const {
        for (const auto &p: mnMap) {
            if (!onlyValid || IsMNValid(p.second)) {
                cb(p.second);
            }
        }
    }

    [[nodiscard]] const uint256 &GetBlockHash() const {
        return blockHash;
    }

    void SetBlockHash(const uint256 &_blockHash) {
        blockHash = _blockHash;
    }

    [[nodiscard]] int GetHeight() const {
        return nHeight;
    }

    void SetHeight(int _height) {
        nHeight = _height;
    }

    [[nodiscard]] uint32_t GetTotalRegisteredCount() const {
        return nTotalRegisteredCount;
    }

    [[nodiscard]] bool IsMNValid(const uint256 &proTxHash) const;

    [[nodiscard]] bool IsMNPoSeBanned(const uint256 &proTxHash) const;

    static bool IsMNValid(const CDeterministicMNCPtr &dmn, int height);

    static bool IsMNValid(const CDeterministicMNCPtr &dmn);

    static bool IsMNPoSeBanned(const CDeterministicMNCPtr &dmn);

    [[nodiscard]] bool HasMN(const uint256 &proTxHash) const {
        return GetMN(proTxHash) != nullptr;
    }

    [[nodiscard]] bool HasMNByCollateral(const COutPoint &collateralOutpoint) const {
        return GetMNByCollateral(collateralOutpoint) != nullptr;
    }

    [[nodiscard]] bool HasValidMNByCollateral(const COutPoint &collateralOutpoint) const {
        return GetValidMNByCollateral(collateralOutpoint) != nullptr;
    }

    [[nodiscard]] CDeterministicMNCPtr GetMN(const uint256 &proTxHash) const;

    [[nodiscard]] CDeterministicMNCPtr GetValidMN(const uint256 &proTxHash) const;

    [[nodiscard]] CDeterministicMNCPtr GetMNByOperatorKey(const CBLSPublicKey &pubKey) const;

    [[nodiscard]] CDeterministicMNCPtr GetMNByCollateral(const COutPoint &collateralOutpoint) const;

    [[nodiscard]] CDeterministicMNCPtr GetValidMNByCollateral(const COutPoint &collateralOutpoint) const;

    [[nodiscard]] CDeterministicMNCPtr GetMNByService(const CService &service) const;

    [[nodiscard]] CDeterministicMNCPtr GetMNByInternalId(uint64_t internalId) const;

    [[nodiscard]] CDeterministicMNCPtr GetMNPayee() const;

    /**
     * Calculates the projected MN payees for the next *count* blocks. The result is not guaranteed to be correct
     * as PoSe banning might occur later
     * @param count
     * @return
     */
    [[nodiscard]] std::vector <CDeterministicMNCPtr> GetProjectedMNPayees(int nCount) const;

    /**
     * Calculate a quorum based on the modifier. The resulting list is deterministically sorted by score
     * @param maxSize
     * @param modifier
     * @return
     */
    [[nodiscard]] std::vector <CDeterministicMNCPtr> CalculateQuorum(size_t maxSize, const uint256 &modifier) const;

    [[nodiscard]] std::vector <std::pair<arith_uint256, CDeterministicMNCPtr>>
    CalculateScores(const uint256 &modifier) const;

    /**
     * Calculates the maximum penalty which is allowed at the height of this MN list. It is dynamic and might change
     * for every block.
     * @return
     */
    [[nodiscard]] int CalcMaxPoSePenalty() const;

    /**
     * Returns a the given percentage from the max penalty for this MN list. Always use this method to calculate the
     * value later passed to PoSePunish. The percentage should be high enough to take per-block penalty decreasing for MNs
     * into account. This means, if you want to accept 2 failures per payment cycle, you should choose a percentage that
     * is higher then 50%, e.g. 66%.
     * @param percent
     * @return
     */
    [[nodiscard]] int CalcPenalty(int percent) const;

    /**
     * Punishes a MN for misbehavior. If the resulting penalty score of the MN reaches the max penalty, it is banned.
     * Penalty scores are only increased when the MN is not already banned, which means that after banning the penalty
     * might appear lower then the current max penalty, while the MN is still banned.
     * @param proTxHash
     * @param penalty
     */
    void PoSePunish(const uint256 &proTxHash, int penalty, bool debugLogs);

    /**
     * Decrease penalty score of MN by 1.
     * Only allowed on non-banned MNs.
     * @param proTxHash
     */
    void PoSeDecrease(const uint256 &proTxHash);

    [[nodiscard]] CDeterministicMNListDiff BuildDiff(const CDeterministicMNList &to) const;

    [[nodiscard]] CSimplifiedMNListDiff BuildSimplifiedDiff(const CDeterministicMNList &to) const;

    [[nodiscard]] CDeterministicMNList ApplyDiff(const CBlockIndex *pindex, const CDeterministicMNListDiff &diff) const;

    void AddMN(const CDeterministicMNCPtr &dmn, bool fBumpTotalCount = true);

    void UpdateMN(const CDeterministicMNCPtr &oldDmn, const std::shared_ptr<const CDeterministicMNState> &pdmnState);

    void UpdateMN(const uint256 &proTxHash, const std::shared_ptr<const CDeterministicMNState> &pdmnState);

    void UpdateMN(const CDeterministicMNCPtr &oldDmn, const CDeterministicMNStateDiff &stateDiff);

    void RemoveMN(const uint256 &proTxHash);

    template<typename T>
    [[nodiscard]] bool HasUniqueProperty(const T &v) const {
        return mnUniquePropertyMap.count(::SerializeHash(v)) != 0;
    }

    template<typename T>
    [[nodiscard]] CDeterministicMNCPtr GetUniquePropertyMN(const T &v) const {
        auto p = mnUniquePropertyMap.find(::SerializeHash(v));
        if (!p) {
            return nullptr;
        }
        return GetMN(p->first);
    }

private:
    template<typename T>
    [[nodiscard]] bool AddUniqueProperty(const CDeterministicMNCPtr &dmn, const T &v) {
        static const T nullValue;
        if (v == nullValue) {
            return false;
        }

        auto hash = ::SerializeHash(v);
        auto oldEntry = mnUniquePropertyMap.find(hash);
        if (oldEntry != nullptr && oldEntry->first != dmn->proTxHash) {
            return false;
        }
        std::pair <uint256, uint32_t> newEntry(dmn->proTxHash, 1);
        if (oldEntry != nullptr) {
            newEntry.second = oldEntry->second + 1;
        }
        mnUniquePropertyMap = mnUniquePropertyMap.set(hash, newEntry);
        return true;
    }

    template<typename T>
    [[nodiscard]] bool DeleteUniqueProperty(const CDeterministicMNCPtr &dmn, const T &oldValue) {
        static const T nullValue;
        if (oldValue == nullValue) {
            return false;
        }

        auto oldHash = ::SerializeHash(oldValue);
        auto p = mnUniquePropertyMap.find(oldHash);
        if (p == nullptr || p->first != dmn->proTxHash) {
            return false;
        }
        if (p->second == 1) {
            mnUniquePropertyMap = mnUniquePropertyMap.erase(oldHash);
        } else {
            mnUniquePropertyMap = mnUniquePropertyMap.set(oldHash, std::make_pair(dmn->proTxHash, p->second - 1));
        }
        return true;
    }

    template<typename T>
    [[nodiscard]] bool UpdateUniqueProperty(const CDeterministicMNCPtr &dmn, const T &oldValue, const T &newValue) {
        if (oldValue == newValue) {
            return true;
        }
        static const T nullValue;

        if (oldValue != nullValue && !DeleteUniqueProperty(dmn, oldValue)) {
            return false;
        }

        if (newValue != nullValue && !AddUniqueProperty(dmn, newValue)) {
            return false;
        }
        return true;
    }
};

class CDeterministicMNListDiff {
public:
    int nHeight{-1}; //memory only

    std::vector <CDeterministicMNCPtr> addedMNs;
    // keys are all relating to the internalId of MNs
    std::map <uint64_t, CDeterministicMNStateDiff> updatedMNs;
    std::set <uint64_t> removedMns;

    template<typename Stream>
    void Serialize(Stream &s) const {
        s << addedMNs;
        WriteCompactSize(s, updatedMNs.size());
        for (const auto &p: updatedMNs) {
            WriteVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s, p.first);
            s << p.second;
        }
        WriteCompactSize(s, removedMns.size());
        for (const auto &p: removedMns) {
            WriteVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s, p);
        }
    }

    template<typename Stream>
    void Unserialize(Stream &s) {
        updatedMNs.clear();
        removedMns.clear();

        size_t tmp;
        uint64_t tmp2;
        s >> addedMNs;
        tmp = ReadCompactSize(s);
        for (size_t i = 0; i < tmp; i++) {
            CDeterministicMNStateDiff diff;
            tmp2 = ReadVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s);
            s >> diff;
            updatedMNs.emplace(tmp2, std::move(diff));
        }
        tmp = ReadCompactSize(s);
        for (size_t i = 0; i < tmp; i++) {
            tmp2 = ReadVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s);
            removedMns.emplace(tmp2);
        }
    }

    bool HasChanges() const {
        return !addedMNs.empty() || !updatedMNs.empty() || !removedMns.empty();
    }
};

// TODO can be removed in a future version
class CDeterministicMNListDiff_OldFormat {
public:
    uint256 prevBlockHash;
    uint256 blockHash;
    int nHeight{-1};
    std::map <uint256, CDeterministicMNCPtr> addedMNs;
    std::map <uint256, std::shared_ptr<const CDeterministicMNState>> updatedMNs;
    std::set <uint256> removedMns;

    template<typename Stream>
    void Unserialize(Stream &s) {
        addedMNs.clear();
        s >> prevBlockHash;
        s >> blockHash;
        s >> nHeight;
        size_t cnt = ReadCompactSize(s);
        for (size_t i = 0; i < cnt; i++) {
            uint256 proTxHash;
            // NOTE: This is a hack and "0" is just a dummy id. The actual internalId is assigned to a copy
            // of this dmn via corresponding ctor when we convert the diff format to a new one in UpgradeDiff
            // thus the logic that we must set internalId before dmn is used in any meaningful way is preserved.
            auto dmn = std::make_shared<CDeterministicMN>(0);
            s >> proTxHash;
            dmn->Unserialize(s, true);
            addedMNs.emplace(proTxHash, dmn);
        }
        s >> updatedMNs;
        s >> removedMns;
    }
};

class CDeterministicMNManager {
    static const int DISK_SNAPSHOT_PERIOD = 576; // once per day
    static const int DISK_SNAPSHOTS = 3; // keep cache for 3 disk snapshots to have 2 full days covered
    static const int LIST_DIFFS_CACHE_SIZE = DISK_SNAPSHOT_PERIOD * DISK_SNAPSHOTS;

public:
    RecursiveMutex cs;

private:
    RecursiveMutex cs_cleanup;
    // We have performed CleanupCache() on this height.
    int did_cleanup
    GUARDED_BY(cs_cleanup) {0};

    // Main thread has indicated we should perform cleanup up to this height
    std::atomic<int> to_cleanup{0};

    CEvoDB &evoDb;
    CConnman &connman;

    std::unordered_map <uint256, CDeterministicMNList, StaticSaltedHasher> mnListsCache
    GUARDED_BY(cs);
    std::unordered_map <uint256, CDeterministicMNListDiff, StaticSaltedHasher> mnListDiffsCache
    GUARDED_BY(cs);
    const CBlockIndex *tipIndex
    GUARDED_BY(cs) {nullptr};

public:
    explicit CDeterministicMNManager(CEvoDB &_evoDb, CConnman &_connman) : evoDb(_evoDb), connman(_connman) {}

    ~CDeterministicMNManager() = default;

    bool
    ProcessBlock(const CBlock &block, const CBlockIndex *pindex, CValidationState &state, const CCoinsViewCache &view,
                 bool fJustCheck)

    EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    bool UndoBlock(const CBlock &block, const CBlockIndex *pindex);

    void UpdatedBlockTip(const CBlockIndex *pindex);

    // the returned list will not contain the correct block hash (we can't know it yet as the coinbase TX is not updated yet)
    bool BuildNewListFromBlock(const CBlock &block, const CBlockIndex *pindexPrev, CValidationState &state,
                               const CCoinsViewCache &view, CDeterministicMNList &mnListRet, bool debugLogs)

    EXCLUSIVE_LOCKS_REQUIRED(cs);;

    static void HandleQuorumCommitment(const llmq::CFinalCommitment &qc, const CBlockIndex *pQuorumBaseBlockIndex,
                                       CDeterministicMNList &mnList, bool debugLogs);

    static void DecreasePoSePenalties(CDeterministicMNList &mnList);

    CDeterministicMNList GetListForBlock(const CBlockIndex *pindex);

    CDeterministicMNList GetListAtChainTip();

    // Test if given TX is a ProRegTx which also contains the collateral at index n
    static bool IsProTxWithCollateral(const CTransactionRef &tx, uint32_t n);

    bool IsDIP3Enforced(int nHeight = -1);

    // TODO these can all be removed in a future version
    void UpgradeDiff(CDBBatch &batch, const CBlockIndex *pindexNext, const CDeterministicMNList &curMNList,
                     CDeterministicMNList &newMNList);

    bool UpgradeDBIfNeeded();

    void DoMaintenance();

private:
    void CleanupCache(int nHeight)

    EXCLUSIVE_LOCKS_REQUIRED(cs);
};

extern std::unique_ptr <CDeterministicMNManager> deterministicMNManager;

#if defined(MAC_OSX)
#pragma clang diagnostic pop
#endif

#endif //FORTUNEBLOCK_DETERMINISTICMNS_H