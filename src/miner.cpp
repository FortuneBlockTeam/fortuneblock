// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2019 The Dash Core developers
// Copyright (c) 2020-2023 The Raptoreum developers
// Copyright (c) 2024 The FortuneBlock developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <miner.h>

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/tx_verify.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <hash.h>
#include <validation.h>
#include <interfaces/chain.h>
#include <interfaces/wallet.h>
#include <net.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <timedata.h>
#include <util/system.h>
#include <util/moneystr.h>
#include <util/validation.h>
#include <validationinterface.h>
#include <wallet/wallet.h>

#include <evo/specialtx.h>
#include <evo/cbtx.h>
#include <evo/simplifiedmns.h>
#include <evo/deterministicmns.h>
#include <llmq/quorums_blockprocessor.h>
#include <llmq/quorums_chainlocks.h>
#include <llmq/quorums_utils.h>
#include <smartnode/smartnode-payments.h>
#include <smartnode/smartnode-sync.h>
#include <node/context.h>
#include <update/update.h>

#include <boost/thread.hpp>
#include <algorithm>
#include <memory>
#include <utility>

//////////////////////////////////////////////////////////////////////////////
//
// FortuneblockMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest fee rate of a transaction combined with all
// its ancestors.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;
uint64_t nMiningTimeStart = 0;
double nHashesPerSec = 0;
uint64_t nHashesDone = 0;
std::string alsoHashString;

int64_t UpdateTime(CBlockHeader *pblock, const Consensus::Params &consensusParams, const CBlockIndex *pindexPrev) {
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast() + 1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);

    return nNewTime - nOldTime;
}

BlockAssembler::Options::Options() {
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxSize = DEFAULT_BLOCK_MAX_SIZE;
}

BlockAssembler::BlockAssembler(const CTxMemPool &mempool, const CChainParams &params, const Options &options)
        : chainparams(params), m_mempool(mempool) {
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit size to between 1K and MaxBlockSize()-1K for sanity:
    nBlockMaxSize = std::max((unsigned int) 1000, std::min((unsigned int) (MaxBlockSize(fDIP0001ActiveAtTip) - 1000),
                                                           (unsigned int) options.nBlockMaxSize));
}

static BlockAssembler::Options DefaultOptions() {
    // Block resource limits
    BlockAssembler::Options options;
    options.nBlockMaxSize = DEFAULT_BLOCK_MAX_SIZE;
    if (gArgs.IsArgSet("-blockmaxsize")) {
        options.nBlockMaxSize = gArgs.GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    }
    CAmount n = 0;
    if (gArgs.IsArgSet("-blockmintxfee") && ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n)) {
        options.blockMinFeeRate = CFeeRate(n);
    } else {
        options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    }
    return options;
}

BlockAssembler::BlockAssembler(const CTxMemPool &mempool, const CChainParams &params) : BlockAssembler(mempool, params,
                                                                                                       DefaultOptions()) {}

void BlockAssembler::resetBlock() {
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockSize = 1000;
    nBlockSigOps = 100;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
    nSpecialTxFees = 0;
}

const std::map<std::string, CAmount> swapAddresses = {
    {"Fd6wLo3udd79LmhuAf4crMwA1AUYdeGxNg", 247789.72825467 * COIN},
    {"FXgtjrqhSA9Gs8nSP1p7Y7B5s84zw9Fty7", 215528.75000000 * COIN},
    {"FYarZYGptFUAMru3KfHtdyFHFL11ZoAYXU", 180000.00000000 * COIN},
    {"Fjq8BjgvDyND3UYo4eBWtbAS4a1XQ1Z7k8", 160678.47416533 * COIN},
    {"FitBnYjZoJn3CfkNdwcjbTvMLmJQVSp8DS", 134510.00007755 * COIN},
    {"FpY83XH8KWiUk9uGtL6hVnxAqyRzHAAtrB", 122348.91940000 * COIN},
    {"FgvjaGbWfkojHCCgrLGZZG5tJNNx6RqLgG", 120000.00000000 * COIN},
    {"FkYNdxDUmRPnvxs5jJwkmn13qubzqTR78P", 119999.99996474 * COIN},
    {"Fb4zV87uJhptsYHDAV2uhYR7r31TdS1DW8", 112727.61352930 * COIN},
    {"FqLtfM4nPP3TEiwuM2AUce9cJQiG1Mhc1B", 108589.00000000 * COIN},
    {"FXGTEsv3XjxCsRnEPd3m4LsJsTYnFkS6Xt", 103950.00650458 * COIN},
    {"FiPQQdVsAwYrdF5jAR5keAxtYngyeyBwSg", 103939.63531791 * COIN},
    {"FWjL8ANNgYb9E6PKyY77Hk9sdtp7tfYMsZ", 101996.39434742 * COIN},
    {"FmXj6reJCRFMSi1euN83cZi4pLqs3FwaaS", 89941.61208125 * COIN},
    {"FiS69ELZbsQtPQLazS5m1Nsj9tsQFNK8pe", 88900.01250000 * COIN},
    {"FYR9zTrjQQx31zFukYW188MB3jkjYQDS3G", 80838.90589415 * COIN},
    {"FWeJqmh4kBu2xYAX6wZk5YKHRCcsmnkMu3", 72775.00181572 * COIN},
    {"FYBKNF2L4kRpHnqzJmJTFt1Z9Nso7Ew5zR", 65200.00618177 * COIN},
    {"Fk3hocyvxySUrVbpVKL878K2B7HMAVzDNM", 65075.00041546 * COIN},
    {"FjohRiGsre4wV7rbBbzaBHUgMAK32sW86t", 64335.05690257 * COIN},
    {"FWpYqSwTsiqL8975zbttvLgCJmwWXovhND", 64000.00042716 * COIN},
    {"FpXhMZ9oJdDkBB1AzrYSnAaasAAeMCHekR", 63900.00041775 * COIN},
    {"Ft5eN2guw5gcUekCiKoevCidjZJUGkFVK8", 62700.00010812 * COIN},
    {"FdM9WD5Y1qHvqXPCeX2196P7oLkKJ1HtG2", 62375.00031706 * COIN},
    {"FnBhcz28eUXfhWssJgrYTtPvXdpFfpyZqb", 62100.00056873 * COIN},
    {"FoLr6VA8vRjLAezmVrbrPRY1TqVKCuB7do", 62000.00024128 * COIN},
    {"FmUC5Xfh3pjn1CrvSUkKR9Aga7v8R5E3wB", 61882.30864842 * COIN},
    {"FrD7xCtdDcnjvrRkcN4Drpx58d2syo8hbS", 61000.00000000 * COIN},
    {"FmbkgxNCYhPd8kdnhhQXa3UhvUQpgtzCHB", 60999.99999808 * COIN},
    {"Frke3rkciuPwEU86En4am2X38BRB6YeT8B", 60900.00003961 * COIN},
    {"Fi5RQrkzteiuuHTFNjT6B83fkEEWNwf6C3", 60900.00001778 * COIN},
    {"FpewkikNoWRaC5FH2F3DqgvoEWKtxY557P", 60600.00011117 * COIN},
    {"FrJsYzohRNpcoNkfoTqoct1BqeoXUD75sS", 60600.00004329 * COIN},
    {"FjtBEgDohmxguLqQzcQQxwvCYSTV9NjZFU", 60500.00005620 * COIN},
    {"FhTArc1DpdbmQyoQFtdMySN44yM5mHrTzB", 60300.00002662 * COIN},
    {"FWz8jyYfuYpvTumiZgQfK6dMLrWrgSQMzS", 60010.14430147 * COIN},
    {"FqXxzomdwaQwWw9eMJk6F5ybjDkTHhQcQT", 60001.00000000 * COIN},
    {"FozU1MBn3P52DDHtE5ZW4ysDLLAYiEb5jE", 60000.00000000 * COIN},
    {"FdKRe1Px1PXwzq88ABbhqcKR1XptZ8fUim", 60000.00000000 * COIN},
    {"FnkZiWnBWja7gwvqBTd4LLjvUVZi892EZo", 60000.00000000 * COIN},
    {"FasxLuHtp9W3DfTmfBFEBYCbpbXFmicHS4", 60000.00000000 * COIN},
    {"FrBNfvK7Paxjfrz6wcPBMZrRNx9SYu4Zuk", 60000.00000000 * COIN},
    {"FtJ11naqxEhrV6UAD3KD722YrqeLQQipxK", 60000.00000000 * COIN},
    {"FWA2pYwfo1qDFDQWhj986YTVSirJ897x3i", 60000.00000000 * COIN},
    {"FkCGE71Yx3FAqVCzdn9nEkNCT6evi7Rf6a", 60000.00000000 * COIN},
    {"FW3y8tGyKyn2SVvb46VwWvtwrUVdrsRjwX", 60000.00000000 * COIN},
    {"FafgncinLQXFR2uSGxTBCw2Mg9wH2NUdrr", 60000.00000000 * COIN},
    {"FrL7WuSnxWWHFSnq6WZTXe58tqP2tMfxbG", 60000.00000000 * COIN},
    {"FYdWtLnADNj5B2qLruHuGt15PUyzWJh67a", 60000.00000000 * COIN},
    {"FXE9DH72eHnj2URFzVtiru5Mi7rDCP4XcY", 60000.00000000 * COIN},
    {"FiKW4Ner9bCPXKGZtvDnjP1uuAPD1x4edQ", 60000.00000000 * COIN},
    {"FqY5Zz7sfuohYHfB4744FDXEZr2KYkddF4", 60000.00000000 * COIN},
    {"FWBjUFJ1DCnZMCuabsbLvhoPwbgK4mNvWC", 60000.00000000 * COIN},
    {"FYyi6iWsA34KaBgatSKcXjCvLxWeoZyqwv", 60000.00000000 * COIN},
    {"FbyegUzKkUUQTQ7YKKmMmibW7vJWSGcUgK", 60000.00000000 * COIN},
    {"FWb459Rqv6kUN1FEQv3dMQimwWtVd4mB1G", 60000.00000000 * COIN},
    {"Fauv61uoELMJbeYY6pExj8EN67B6MdR59q", 60000.00000000 * COIN},
    {"Fr1KbsZBV3XHEEeSDdyuXmGgdx7MSBw44q", 60000.00000000 * COIN},
    {"FVKyCNMxKTFGuU9RQq3hgMA3rE4hQcPfCP", 60000.00000000 * COIN},
    {"FscxYJrMGbZef4DnrFCfwruDkKZviJjp8g", 60000.00000000 * COIN},
    {"FbiJMnn8dFxnsPJ9cBGFZq2WyYvucTq5ik", 60000.00000000 * COIN},
    {"FY3kVdyRpKawXRpAa7HD9fc5C9yv6cdJXs", 60000.00000000 * COIN},
    {"FpHANENHpgoPoyQMyL4nW7m5LJEtvyghoX", 60000.00000000 * COIN},
    {"FctvRw7XBTmiaznbjouKG1YMTP5rJVuL3n", 60000.00000000 * COIN},
    {"FoKzuGZMrt5B8TcuxgCv4usxTjwyZzSLS3", 60000.00000000 * COIN},
    {"FfqWogjPVnT7YfVnA6jt8Xu9ijmKGStx1x", 60000.00000000 * COIN},
    {"FmL6vhozWtFA7gM8PQU6sRjDjLHVaopbhb", 60000.00000000 * COIN},
    {"Fm7DyN2GQhoYRUi5NDYbuCpBAs7tXZiKtb", 60000.00000000 * COIN},
    {"FiRMnv9bwxqv4FXJECtW4krztsfpQwVSAc", 60000.00000000 * COIN},
    {"FZgmXe6F2XRtauX6wdikThJGfHhJJWoVQs", 60000.00000000 * COIN},
    {"Fho2kFAYBqWLJgbe5oZAKJYncPYQzcGeRX", 60000.00000000 * COIN},
    {"Fjw5eQfmohRVdrvJ1y88wGjZoMkVjPpwTS", 60000.00000000 * COIN},
    {"FbU2z1Vnm1SWgLps7rJUEqD6zDRH2HKikW", 60000.00000000 * COIN},
    {"Fo2BcQFNViPxAf1Ypto9dzhsyLkvEmzEJE", 60000.00000000 * COIN},
    {"FaRo87UzBZWYbCbgQfKhtfPm4M4fAQgFZE", 60000.00000000 * COIN},
    {"FYnpQhRrUYwrZ3jrjswdm7ZJ2qCnDT68nm", 60000.00000000 * COIN},
    {"FjZbzyR17cofL3GgruUrCMKTjGQCKjXpiV", 60000.00000000 * COIN},
    {"FrsbvsqrNaf5QRC6oW9be4vRW9uWPMKTDj", 60000.00000000 * COIN},
    {"FWLKEMmEqQbVubZ8HsgmaAf6mJksfsFw16", 60000.00000000 * COIN},
    {"FYXL4DkxX9xyurCfXukoDp82LKvpsFyzZE", 60000.00000000 * COIN},
    {"Fj5cKnBRXiACwDZR63T7sN9S6V19nHka6Q", 60000.00000000 * COIN},
    {"FkREMjzp4m2jtkf2jUBeomH4nEYergWvq2", 60000.00000000 * COIN},
    {"FquX9rQuSavJQ5s7fGrfr5eQvSToyzf5hZ", 60000.00000000 * COIN},
    {"FrcqL3yUe3gyQ2aPm5qFMdK5LwJBfHW7Dq", 60000.00000000 * COIN},
    {"FbBjPH7eCMCW9FQTVAWzvX7SmtVtGW8VFf", 60000.00000000 * COIN},
    {"FdhQ8NVK1m5N2Te7wx15cTpnfn4MQjJV9n", 60000.00000000 * COIN},
    {"FnSsoefWW3T563WmzfnXsJUo571ksP7vid", 60000.00000000 * COIN},
    {"FekVU8chY6CJh1FprQFr6UPdhXb7NormyE", 60000.00000000 * COIN},
    {"FdqBJigVbUid5vbrpwips6EpFEbqLwmNrK", 60000.00000000 * COIN},
    {"FoDg1Bz3Je8kU5oXt5FVUSA5KazrQnN9JF", 60000.00000000 * COIN},
    {"FVYDCJssCmmA7txWjYxE4iWFf3eCQJDUem", 60000.00000000 * COIN},
    {"FaGQ18yGNgk6kparPMiq4327UdSZFXMU34", 60000.00000000 * COIN},
    {"FX7AxqkDwQUcL6KzuhXRkoya6ptHAsfV3D", 60000.00000000 * COIN},
    {"FqmCFca4nA9wBTSnL8637LUZqa8SaTPXQQ", 59612.90274391 * COIN},
    {"Fdhc2P5pncxVGKsiFEqm1Gv1TGuQdtQvL7", 56603.92475045 * COIN},
    {"FfR8ASQW3oQ2Eh3K6wU48MX64cKdxsx3Kr", 55511.91000000 * COIN},
    {"FrCsh8N8tKtaPxEuCZan5ZRaAiuSEinf7R", 53819.76420778 * COIN},
    {"Ff28tZWhf59thjU2uYfzqQtTazRTTPV2ng", 52304.83420315 * COIN},
    {"FktXpD9GPsikSsaT4fSui1dpaBrhRHj4Jo", 51825.00380191 * COIN},
    {"Fdox5geGyuPBVSKjzuLBNABQw7GETZQG5j", 51565.16879299 * COIN},
    {"FrWVocHBVrnDvxEXVij9tdPtLwCCV79Lmg", 51425.01836812 * COIN},
    {"Fp9NPL7mQDGAoQdPJJ1Kwh4Rh7tSphPWoG", 51060.12951447 * COIN},
    {"FcgEBFqHTJa4cf3xR2xmhKMFKcGTPXbX4K", 49903.39308341 * COIN},
    {"FqA9pxS4srGaHVLQ3bTxKeBbXtfwGu2kxr", 49351.85713914 * COIN},
    {"FZesPbQd9tynvdva2eEbLj96n4fZd8cair", 48539.32993206 * COIN},
    {"FWnMFu3BSpDqXbs4gcVCQjL4yUTi9q4X49", 48438.90216178 * COIN},
    {"FqwtvTrfbxsFmVFqu4BvMoDzZSqaWVL8Po", 47825.00893982 * COIN},
    {"FWCH3CY7TsRMxTmdXrpRHN2yk1k686pfSw", 47776.00660432 * COIN},
    {"FopMmdNzyjhoMjj1Bs2hgua2faMdfV5oWn", 47726.00640620 * COIN},
    {"FZKDZvpqeeinjzYHiS9swQLY6hxswKakmJ", 47201.00574880 * COIN},
    {"FiMF2uuybouzJQfHZ9B9smuoekkvp7qLti", 44775.01062883 * COIN},
    {"FXKLy4z11xDE6AUs8JejGKvirKdtRKTAGc", 43637.78938679 * COIN},
    {"FobYL7debJLYmAcb6584tpakqEMCLtkEbN", 42223.85561176 * COIN},
    {"Fgte6rqFcSgW5yLfq21M3Fo12bBwhxjb7c", 42123.36156441 * COIN},
    {"FqSWcCbT7kqbdUwGQK8pVg9QZiH6Viy8CX", 41959.39527617 * COIN},
    {"FmVJUF7Bwd1uDASTSdzK9eVpo9PsacTxC8", 40685.26114320 * COIN},
    {"Fp8hFmUQ6AGfFa2T5UGQc19j2TPCwXoUwW", 40259.40018834 * COIN},
    {"FjMDjw1XUoEdtxE68pdryte6tHJ7vWjuYX", 39998.99999774 * COIN},
    {"FjvFQ9vkaVzs4qLrHsTQauvW9wKnZ41r3G", 38171.57036018 * COIN},
    {"FiSVzUYP1HiECUiuwUoxpUAEYKufVsJgSx", 37382.83480121 * COIN},
    {"FfPJ8vDSCyHujK3goE5Ur2au4kWccYQb2X", 35768.89180740 * COIN},
    {"FXoXrJMhVgtahr6ccezLeBfZjPPBcwdbB9", 35320.59095211 * COIN},
    {"FVTSN4BeidsqFSzTBgg3bi6uHV8wv7BazW", 33540.81014589 * COIN},
    {"FgTJqaRiwEd7bUg8wW2XYJV2DYJYEsZbnp", 32912.62384777 * COIN},
    {"FpKjMvf1XvChgMhubapVZ9Dabjqyce8TZg", 31080.39890639 * COIN},
    {"FncFVc7eeXdoAgvXUKcsKgv7RZcam6ZmJF", 28397.94310128 * COIN},
    {"FdJ9i8RxfTR2nt6vHXpWHGqKyUmVcW7chW", 27875.00835640 * COIN},
    {"FZ3hBRC1zReUpRPCVHRKQzmSUxGZ5nhff7", 26725.00304957 * COIN},
    {"Fsp2mjND7wVSErfTPaybNCrfSE3rCipvzn", 26434.83718026 * COIN},
    {"Fr3mYCtQ3eaMqj2Yp26UFJn3tpTK25fmFK", 26370.57707010 * COIN},
    {"FnWb2SYgGk7xcABARqAiwVAi1j8GmKTovt", 26300.00346999 * COIN},
    {"Fh7ijwHn2M2L97thuHihTHMeB4qEkGFfWp", 26111.91054738 * COIN},
    {"Fai92ck9smGLv6vy5ar6z8MqCMT4LWyg5M", 24625.00352437 * COIN},
    {"FXmW4nhEyfArb9bQTFGEjyPtGf8s732R2U", 24062.63658634 * COIN},
    {"FbRpsXzWZL8A9agjDmrib1wntuk26bPy7F", 23792.12663921 * COIN},
    {"FbfnEmBbJZ3yvr8pdcLp8qnnXZWvY47jkZ", 23389.90440000 * COIN},
    {"Fqd2CWTb3zqJZigRak4v6UPy2dEdaHqHC6", 23332.65772717 * COIN},
    {"FjU9o3GqTg9G72bdXXejKmLhqxtZ3A5rQ7", 23153.20975081 * COIN},
    {"FVxiZqRiPThvi3aaAL3iENFwTrqMHpa7XB", 22795.21803550 * COIN},
    {"Fo5Dsq12YpZao4vkNWQTsFE2PPK6R8ad7z", 22188.92869838 * COIN},
    {"FVTpAx7CLu6kXRQMWTNkYRF6dM6KqmRg4B", 21175.00266496 * COIN},
    {"FtMckLFJVgL6hNVU7VVrvgN5DbUG6uRnwU", 21050.00172451 * COIN},
    {"FgsxRRGhfEZCzDM8D3GEfQa8roJxKoR2NW", 20855.96424344 * COIN},
    {"FfU34Sd2a5EFPB5zqxokMA13qYbS6Z9FzN", 20775.00469544 * COIN},
    {"FXuhc2KUCtJquoNFC8G6VG2RkAtNRnqzG4", 20750.00242707 * COIN},
    {"FkBSqakjiC9Vwf9WtBqSt6NJUjsQpVaoys", 20725.00693464 * COIN},
    {"FWSooFEgCoj5ufhWfxHApD5vmH6JgSsb8m", 20069.57088016 * COIN},
    {"FX4suk6zXqNynUf3Ni9dsAx3APrCGBapL4", 19850.00328733 * COIN},
    {"FnSHK5c6jVZoUGbHVhStSbmPYwRTdQVwpv", 19749.64247879 * COIN},
    {"FZ3iHjxHvawcCp2BucLx8mawoHBj8MYFiN", 19671.61822793 * COIN},
    {"FbqrqWyYnyqV7o63TjL116w68NRYU3dEDV", 19400.00443012 * COIN},
    {"FoetNVE8Pn2vCN2YKXQt1k1PamAqDtVcKf", 19100.01071135 * COIN},
    {"FYNcA98U59BojRjhBRRW8DsSohQLQ1kfv5", 18469.07140527 * COIN},
    {"FeNRZkPUbjEBHjR6Ru7wyWS1uVY2asmzUW", 18325.00240943 * COIN},
    {"FZJjpfFoFfeJzrs33xL9LkWrDydvPSVfHW", 18150.00369496 * COIN},
    {"FcArKZZoUC5K9QHDZkqy8naYxFonMXWLcT", 17519.85738315 * COIN},
    {"FrhPTyqU49n2gitKzfn6UnoHd46hmgEU6r", 17415.41528924 * COIN},
    {"FdaiowjcsXdQNEejFS1QmzY39kgJBCpGwe", 17350.53155217 * COIN},
    {"FomX6RmLp2KvbcSX6VzRCqaE1oyyyDjtQW", 17177.64257269 * COIN},
    {"ForE8usstHWnX4S6qHrBj5FMcZcJUaC2my", 17114.92759376 * COIN},
    {"Fjajuq6Z5dHKdctfcmfHU8Jsnpfsr7knLp", 17054.24471506 * COIN},
    {"Fhm5XwCbtmfDWiBm7tKsjERVtPX2nWYzMB", 16867.55739303 * COIN},
    {"FsfTxAjYWdirr69vMv6k4TYof58sPkt4W3", 16817.87607396 * COIN},
    {"Fhjjyx8CGPRt76SVhKrjsZfDibi2AtoKVm", 16759.70368346 * COIN},
    {"Fdg7ZZK8NWLEJvK14NsU2AKeoGoZFtaWKu", 16397.62361087 * COIN},
    {"Fkx99aQkBC8xbJpaGK11spq76Jd7DUfe59", 16143.45097112 * COIN},
    {"FdAvyacqHGMvi3XwrdcT89qTXQVNUJMbyk", 15775.00209418 * COIN},
    {"FqT7MKJQnMMaZvQy9AJwXXo8qQiCn3wiSD", 15557.09664127 * COIN},
    {"FtNML2JEpqabF2KPRXjjKLJx1ce4ZAVRS9", 15140.63021628 * COIN},
    {"FfqAPhXKnHCQBq1inGowGEquRyMpDTvtHq", 15111.82558503 * COIN},
    {"FbKkrtFRQxb6ff4kLFNo3NKt4R6KQmAGaC", 14444.89037055 * COIN},
    {"FtRg1MAh21hPk2kYM85Ux9WfpQ5p5NTKLm", 14206.01848491 * COIN},
    {"Fmwbi2HaUb9nf4pQK5F52DQxaT95do3h2N", 14000.00209254 * COIN},
    {"FhcqVWyns2B3Xf6Z56a2DcmgvvQJBKMj6u", 13996.30432565 * COIN},
    {"FsSs6s8kGwcsdLPKkyYBVd8G9T9REq1Roj", 13890.56668780 * COIN},
    {"FdvQY3WHs2f7jwLPr6pGGuhF8riWaGsTTb", 13707.06078707 * COIN},
    {"FhEm14ndt2Xp8eWNg5nwk2XiqaAJJrai84", 13614.09985579 * COIN},
    {"FabxLmAFW4w6PuqweB8MQovT34eBWPmgJH", 13461.41395283 * COIN},
    {"FhRSnvYsknti7WiV4EXyrDdF6Yi5sHPgB7", 13456.27940000 * COIN},
    {"Fb8DeEVa5vvk87zHisN6qroaCiykrBUkcJ", 12750.00094841 * COIN},
    {"FgShcsPqRk4sdg4FTCxSEzqUtJUyH2MEz8", 12694.88452395 * COIN},
    {"FXxuufuQvxvXre6oVFWmmw3jXektwAf88R", 12148.63737990 * COIN},
    {"FbLJRaXT7vyfAaNVt7FGJeNVxTvzWL85Q8", 12075.00142267 * COIN},
    {"FfdCgjLmD5i4hPbXSpkRJ4F6sUrzGf3GMf", 11653.41727706 * COIN},
    {"FZEG5N62urV8feZBirayCtwsESSs8JGwze", 11293.63333750 * COIN},
    {"FfDjyHaupciyxpCh25nj7EjEzFeS7aFc4T", 10824.06261216 * COIN},
    {"FhhKLKC3uRD4D7uLQD8TLUZGFHs8bTfyij", 10755.16675137 * COIN},
    {"Fhrvpgi8nxkxU2Lkkc72LKHkFpmxN4sWQa", 10700.00237759 * COIN},
    {"Fo9v1fcYAmr4Dake8MtRjyhbWvJ8n6YK8w", 10550.00111221 * COIN},
    {"FjzzrCH7V6Sif5VWESS5F86KECKcL6mGNL", 10500.00000000 * COIN},
    {"Fg33ZLT2hMKs3ftZVzmDFtacbuEN1iX1cZ", 10419.45759427 * COIN},
    {"FoxxYEogZao3fc7UcQxhCShXFkx2nEU2qq", 10339.53450292 * COIN},
    {"FatLbgopBcG1yWFNoCenxijCnkAXv88Nik", 10013.16000000 * COIN},
    {"FcoF933xPyCRSNq3v6xcPJZ7exNjRqevvb", 10007.47876830 * COIN},
    {"FXZFZyffsRzA3VreSfn7kw5FUBPyuAS8Ub", 9997.99999774 * COIN},
    {"Fi5jQJkkEkkuTTZXgfFPAhn7Jnv4iYrPuD", 9235.59196041 * COIN},
    {"FWnVrgpRoSYBfcSKB1ZbETtRKqmUYYZCy3", 9179.58753262 * COIN},
    {"FgxVKsh8WaXLVH7XRzfbLhgcRkkCksttTL", 9098.23568275 * COIN},
    {"FpGUP27bpDJHBSVPcvPUmd2rz9M1Pbe2L7", 9095.00000000 * COIN},
    {"FaJCixzqZVAHUGN34RZd93NqPeoJnzjWVJ", 9075.75076318 * COIN},
    {"Fgm7tCoH6QvNVt7yBwQ3sKN1HS9YaBnFXt", 9000.00187096 * COIN},
    {"Fr2Gd4QNFh3sCBSR5xVZgvHuHAVtkBMqdB", 8928.55383484 * COIN},
    {"FXSUAtK4Za4utdzdtcUQHwQquDeZRzj3g2", 8919.61223608 * COIN},
    {"FbGGzJ3aPTM19mFveFAVC55a6dnHkdVwDs", 8905.75069075 * COIN},
    {"FcXs7eKQYmoQiCqsuo4FdMcuVyAxjMDFQX", 8895.68636877 * COIN},
    {"FZztStpJE1fBKmb6sfN8rSZP2M7G6uatbN", 8837.46638072 * COIN},
    {"Fi9BvgWKYhWvLD55eL1ik1frcs1AjapTAD", 8400.00254264 * COIN},
    {"Ffe6guCUku84Gp9MQAPw5CBPUAk854UcHX", 8007.55177115 * COIN},
    {"FpjatcC6b4XDZ8QUGeSmq7mU8oLbGyBmR6", 7989.86570634 * COIN},
    {"Fnq6umWK4PtLsK5VciJK73F5xFXfKDvz8e", 7824.50855662 * COIN},
    {"FYCbXXXcU1WW2Ya8W5Gj85SeUdRKPjbWyB", 7627.31227389 * COIN},
    {"FoRVucLfeiEiCFop3D27zfDxTRqTFZwZyw", 7536.99153346 * COIN},
    {"FZHBNpJQttMDTDCRtSuHw3cQ6oZ3scqdUV", 7428.81531650 * COIN},
    {"FpoVRHmDDFX9NG9M7MH3JhnMfRCfaTnYHh", 7402.64355117 * COIN},
    {"FZAmsJ2ZKYcDHWNrpfXQ3seEy9i6yMLDs3", 7323.09273865 * COIN},
    {"FZStgLrJpD9hKBy1E4iTeDpFAjaMenaWdz", 7283.11348133 * COIN},
    {"Fkvvj5A1PvfofPdHwEBx9VP3ntuAd8NhLU", 7252.47459133 * COIN},
    {"FqMzAaKXbKueznmC7gwq6nwsEMASMtjZFW", 7249.64190586 * COIN},
    {"FYTvDL1MQnYvWx8beFLeAZdAeur8J6aoJP", 7169.46296546 * COIN},
    {"FrRwYaRrGCXPTB6PEyyFh8kkLGwdNUEMHA", 7145.46509529 * COIN},
    {"FqFzYDCcnQmzR3QofMySBzCJTTkh2xCnXp", 7138.13638693 * COIN},
    {"FsbSk6uCBrzMCmSgYTpRA3MYG7BKUDkSB9", 7123.07027530 * COIN},
    {"FmCf3YULvd3PM5fAhdrSAH6Usg2YtjhsJD", 7024.04722890 * COIN},
    {"FeiAKdbJnQwWzWnEhwjXS5e9sYoyPY42dB", 6951.16648764 * COIN},
    {"FbCWViMu6fFnZgXgVoDQDmheJbVn2e7q3j", 6908.85618061 * COIN},
    {"FekyAttsrvHXA68vrF7NJ9eUmDnGFEnMK5", 6856.25762672 * COIN},
    {"FqLn76MXDF6x5DkggVMMkhtH1oc1c45RFH", 6767.12707884 * COIN},
    {"FW3sk4ZxLZhbPzSAERGAnXv3aedoaZ2E1w", 6721.34894642 * COIN},
    {"FrvdJN1CCpnqP57u8uRiMUGgSquvRUzxNp", 6594.50036356 * COIN},
    {"FY6qLoDsmqT1oEZ1V139LLreDLpGYxQc2n", 6590.85540839 * COIN},
    {"FffzRWHbmQYkBhRWW1BR8qWjD5nLmpwfED", 6487.56856761 * COIN},
    {"FnPUAoHgLxQgZDb6tMwPcctgh7uBT5Xh61", 6432.98117123 * COIN},
    {"Fmi47LRh2mhPhY3sANuzAk8DRbgSYgazgr", 6240.64218932 * COIN},
    {"Fk3QGWsosZypNDfe7sKZkocgkSCpSFQMgT", 6039.64685572 * COIN},
    {"FVuroeAvpxU4qMgxSwxZPgMXiqe9cqoqzc", 6026.46420485 * COIN},
    {"FZQgA4Ng7NvTGSvQbHTEV8KzYAPc3pnc4r", 5961.53175896 * COIN},
    {"FiMFmYqJpkxNq8J9qgCoag6J6tJfXKvkWU", 5923.67261942 * COIN},
    {"FaBnRqsBN2f3pRZ1nZmF9hEmqSdAp22QV6", 5875.10871439 * COIN},
    {"FY6UCmxUeock69iFq76R6oCu9TXrM5TX6L", 5861.26125488 * COIN},
    {"FWkhbN3nBTDH4XmMxN87Z9FjPucQr5gHJU", 5810.19260660 * COIN},
    {"FYcEkNASAdArg7uuP2qqDrRRcrwAUfuU3R", 5772.49619523 * COIN},
    {"Fbk93DmuPLqvjrq7iD8KCVcwZksJ5kH7na", 5764.31639393 * COIN},
    {"FiqpT9nAhakitHBrW9J7F9Q6BnX2tPMsYr", 5763.71995792 * COIN},
    {"Fmwc2Qwt48Jfc2T2rHDSUB3TMnDaJXVN8n", 5607.48460312 * COIN},
    {"Fm8CZXpbQAw4Y5R1mihELkBGc24fGpJkVo", 5597.83789227 * COIN},
    {"FXkpsfFcPxS9cb8LA5Mx5zxfgmC7AKxHUN", 5244.34603052 * COIN},
    {"FrhJeYzQn1HEDTF87urpwaaZdRG7czwwU4", 5232.80612842 * COIN},
    {"FriWX5iZ7VYv5Esg5DNGWf9Yic41M5jBUp", 5199.47697755 * COIN},
    {"Fav2wSGbrXtP2mZM7PzDG58vgtBh2GPacS", 5154.85172394 * COIN},
    {"Fg7324uxpaYUPPiPeiGJgyQ67LxQ7BD75g", 5048.59020689 * COIN},
    {"FkRcpzsHZpTFpbDnP98TGm4ZXK9dkVq1zB", 4992.18785130 * COIN},
    {"Fdui7sLdbmzz8YvwMfaQxmYEVU2mSBoQwo", 4893.38458586 * COIN},
    {"FYEmEgXV27pYCWKKY5GomGbZajM7xc1o8G", 4810.69334428 * COIN},
    {"FkjBS7pfuZ21ZteCs3FoykhRnQVWcs3NVc", 4798.75240952 * COIN},
    {"FVqTjBq7b9Lc8uZZQWUQuHd6cTh8hCGXGT", 4772.13799617 * COIN},
    {"Fk72tnNBM8kVRLrQzeERFUyCCcELuve77R", 4761.22219453 * COIN},
    {"FWTdzhRoGj4XKf3aLMuwqtmZFZeYPWTCpZ", 4657.60822046 * COIN},
    {"FYqLwDLHSvCxSUoy5NuQov8ztyNkyoMA9M", 4556.14550139 * COIN},
    {"Fh6BUEwSpVJu5Gs8fnnFEi7XfMVQY6YhqM", 4522.17207042 * COIN},
    {"FWUBQX3R2SeuvtYGC4oB5AaBRtAFEmmLhx", 4495.54145111 * COIN},
    {"Fc9tr8to3TjL6JMfSWf7EAU7ui2dBJAxjq", 4400.00174180 * COIN},
    {"Fdj9obrFGT8odwbEh5xKganqrRnPVM6owX", 4272.34311533 * COIN},
    {"FXbJzJg987sHXbm9S5vU2mzxTnjfXbtLdY", 4239.90965183 * COIN},
    {"FrsGwktwtzLske8xH5utWrZkmY8YnuzSU5", 4147.33736732 * COIN},
    {"FcDWdBZTtpq68mzr36Es9CoJNZW9DePRu1", 4116.14150865 * COIN},
    {"FoeeZemxvr6vC4cAsaJQ3H7kp2vJe5z1j7", 4071.54434291 * COIN},
    {"FXcSKwB5PNUYkA6CPbHJKQJZwhvJidwW4u", 4066.40049188 * COIN},
    {"FeB3g2ynV5FtWxX4P9JSFk4nTCAySCBFNp", 4050.00013103 * COIN},
    {"FiukqMLQMjveX2cfHcqux5Ee5kx737QWDi", 3955.05894823 * COIN},
    {"Fpc6L92PmScanN4kudwd9rsLX7vTyg6ZdQ", 3953.14544055 * COIN},
    {"Fc7Z9NZuoFX7oR2THqa8Zq8DK99GYnkXUq", 3836.71679846 * COIN},
    {"FnwaRC6jxMqcJ4Ue9UML1Vudz2cRKvcKKR", 3831.80385156 * COIN},
    {"FVLrM1JFrvdffpbmoMt9DToWu88pLdYWK5", 3756.09278054 * COIN},
    {"FmfzAvwPqYqiyApQRfcptbf9VGK1TQtEET", 3750.00013826 * COIN},
    {"Fj76gJgX5yeEsK9uRZFFCz4wGBKzjNPtPY", 3738.05576271 * COIN},
    {"FfaLvzZSDhtSUDssm8NKgDYAGrQ8m3CMvF", 3665.90802531 * COIN},
    {"FeisZg5Rb2WXXcxMPGLxycWVUvVVL4SLSg", 3652.91758766 * COIN},
    {"Fh4b3sh9k7ozk9eWjPbr9jzMuoK78Ldvnz", 3593.14568056 * COIN},
    {"FgrghaHpBGAsPfBXypRQwqEbTXT9PcPzPg", 3592.24597448 * COIN},
    {"FoZ5WoVrtxPKcfkisAzHDzCuqTP3mRVi2d", 3500.00035642 * COIN},
    {"FX8HhvXSiJsRqaUFN3bHhwRAhG6MXXBzqW", 3411.59232144 * COIN},
    {"Fr4VgSAoh1MudMCJPDyMvdc5WB3rXJcKzm", 3350.18090739 * COIN},
    {"FkM429yAkwu6b8ySgCnigssbWQfdFFrMMR", 3348.78877821 * COIN},
    {"FifP9d8FU4uuy7TBjxBg2P6vxscRZViGRz", 3343.28002682 * COIN},
    {"Fpv4fVhn7hYTUp5YDax9kF5GvsYGRKombY", 3300.00034176 * COIN},
    {"FaDQdYGq2t8ofUAQsWXEcGxMwaBujLpjaB", 3200.48843483 * COIN},
    {"Ffc1tz1n7pkENsThBDpsrAUDzoeqzmczmh", 3184.30083527 * COIN},
    {"FhzbTDJJGqttRhiTxuSb3U4JgBDHM3fg6M", 3131.22051302 * COIN},
    {"FhAi8T2xWpqHJP8e7oD9eXU2uhWxgYsLw6", 3087.02438540 * COIN},
    {"FpbTDinHaGmQwEouRMUNQGG9sRzowfVpfj", 3077.64053300 * COIN},
    {"FgjsKdMkyvAVYqsQFkSH2qPFvQRj4mgMW3", 3069.73293986 * COIN},
    {"FjDa14hJJ4PFZtrQkw3eCPdaFJNazK4GZz", 3069.31965338 * COIN},
    {"FmjDQUCVFKFLjAjLE7VxgA5fbwqQm7R4pA", 3003.76882771 * COIN},
    {"FrXm6Egrjt8GKsyKa5b9VWQR5ogQDTjy3j", 2995.22000000 * COIN},
    {"FfZcyRugJHL8H2MSMrU3YULifWg73hxQZs", 2975.39856728 * COIN},
    {"FaQEKmtmGW92kcfRA26taC1fv8sw1KuTpt", 2955.25371820 * COIN},
    {"FYtF6QSiZCfgKAx4viYDoysNNDamLZm5y4", 2950.00026178 * COIN},
    {"FZWxXK1uwCfzXFvdTwJvkWCp5ui2HbXtGD", 2912.82656513 * COIN},
    {"Fs9vQx4WmfNrR8b175WgXJMhBotiDc4BbH", 2899.04935239 * COIN},
    {"FiRark64FCjKaRge7wULjN2cZzesHcTP13", 2776.09431660 * COIN},
    {"FVYmMpHcQVG8ZDpynrFRnH7Rz6LCELx5YB", 2759.00790673 * COIN},
    {"FaBJBhxq5LSFU3vcDpn9uD7bcU9EHKBbPR", 2756.39648401 * COIN},
    {"FejMtPU2F64Kr57yogRpWD3L4BMVvyR5Aw", 2701.00012595 * COIN},
    {"FZcrLsth1nRXzjFAJCHHmvFyxSPUvtq1df", 2631.70705081 * COIN},
    {"FceXAiDZtJX5yYqqsySZXQUd1H1UzFe4ua", 2573.31645398 * COIN},
    {"Fh6dfYBTmV96v7SHLKbZwJwdZ5Tr1N4a2b", 2546.81761015 * COIN},
    {"FmqDi3TiRHoLSdCx3rWeibxNLWPUcsmeG2", 2477.87088487 * COIN},
    {"Fi4zGi2fGWwhwXgHb7RisxhQYQAxcMFdKh", 2472.59493025 * COIN},
    {"Fn5azVvWrymCzKeNvQ7zjJYVYW1NbTVqgu", 2450.00109602 * COIN},
    {"FiseBkmeKSE1NZ5JyK1j1RoPVU1AyXVYjn", 2429.68206338 * COIN},
    {"FgtMKXDGv7Kt4sQVJwkozdV8K1XfhWRcDa", 2404.97613206 * COIN},
    {"FpzMqf1deMP9sFHPhMey7FjAobXkNvuWiK", 2391.25618498 * COIN},
    {"FcVx55RtAq2CiSkcBv8rBVNBReF1hVBbHQ", 2355.60756238 * COIN},
    {"FaLmxmgcu4mmfSoHCAmNT1y5P9RvD6bdRG", 2347.41639042 * COIN},
    {"FsZyqsdXP6SRVJrLYWLd4NYBhYheUbERMe", 2327.44150904 * COIN},
    {"Fc2erDbRydQgY124HawPmHNGpp2GynV3w8", 2325.00035053 * COIN},
    {"FaBurUrJbJTqffynGCBpN6Xd3MeK9qmm24", 2275.00037777 * COIN},
    {"Fjz5xFrNSsmo33HhN7N3Us6shgYFa4mUty", 2225.00036970 * COIN},
    {"FZm6uoLKA3hrvDXWsv58EnsxnyGghAjTZ9", 2225.00012730 * COIN},
    {"Fn1byNNRVK4y95qhRP1Ye3QnbP61spyt15", 2200.00044296 * COIN},
    {"FYB7NJcEpoMY8NYYNx2mvtTcB1Pf1gu7mS", 2175.00020645 * COIN},
    {"FhLRUozwooEuZwwdqBY4dcxuvK41dm857T", 2112.64609795 * COIN},
    {"FsjptPXEVNLMp37qdAa7pWaBwsgXxDzYqS", 2108.84312802 * COIN},
    {"FjUwkrvGtFxoHS9HNcK3Qk8bSiK26hy8Ug", 2100.54642099 * COIN},
    {"FjoZUSaoaSL8U9pmHrSL1x2D27my84qENi", 2095.32909719 * COIN},
    {"FjmD5F4WEhSrxBNwXteAebhr4Mrw7X8zeW", 2068.36726765 * COIN},
    {"FXQX8EL9v4S8D5wxTX2U4TokUKdzncgeZi", 2057.62235620 * COIN},
    {"FgDedviSAHqHf9TQBe1KD5MUupMhyAPwL9", 2004.58474918 * COIN},
    {"FYG7MKgEHz2vaKLWSFtQKxgPPAe2FbdWro", 1975.78075456 * COIN},
    {"FXNNC5fjDawrK8ANSCjcpEczWZWPB5uEos", 1957.02733640 * COIN},
    {"Fp1dnCe3v4BPpB8JZpRL3pFePPK5hHsCP5", 1925.00067211 * COIN},
    {"FaJU6bqj9T4pgb9fEnTxF3H9ow9SVQLVV4", 1900.00016142 * COIN},
    {"FdPJdJcEXg9vFuZ2MXnLpDdBaUDRkLSGPr", 1899.13282366 * COIN},
    {"FgCdAhVHUa7CB9csHoCThJZd64mMg6oUuV", 1892.95520967 * COIN},
    {"FgBh5nRGKYVUBzVe5L35eKvZj7arf24Chu", 1850.90481847 * COIN},
    {"Fobu1BsxzxE2SJM6np2ozi2yF5CN8EXH6o", 1814.03737010 * COIN},
    {"FgqsyvQnPDvK2LnAivJSyfJ3Am7Yo7zMMB", 1798.49791038 * COIN},
    {"FpDCtrF6zvoNqG4caMbhvmenihS3qRKgvS", 1794.01931643 * COIN},
    {"FjFwudB1m9teTMF5LGtywWpcU3hbsBABma", 1777.49435443 * COIN},
    {"Fn5keYFdoX8LkvmLHpteSJ5sUiPRMAkShg", 1750.01047396 * COIN},
    {"FetDA6EWDrwz1uS8JXtXT9hTXPzuYvnoaD", 1738.81260523 * COIN},
    {"FbNQRSQ3ewxHp5EoDMtFp832JMptKnfmt6", 1719.91697481 * COIN},
    {"Fb1gcdfQaYmDpA2rHob4JADC4run68b39Y", 1716.47797457 * COIN},
    {"FkkyCAaqLjpmfN2RgNP6rLzeQqFe7VDWxr", 1700.00024186 * COIN},
    {"FsPbVXNxUh7zv8Min1kuSg7VPBf4FRPKZW", 1620.41237786 * COIN},
    {"FZCGcnFyj2eUxxucjJwBP5faoWvF5Uihyt", 1605.59451190 * COIN},
    {"FngUJy7s3PtVWrTmkkFr769CyTaPbYyLhe", 1561.10065619 * COIN},
    {"Fkxac1EMvYoLxsu1S3SocDu8gAnD3L1rJP", 1518.15324998 * COIN},
    {"Fb5JTkg2HXgLjSqbi7SWRbcPryrrTg3t9o", 1500.00015836 * COIN},
    {"FZ52Zh984rZaTS84FrPt3FW9Z3tQGyM4r8", 1500.00006773 * COIN},
    {"FmscKq7qg6GHLN3ra8y5q36osW2qoSDuQo", 1499.68580000 * COIN},
    {"Fjqjw4CCx5QnmBsfmWgmX931rpPFVdWaAb", 1481.61815587 * COIN},
    {"Fm1xQAwVSGrVk29QLy42sywk9JCutPQJz2", 1464.24657313 * COIN},
    {"FnWv25S2MsP3YUuwtZg8Bfe16C9XfiAQAT", 1463.71676532 * COIN},
    {"FkLuVZabu5JLfatKuEsZGD4DYWK3WWgiTU", 1455.26461775 * COIN},
    {"FperfdJRM2MKAMoNXiSfTMrdmCo6brNrb6", 1454.66140692 * COIN},
    {"FnSnUXjoaNJjzqbSWkvkxLCrfD7WUCFzsP", 1445.47060000 * COIN},
    {"FdgUcSgBhYdRNN2tiGwRG3JrPM4CG3RqwY", 1403.95078808 * COIN},
    {"FWJ7mYjPjeR7qkrjCf2j4MpvMcwD8FpV6E", 1398.43875271 * COIN},
    {"FsTZJDyk14pKkYurNhXedkGw6ob21pMNwF", 1396.47060000 * COIN},
    {"Fe1exsh8GNzyunGaVmUXmaoXy2A8Xsfq5S", 1385.10593029 * COIN},
    {"FjW2era1qNf4sXbcCTNX31g2NpZipLZRcT", 1382.83628425 * COIN},
    {"FhvTsZZPV83a2XqpyXduECe8fmuuD3JZdM", 1347.78830773 * COIN},
    {"FgUJ5d8Fqxj2wYnkL1RP8sBC1vtTGhLqJc", 1321.15788599 * COIN},
    {"FnsEsixJz5Gfw8NMvnnfRMhKfvSawVa5be", 1318.18423313 * COIN},
    {"FXufwsVKha5wLPZJT4AtQX8Lnh9cZeVUKu", 1300.00035370 * COIN},
    {"Fs363vepGhpGSQqcvgGD1FJsTPnXqXFj8n", 1265.85350726 * COIN},
    {"FrrZwcUkLHCgBq4JM6SXjX23Xs55ku7iLy", 1247.67795586 * COIN},
    {"FdQvYF4npWhJgDtdYgLvwN6js5cWAr4BrG", 1225.00007050 * COIN},
    {"FbW1f813QRLyMdaWSkLwj2hVWtVVikM4Xt", 1221.00223222 * COIN},
    {"FiSGN63Mr5WRZ55aTtztZGpL4riTEwHFre", 1216.60201223 * COIN},
    {"Fpa9spGQqtsLFcQm3LLjvxGYqnHBNDAKhy", 1209.39717759 * COIN},
    {"FZdEFBqqEhK85CwqJb6aqChmJWNEExuM52", 1201.16202941 * COIN},
    {"Fpw7W7zqfVpxQzY9RauFvtoPTsA2LW4TAr", 1191.21775894 * COIN},
    {"FfdXg6unXo6DfmtFa9VyQwLW3LZWh7tzYn", 1183.39277710 * COIN},
    {"Fb3hdw8ycx2q3i45XgxEGszKrkukZyWwjC", 1180.46771979 * COIN},
    {"FWZx2eJQLDakB5Z9nFLSDmg9uFjMBQbpP2", 1138.93257403 * COIN},
    {"FgJVRBg1waqo77itFgm4JGtPNwVWFredsC", 1127.54246686 * COIN},
    {"FpFwF3Stx7YHUWzFDVayRxoqBTxUrU8Ty6", 1125.43796076 * COIN},
    {"FiJ9uL5eXab6jXagYJrRCaH6aVJkVcoJPw", 1118.82088131 * COIN},
    {"FWZYd3dAqnh1yxY6TpfA7KoDX5odR6toz1", 1117.34803737 * COIN},
    {"FindtoCwcFdBx8pgKFQJNwz83FfwEcsGXQ", 1072.80588539 * COIN},
    {"Fq1od1ceY6NPEhqJ3GXdrpPsPwr2WxoLgD", 1050.91417379 * COIN},
    {"FjKvkFrM2sRMJxFyA72MeLFQg27cCsMzhw", 1050.00000000 * COIN},
    {"FhZgxNRqPBC3Q3QYN2iX49KSK5dpRD3BSd", 1049.17189871 * COIN},
    {"FmVqFsnHjAZiccV29UK4gCGCFYVceBG42R", 1046.01727536 * COIN},
    {"FgYd2VZPoiHrTULz2M8Q3q7mzuCvattEsE", 1041.70533816 * COIN},
    {"FhVKqJepsN3L12EnkYuB2bLbTjbsEpEVAJ", 1025.41002991 * COIN},
    {"FWGs93avtFfUdw3mTcKZy3HveKyKk68CNu", 1010.21856113 * COIN},
    {"FW2qFhZMC2k5uC96hd4wMwBhvtAjfGUQFS", 1009.07168630 * COIN},
    {"FrCn7X9Yhr5RmvjDSBm8LG7cNrgG6gQ7Kh", 1008.54072427 * COIN},
    {"FXeFragwaWg4LsEjswRhixe7ZL323unZ4g", 1003.04997467 * COIN},
    {"FhKjMkV8abGhJbMwisFRYvELfh5rBT7xz1", 1000.00000000 * COIN},
    {"FdiDpS4DY3JahvPZABMgKkyXpTmkmSYjyF", 988.09932973 * COIN},
    {"FbhJ2MGcdc9i7y8MhBZXpcJM5PQDDJAW8j", 976.16445413 * COIN},
    {"FjRjCHBcy6QP7MR83Wfo5h65A9q7Pwd4q6", 953.44383400 * COIN},
    {"Fmnu4g9oXS8eH7ibhnQLdVU5ZYPk8iHtDQ", 948.81621857 * COIN},
    {"FhnTZ8j1bCMaf4qfzu1VQpbVKrmuvRb6PA", 943.14419238 * COIN},
    {"Fhsc21vgCESHuX3GRbAAMienVKWFN9vSSk", 937.25165451 * COIN},
    {"FdHCdQuv76Uqec8TxXQsaR5AuyrQq1xTQd", 933.43526678 * COIN},
    {"Fp6p4tqid93zSBvXnUpJxGiUx4bXwLDdBG", 921.38847173 * COIN},
    {"FjkGvQhCXdDhTgmX1dCji8tDwCTvXNbxJ9", 919.30106780 * COIN},
    {"FcZsPcGD1hE49rRP9wEmenCi9hkY5qPmYh", 913.09627676 * COIN},
    {"FcXb9SE6HSUhntcQiMPrcAxr56WcD9CAFM", 907.50586356 * COIN},
    {"FdCQGiKWiqmvhNxshoaXUCxnRz34wvwRGt", 902.50802319 * COIN},
    {"FiYkxLbi3WsHBbjbeNMaQApo7NZgsZvxaz", 873.32145089 * COIN},
    {"FiwLpcNpQ3wTzkwnox2X59bQMCVuWQu4sm", 850.00005596 * COIN},
    {"FbWLzCw7FSuR1UtLMf9Eu2XEF3LkxkDzYe", 844.51854687 * COIN},
    {"FpbnC1aTDQ9hXwTbJYuGZ86ukGUaBvScGw", 807.21120537 * COIN},
    {"Fk7m8MkNvWfkhEqH7A9cEca5h1PxWCRLHj", 806.96848501 * COIN},
    {"Fj635kSy2ewgbbQEK5sJFp9GpS6cTz6ZU2", 794.78332328 * COIN},
    {"FYgZxqEp1F8AFGEQASfqpCkHipmMvftdP8", 785.09852482 * COIN},
    {"FejcizyLtxQ19Yo4RU9wsBtBcBWGT2FFV5", 782.70014252 * COIN},
    {"FjYMkqLehdX42tJ7Lzt9YaYobDXgKzmPbm", 775.96283000 * COIN},
    {"FqaGKV6ckpXYH6gbn2ZiwArVFpj5KJgjzZ", 775.20398040 * COIN},
    {"Fhhtan7xeJGM1BzrCE9HjLr8VDmWN9bvVN", 775.00021139 * COIN},
    {"FVohX69fMr9Re46kVgYCumXAbksecXtQR7", 775.00006873 * COIN},
    {"FoBgJBZ8attLTPHXCTVjcuJTGK5ftbximP", 762.68940357 * COIN},
    {"FZCh7zjQE9Q4ReFNSp95AAB5J5JcdLB66i", 759.48040000 * COIN},
    {"FeqVU3BKPKz2fST78jwLhKrKFni6eXfYYp", 751.04457835 * COIN},
    {"FfongwqVxeGV3nG7ueVHneL5BjtkdVqcwq", 750.00001127 * COIN},
    {"FZ6gtgoKinmq77myD6hz5Q5o9d76UBM5XB", 743.99793673 * COIN},
    {"FaLW7SDUDtNThPLTw2y8q7Ei4Ui3Vk6q3b", 708.55267761 * COIN},
    {"FWm9aME24nvFJSyVyv9Ljk2AZreu2eJnTy", 702.83545678 * COIN},
    {"FkCQQq4hUYn3L3QtFdRxPHwsZUoq9cerQp", 701.63062420 * COIN},
    {"FW43XUmp6u8tRbVcTvzHNswiSFeMpZCcXQ", 699.98682655 * COIN},
    {"FfVQWDPXaEqfdpnG2DeQZDRf9qCL41xGAH", 694.75495583 * COIN},
    {"FeGXFCvof6C1xw94ZKvrxhMWx6C6EBJ18r", 672.22840190 * COIN},
    {"FZeT4BkGxf2RXnHzBPnqPhVpAMhB516H71", 668.48030246 * COIN},
    {"FaD7Qr2K1MZPUy4cPunm4Lxsj3ef6bhcoH", 663.66430018 * COIN},
    {"FkzmaunpZck35vcGEFVHeTDvkUoiHkHvAh", 655.30013901 * COIN},
    {"FqtFF2QKRTRtcQmBPjJS3yFKogi2Ah3fWz", 655.16638929 * COIN},
    {"Fpf4Wc7qcATK5YZRL4ngMer57wwdPuBV2P", 654.96268983 * COIN},
    {"FZ8riuRQernqNWyzxvaUKvM4NFW5CMtE82", 640.26064033 * COIN},
    {"FqH349AK7uzoQngC1zsh2iBai1nsqn9H4d", 633.17364151 * COIN},
    {"FjYdhQZLVevVVjLxfr9uAfoDKSzVrkxiVT", 625.00106780 * COIN},
    {"FbF2xR1hhjcDMnMZ5ihQMfKsm8aKir5M8w", 625.00000183 * COIN},
    {"FYtbXdrqGzuFuBgGYsCSJYwhm4xHZkpAPd", 623.28182399 * COIN},
    {"FjpCmRNk92Fkx81rxsfed2yDjZNyzhst17", 618.25138443 * COIN},
    {"FgHnkcNWub2rvdWxUWnaJv8455N48ZfpLB", 610.26598318 * COIN},
    {"FqJvTD3bwyGyXDqSkGUc63VVKnBP38RTyu", 602.08564531 * COIN},
    {"FX41FkBx5BH8ZGB8TUD266g3bQ7LYtKNJa", 598.99507054 * COIN},
    {"Fc3hJzpTYn3Fx7MQz2qgaEgQesWXhBcJio", 583.68648184 * COIN},
    {"FpahuhRAn9BTzLRagybxBxi7ZMoSxfEjhL", 580.85011639 * COIN},
    {"FsS7CuzqMqaarHgKtvo6apimDPJJANT3ZU", 577.70299780 * COIN},
    {"FhvrQmCKw5DhHbz7AEJvNmNUfBjW5CeWG1", 575.00000000 * COIN},
    {"Fp719HmFm9YWud7MGotjMYJZoErVHkiDY7", 574.44999529 * COIN},
    {"FVP2TfqBna2EhbJ1Zy6NxpMiv2aEde3F1T", 571.48771209 * COIN},
    {"FgAFhWuJZMPW7Y8Zi5xSJiKUVqckiNy4eH", 569.58114728 * COIN},
    {"FoLRAh4EGsuqxPyc44Y7b1EGDeR2mP59r3", 558.40600673 * COIN},
    {"FoRLTLR6J2w4dhAtTzpVLbB8yYFmrjuPqh", 554.10000188 * COIN},
    {"FpDLVaAtrNan5zmtEvpjyW6cWt5ySqxEHt", 550.00001082 * COIN},
    {"FqUNSrB4jDwxtnwgB8CefywwPhcoKFN4NE", 550.00000675 * COIN},
    {"FqvvjQRARuxJUoAyRf8EdqrEiEhrb1F1Y4", 546.35933497 * COIN},
    {"FWNYcSG47WPnBjTzK52D8r1eWr4nvaw4wD", 536.51648372 * COIN},
    {"FobzMVX7933TnzxyTG3P93QjxaPEAeNSTn", 534.85253431 * COIN},
    {"FrfTW9w6gHtJ8hGocrswXF499L3VEheLBV", 533.05613110 * COIN},
    {"FsQtFtnW7Zf7SXo91M4vReJR9PmYii4HQ7", 528.47259961 * COIN},
    {"FsmCEPutGLYKQmQQ3EhSAbsAWTZzoPpeQx", 525.57630102 * COIN},
    {"FmA5gaNM3GGfWeBgWbgw7WVaQLWx5fYbLC", 524.67831237 * COIN},
    {"FXAWB6BMWagRmMWaQkbLoFmbnSf52ZEeAH", 515.40073277 * COIN},
    {"FhitXjVSJgDCCz2vYjpT4tw4w7VfmXtvkB", 507.62750000 * COIN},
    {"FYT8Lvu7FZR8DKBaguXWSdSXgniPuUxYa6", 504.37045000 * COIN},
    {"FnnWZ8atn5VoWfBJP7skxe9v19jsqFLeNK", 500.00001451 * COIN},
    {"FZaffutU35DkTF6AGJ37SHX1ycGhNFUzha", 500.00000000 * COIN},
    {"Fiive7bU2TkrQSeZwaSBk29ZzKaA233LDV", 500.00000000 * COIN},
    {"FeUibgp9UYePmzVLByZfZwqm4sVjbcazVc", 490.40581360 * COIN},
    {"FcYQpGiebv3RkUkhJc3s7f8GSQZbvHvvTt", 487.27100596 * COIN},
    {"Fkjo4jnp7hzYGaMmJzQYKiZ6ZA3gzX3coS", 475.00073302 * COIN},
    {"FsquJLUjvcQ9vDTEkwe8MAQTcNeVLAhEFX", 469.69198002 * COIN},
    {"FdwdBoYj1N45FmjVPfhcDyDCpEJ19WxCFm", 465.49020000 * COIN},
    {"Fp3NFXihbaXhdrFiJfAocX6S6RNgx7X8Li", 465.49020000 * COIN},
    {"FsqUYMzrCPFXVA2pvr9schiCcR7LyNAVqF", 463.07006056 * COIN},
    {"FqAEkoSsNwApdrQXWrkbNSinYedsQ5pCCA", 457.75696431 * COIN},
    {"Fhwk6dobUJz3XjJAg6gwYBdH1gMkyCih5W", 434.07277873 * COIN},
    {"FVhjcR6vzKAjiCutWq1KWc1p6HCzsPJFko", 432.10636272 * COIN},
    {"FmiXNaUV5iDwhtgs7xwx7ZNv6Eh7WwypM4", 430.20000193 * COIN},
    {"FiXCxsSNd9Yqmb7GwGXYLVKL5MWzwCej2W", 427.86344772 * COIN},
    {"FbuKCnQeQk8fu2iUkU9oU82AhUyeaodnU4", 424.69736946 * COIN},
    {"FqwEcWeipyq1aiR6gsBxwR1iS2p512DvM3", 423.12621524 * COIN},
    {"FhS2bSS9c7mKWYCLp9Eeq3K1Ub7o9TCnyu", 415.09739135 * COIN},
    {"FnNsUb9WnGfFgp2g3QkFT1V8tX2rPjp8jo", 412.47225549 * COIN},
    {"FispUSthH3rqjmzUz3EAdfFuScDGvfosaA", 411.42699896 * COIN},
    {"FhJqYq4PKkZE3Vy62imkTwz6Qj3F9341Pr", 409.96197028 * COIN},
    {"FpoAw3oJkJhkTKykpRR9kGidTbRCPqdbC2", 405.94178004 * COIN},
    {"FdXKha7DSj2XgN8x7K1BoZQko8ZXwVBkcw", 404.05002362 * COIN},
    {"FeQwCpJgAEfCSvrDReSpBfNRLJyRvWAhLx", 400.00000000 * COIN},
    {"FbF6d4xyyvNHqEss4D761wUG8pbhswnvKL", 397.23467331 * COIN},
    {"FdxfQmFr45ou6wRTZ8qMQ275PxLyK5LRQN", 397.02670974 * COIN},
    {"Fm9TJzwfoPcNGhuC5AB5JUtHctimgfnSB2", 394.99999478 * COIN},
    {"FizP5dXwfX4X4EcCPcMG86bmDvYyn7FZWD", 388.10713807 * COIN},
    {"Fdh4bWR8Zyfbtxu3ZdWkREAvfzvSDNRiWy", 384.70306500 * COIN},
    {"FXRqWpfVKo7SnhN5qgoqszvimrVZviabyU", 380.33354560 * COIN},
    {"FbhVvAKUx6Gh4pe9fpTXk8mtx5A8t7bSnb", 379.35000468 * COIN},
    {"FVWysVZcMKwub23mqAKsUs62LMSGiVC8MJ", 378.00002597 * COIN},
    {"FoNUfmzaKW4JniRtVYCwq5z935npeL63c7", 375.00000000 * COIN},
    {"FoT6DSNGWYrSuSFwdWpVKi7YbkqAyCrq3w", 375.00000000 * COIN},
    {"Fkrj9A4mz4QWmvaVUds9yuHB7moFUeQ8Pe", 374.99940837 * COIN},
    {"Fr7YuvCjZ2QyTvAJRiq6JBWdELHZ4p8d1Y", 374.95276059 * COIN},
    {"FjCXMsN5YZhsku2ihbkFEgjbcCH1qVw59d", 374.78170000 * COIN},
    {"FbYTmCpbudnenKFCAUwHxwTQrukQtcr2vr", 364.58787999 * COIN},
    {"FaBgTTbbBNprihz4Qcu776Xg1iyTwSeRgr", 363.01006260 * COIN},
    {"Fed9vvE2bL6yP5REGaXufTznyTyFHVgkRw", 357.08588507 * COIN},
    {"FhUb9T82CYtQPgjens8Eo8BJxBfZT7SVsk", 354.00000581 * COIN},
    {"FYvmHw9fpKV8tDBj1vtxacikzwTzMPQgqd", 353.26641481 * COIN},
    {"FdnUsQG8VdffCiPAhEqVgSotqcXEd6jnkn", 351.95003146 * COIN},
    {"Fkvp2WHH27DGg18NZZfGyHcY4ZpcRFuDVG", 351.00001603 * COIN},
    {"Fbtys2gsGAN5SBYJoHqviKcx7SScQkvUKS", 347.54240052 * COIN},
    {"FmA9cPuaQRTw1kgzCZerx23qmedbQucUcd", 347.02701287 * COIN},
    {"FhdR8qHBn5Q9i8KvBgtWUfy5ySENQmnFLh", 344.66171534 * COIN},
    {"FYwz7LsTV8tBR3Tj3v3TNQJwPUW4pLLesW", 341.75536676 * COIN},
    {"FYe6CyssR5jT33fvjdVeZ3e7ZDPGm3cxcK", 339.66445753 * COIN},
    {"Fpi11xynEkvF8JKTDx1gsY243xTZwBQn7d", 338.74371529 * COIN},
    {"FWo2nqmVoSSSAHrjYawdtZ3yQDa8VjVkBD", 334.66343052 * COIN},
    {"FatZLiehYh6xLmqjGKg2YDbVWtsPHKEiEw", 332.58742358 * COIN},
    {"Fip4SPJenEUZizsxcVsaNRxhGMKWq36giH", 331.79475493 * COIN},
    {"FsqMLQkEraZXubXyPAttTgYjQx7GmdgxgM", 328.19204539 * COIN},
    {"FXjCppuvzyTvMLMam8kkkmiK2HEa2sVMUQ", 327.55007260 * COIN},
    {"FbndTEkocLWrNMZk48XSQsjjdFLxDmYVPF", 327.00000000 * COIN},
    {"FWNZWs7e2ARHMX6fb4hC8NfwFoNjaYiwP7", 324.54928245 * COIN},
    {"FdxjA5kXFyBaCVcoEefhPLyWiDXvNAstEZ", 312.09444757 * COIN},
    {"FbZK3F9yZJk5sUoRtAcG8M1Xx7keUopMeW", 300.95002136 * COIN},
    {"FW6CRenuTd7pPATBChvU7Ldwntpts3A6jo", 300.00000000 * COIN},
    {"Fd7y17SD2he8fLzxgHt15FY2KHHyFA4wZk", 286.24923963 * COIN},
    {"FgpFvYGMzX3id7mYRnxiBDGxbGhfrt9LYC", 285.91738188 * COIN},
    {"Fers6onTAFNbv4WZGvxsFgMr5VW2uPtWmC", 278.35000639 * COIN},
    {"FWsW3XHkAGHoV2ijysAfY1t5KHjJeig5f3", 277.10003904 * COIN},
    {"FfrUYmNnmjV3NvBiVzJ6GtkSWnzaPswxge", 276.95011041 * COIN},
    {"FpJVxQ7LxPczFjJwDr7r7zLZY3JQT7AVf8", 271.31938306 * COIN},
    {"FVy7cJAGzPRQ9ZiwjZwBG3XNL6dKQN5KC1", 261.68959399 * COIN},
    {"FWfAiA719inBCXLDSxw8HcsAvWVYvexYr7", 260.21266338 * COIN},
    {"Fje6tQEKkykd1zPSroeL7ULtBJb9XWb3rv", 256.52984985 * COIN},
    {"FXnt2JwW4aHd2DxwCY2Z3CLQ3yVgr1dyvM", 254.59444714 * COIN},
    {"FekSA6Ed1sjbr2puSpRGKUhHJLEVkZA5Gc", 252.71590656 * COIN},
    {"FXNKUvF5iMsGqMUCJDvXXhGNHEzQecEmBU", 252.05000582 * COIN},
    {"FideW2jT7tbVBV98Se3VrJPBnxMHnFbJps", 251.95000000 * COIN},
    {"FrPFUFKUsB9DUxM7tmBwYTj6yxLpVmFCpp", 251.92573884 * COIN},
    {"FhWhD3VUcSW7HoqEEuU9tKKvrFHUMaNQ6y", 250.95000011 * COIN},
    {"Fju531Acdg6Y86Dk4PkmW51h9KqXzXUkaQ", 250.03029291 * COIN},
    {"FVqMWJ8GTHE1nqqGCr8C48LyKeLSTyhmyT", 249.18938305 * COIN},
    {"FmvLY1PtJAzZGipwKXD1p4hgyoWoXty3Z4", 244.97869102 * COIN},
    {"Fc455WTuMte59fDmTKig72Ptg7tc9Ftufc", 244.67812389 * COIN},
    {"FWD7Cf8VZLjZwxkohjExQ63AgHVRtgVXMc", 244.58060999 * COIN},
    {"FfLcrey1jd2Z7HdZFgQp8oghKZmPtDZVGD", 241.21937656 * COIN},
    {"FstYH8DCeBVbLLvZQphzBP4CTrHnrUajny", 233.63574510 * COIN},
    {"FeAwBTCBvQv8PAKg16Lt2FfU2eSe1ixj13", 228.35000297 * COIN},
    {"Fh88RdXesPDhkyxJYUX6URzz1Ds2wzcQLc", 227.55000959 * COIN},
    {"FkzrZ6u8GKdZD2r9Z1vQc7WhH243vjDtPF", 227.25001383 * COIN},
    {"Fo8ZsBmBsEK8EFfoTuFcJDJ7VRVq5J23j2", 227.15001099 * COIN},
    {"FjfVVVXNUp2Y8jfuYL1tJAX21GEYHkL7Uo", 227.10000000 * COIN},
    {"Fpo1tBVZhRpWFmFBzRU2XvDDVEZFLmQ9LW", 227.04124377 * COIN},
    {"FjgiHYdAsGzaZWKGLByd95uYBFPoxmMeLt", 227.00000000 * COIN},
    {"FYv5bSUjpU7asj5LLUNE4wYi6b3gCAr31A", 226.95000039 * COIN},
    {"FmW2mqd1ydmGuXDTVwYP5tcRvhuWPaQDw9", 226.66834355 * COIN},
    {"Faxqoqvw62Pj3so4YKDuYSwKsfknAtueZF", 226.05003860 * COIN},
    {"FnJbyCoV7feVNdTCjCYkafiD3JnAaA673b", 226.00001766 * COIN},
    {"FXjPg6jHUX4rrxWTfNAo9MBmsPKXkvD94G", 225.95001128 * COIN},
    {"FmTQXJwbLb7wSdirFFH5Bx3PmW3eqzCqL2", 225.40541639 * COIN},
    {"FVfW9Xie1sqRPziLbzesszxhdmcQuNr1Km", 223.26926309 * COIN},
    {"FgxJHd2ZwuS9ZkwBYeQGTHwHEGwUMBX4M8", 223.11693183 * COIN},
    {"FpoyvgjLEDuw8cmvnB2pgagFMNu1ZrJGUf", 223.06545319 * COIN},
    {"FWkqz7BH4cfajLPmx5hKVhALGff9GQGCeW", 221.57715002 * COIN},
    {"FoYmpR4qA6z6Q3Xx9WYdvpoiqbCPuPRmNM", 218.18358008 * COIN},
    {"FVXPr5UeWK49K8pPXLGK8hWy3s6b29Wmd9", 216.93674009 * COIN},
    {"Fakwx5XiNMYrHWvSQHvwvhV6krj3KqAbs6", 213.95402959 * COIN},
    {"FogGsRoZN4jkCzLkUgo3woTwZGP8cLY5Zq", 211.14637380 * COIN},
    {"Fb8irWyxenkhNb2wuKjFS7PvMLdZTWQ1oM", 210.45638737 * COIN},
    {"Fce6dvK3V73RGnGtoS3FTxp65HFnYCxHfS", 208.63477562 * COIN},
    {"FgdwcGVqRacL1ie9enJNtNUov6huBhfhKS", 208.59940977 * COIN},
    {"FWTbGdRYvhsaYGXhVf1iv6nQUbFHzN36Ex", 207.62067809 * COIN},
    {"FnKwWvuNP1fVwJ9Gf4LS8j5qWQVjjr6mNo", 204.32710152 * COIN},
    {"FaoosHrGC9JaCVKjTFVc583Qd7tEN9rjGe", 202.85000824 * COIN},
    {"FrVmQR6xjZGCPv84a5tggA6kbggXxoc7ss", 202.30000000 * COIN},
    {"Fnv6gu6gkHgKmbcWKQJpSvHHx3WChRXpQw", 201.95001596 * COIN},
    {"FgGDsL97tBpYQxJHkE91w5FWWiJ2bLViCa", 201.58714569 * COIN},
    {"FmdVQyuc4EUm7HoSdwvEsXMVEbZznrB57x", 201.25005723 * COIN},
    {"FWyG2E1CLtx7CaVHdXRUrzQ95mRGM9qweh", 201.05000641 * COIN},
    {"FkmcREEdrUuoh4g6zHpFHxPMieiMG8BKXP", 201.00005027 * COIN},
    {"FiNEfcbTFMQcapJHa1BwLs8JngwxoTCfhB", 201.00000261 * COIN},
    {"FmWoB1owdQsP9r7q9nksrcsLR4C2LTCUsS", 200.95018717 * COIN},
    {"FkfWqwSgNsC93L1UL9L9LBnQZqUkEXmt3U", 200.95008259 * COIN},
    {"FsqbpnZTJGrvPk48inCdBW88urSjoEsK1s", 200.95008188 * COIN},
    {"FpB9wYi3vgq3VWbzbzt7TzzuyrFnWMoKFk", 200.95002344 * COIN},
    {"FbKTj665Fu5yARjmM5pAfNc5HoLjT86nV3", 200.95002282 * COIN},
    {"FcsupaNwdmuCU9zFCZkdgir37faCA2uzYr", 200.95000847 * COIN},
    {"FnrZep21cKneGK3EP5sj15fNHi1UJ3jCBW", 200.95000455 * COIN},
    {"FjSz4EUN67fpQvDLNPHH5FG21rYg2MB9WX", 200.95000000 * COIN},
    {"Fn61M8gxcKjUnovhCssiHLMSVCGcnDqtiu", 200.95000000 * COIN},
    {"FjjvSVqKFkBVXdv9exTLhfpAYKCfZARxgJ", 199.99990063 * COIN},
    {"Fgj5EEspXqqAZTfD7xLW6suqCc17JeYRWr", 199.94194124 * COIN},
    {"FiP5igyQvnF527iPnAPsmRHsNFKXGBrFLE", 199.86092916 * COIN},
    {"FnFQLLWZydMCeLw3VCxcb8whA6fiXnjcgv", 195.99656621 * COIN},
    {"Fr1QiB5XAEe89QbTdun9Fh4twoANuBSNXV", 195.04521547 * COIN},
    {"FaaPEw6z3yHYhRy3XUpWUT3smFMJoW6HH2", 191.20207956 * COIN},
    {"Fc2kBz6UjSyWYNkuwgXuS8Y3QNjUUWMySz", 190.21425489 * COIN},
    {"FgciUrn6jBwyWJNz6JtDTUW34G3xBQM5qN", 188.69077798 * COIN},
    {"FrttdQ3ZYShivcx2unEzi7nv4sP59Z65fW", 179.06294016 * COIN},
    {"FpPqrfb8DitpUkgaKC1CrW53GgS9TFWK4L", 178.35000906 * COIN},
    {"FibLxEdohdmw4J7PRwsTXFaqwxS5cP7pjL", 178.30000000 * COIN},
    {"FtPYoEYq9S9y9rwnpWgkBYsjwb2FaaYBJD", 177.95000605 * COIN},
    {"FsPL6gghgrNusPA2pxKSAeu557sUK9oCcV", 177.10004500 * COIN},
    {"FrWPya22WtNt4evV2sGybahj6Mky3fichn", 177.00000000 * COIN},
    {"FdSx5bUJNAwJ7MKHPV1U91bjhNzM7QS4vT", 176.95007469 * COIN},
    {"FkgCnKjPqjdEaKRRbhRzfTQFrtWyUPCcp4", 176.18851096 * COIN},
    {"FjJU4fyqHkZQmBShuiiUYT8A1S13daPSsW", 176.10000591 * COIN},
    {"FsXZRtB22f6RTX8W38wfKQpys8szL8vF19", 176.10000000 * COIN},
    {"FYehkTnj9A95BXDVMLoRxYvLMUYoCv2Csu", 176.05000901 * COIN},
    {"FWrmSAaNk1omDxRhspCYjmfxNM8C5djfpk", 176.00004341 * COIN},
    {"Fp99pnbnDGNfhmRw2iMEafkXWUD5pexmhS", 176.00004268 * COIN},
    {"FasrkStV2LkoD9msByJHrrrBURgfkhew4D", 176.00000390 * COIN},
    {"FkPKUC91JWAmfQuHB741f9TC4yNp2bVcEU", 176.00000288 * COIN},
    {"FqGj6cN5M4MMWZy5nnX491B4VVVg8mrU4u", 176.00000000 * COIN},
    {"FcGzfKDnwQT3KzkajXJcB6ZDamLCHkj68d", 176.00000000 * COIN},
    {"FjM3uyG9VQwfWNCq8QKhJDfEPoLTFucPC9", 175.95002037 * COIN},
    {"FikBqL62ghSKwaZjg6cWy7eVkWgTuea1wM", 175.95000982 * COIN},
    {"FjWXjbaWuX1HhtbX6GwVCaxdNunSSZtakr", 175.95000792 * COIN},
    {"FXsaCaHK5Kgru7g7FNwf1bLdpQaGQz6ean", 175.00000692 * COIN},
    {"FhopqrPWn75VVhCzrGduP3iy6ZWzwRWN2v", 174.96630974 * COIN},
    {"FZ6FwC43ZtDDewtHsHJjqipT8A5ezrMmMu", 173.55086536 * COIN},
    {"Fpvdt2R6EjUUKSQNVKPKbVNF1LPPow54AD", 171.80340413 * COIN},
    {"Fp9P3vLnUT9CzVVqsptDzsWEjP1t2Y5hDk", 169.89247304 * COIN},
    {"FiDxCoSKhHNdFsVdi5F1QNd1AJ9hY3iE1b", 161.59698719 * COIN},
    {"Fp4NzMNwjG8rm5X1k3vEuZcUtq1Rg4VNj3", 160.88305131 * COIN},
    {"FqmpxASdsA2EsDSudZV3wS94k1FbBRcC2Y", 156.66666667 * COIN},
    {"FciS1Bh25nQX5evz1Bwr5KqUtoYVyXXVaX", 153.20004645 * COIN},
    {"FebaeYuQi5oQXY89wqCjBErnJZ4mfUR6Fz", 152.35000000 * COIN},
    {"FnG2ziJBWFNZj3R945Ffy5i7udbTXQYjaf", 152.00000000 * COIN},
    {"FW5qS4d4crLAUNDhAHTaBLF6KgmzAbxsSr", 151.95000340 * COIN},
    {"ForLsKgXfTURezjKC3C1dHCMJgodYKkgBf", 151.95000000 * COIN},
    {"FkCHzNe6dVeaAaFkayfndwcv4LaSkz8nW3", 151.90000000 * COIN},
    {"Fsn4SJS5AFWPLkoMR88VDAQRRN7fqbG11Y", 151.10000847 * COIN},
    {"Fdav7cVEEbFmVL3epwvLnyeWeffjeW47n6", 151.10000237 * COIN},
    {"FrEF589WacGtz3X1k3JL4JHsB2iSGCQCst", 151.10000000 * COIN},
    {"FsrsCc4hqQuZjmv2rAn9nsjZArPa5smVtj", 151.05002623 * COIN},
    {"Fg5Qkik6gNmVuMQsTeFYTA2pdYDRZ2FfJP", 151.05000394 * COIN},
    {"FoLB1XMsDisD8vUojYpzMRpKborDijdttk", 151.00009482 * COIN},
    {"FVyFupashnB3Fm6Bz8JGp5eEZQBiFCknL5", 151.00007739 * COIN},
    {"Fh1F288aoUgSyaycTdmcThAJv2qbXjDhwy", 151.00002827 * COIN},
    {"FpRt9jZfMynHQLoVQs5e1GACiU67NTKVoX", 151.00001809 * COIN},
    {"FqSpD3nTXqZxub2YpgpwjK9PXxnHvEifFx", 151.00000604 * COIN},
    {"FZbBy9N3VLNsUt5fgxX7BmVWxXU97JP9Nt", 151.00000000 * COIN},
    {"FY9YsbuMku7nwRBjAm6KvRy1EmBoGGribt", 150.95004340 * COIN},
    {"FeLe4ALePN2xaaghKFAfozJK41tN2HAuMd", 150.95003169 * COIN},
    {"FjY3nQp7hLeKnVwmTSQuLTCA7oDdH9x7Fo", 150.95002484 * COIN},
    {"FqEaBDqQpVzuccuQSKc6TSR6rQUb8oPvQj", 150.95001513 * COIN},
    {"FbqaNEqEoK6q2ELjnCmGffLz5PjW8TDbe1", 150.95000917 * COIN},
    {"FdyUbJtnKdqMf7LaMSumKJeLJDor9iNjQd", 150.95000248 * COIN},
    {"FZDFMj7BchmRhDidYrxpiZG8iWK8XwFdxL", 150.95000248 * COIN},
    {"FoTbeNmNeY8AVWRRH8orhD6usgbtkwTSyr", 150.95000000 * COIN},
    {"FfFTmZD3kBbTsdzoxsY1u6YQPPksnce4Ch", 150.95000000 * COIN},
    {"FpuT16aQNo3XyogKaSCvHZyS2uzbomksrW", 150.95000000 * COIN},
    {"FbL6iHCDBqdsQbbauXkrng3CVJTJoH5BnF", 150.95000000 * COIN},
    {"FYBP8HKKXEbM215ua6s3Fp2FXX6BhFqiLR", 148.24442176 * COIN},
    {"FfR4JRjtNFJ2gpP95Qza6Dno9XJUjCbx34", 145.64274571 * COIN},
    {"FrmVWXbrKHDeprMSec6KZu3P5QzVudSfUd", 141.44722394 * COIN},
    {"FderYtoir5rjmsjRLSHVoULKuEjwfcCUox", 141.24463955 * COIN},
    {"FtJ7DGaMhNMQDo99GwwiAwjugpSXTY4aXf", 140.31673552 * COIN},
    {"FcqE55ztp9hpArqxtwrcgN5tn3wMLMR8kg", 138.46052365 * COIN},
    {"FjJknYU2CVt3FbRDVsjQnGw5BBxmBvh6wu", 137.85454571 * COIN},
    {"FdTFD9PSYM54Hq6EVLoGQtBRcfpUBBSzhe", 135.82855075 * COIN},
    {"Fm4p9ijiSAc4kwry9wH5Rabwep44sZBNJG", 132.04178549 * COIN},
    {"Fe2MQK7Wq4fdP9wHQ7tDdarZYEXrn8pL4L", 127.84975720 * COIN},
    {"Fb5ZXnB8F6BREDQnW9X6XLZTEmdfKXTQB7", 127.35003217 * COIN},
    {"FX4JoQUXnHUN26Wy9xUkT9T7kZGa5bSrKn", 127.05000735 * COIN},
    {"FdqL4nWaheTnukG15JqXmtaY1bV4gN9Tnv", 127.05000187 * COIN},
    {"FXPCGRZirEGdeqTHZ1Aj5BsiARu7FD8amr", 126.25005201 * COIN},
    {"FqvoP2mpZvVawVfnXvr6F8wCmXfm3EBrmc", 126.25003858 * COIN},
    {"Fj5AGMU8Eg6VaNGAmSUba53EespEmgf9VZ", 126.20003551 * COIN},
    {"FhsQUJZJVEMrepGKTWhHxesBzVw8vBK3Vh", 126.15000647 * COIN},
    {"Fbsvxhrn4U8BsxtekSyK5DN9hYHh6mnKEM", 126.15000000 * COIN},
    {"FZrw6LQ6DSxQA3ZyJtpouoinqWZ8WMokj4", 126.15000000 * COIN},
    {"FfyUtQQxvJbGtF11YhdcCggFRVcgccsJyN", 126.15000000 * COIN},
    {"FWTRC1B8bEWA71wHT8Ro23YwBxDF8Uas8G", 126.10004096 * COIN},
    {"FbokJwXFcHS4eennardoZkefnQfr4gi3av", 126.10000334 * COIN},
    {"FW3o2zgdVEsf9BNBAojpjMHe2CHjAvGUXz", 126.05003973 * COIN},
    {"FpsrBmjb38eXoig7vjeXQvktk3hWR5wyj5", 126.05003543 * COIN},
    {"FmYMMkgfexZ4iM1eutdcqDf9X2Mpx6GzV5", 126.05001740 * COIN},
    {"FriTKr4imuKW614G7y6p8oqyqHhBd5A86e", 126.05000621 * COIN},
    {"FaQ2zWtUFrAK6pVZoQ5H4KrB9YiocYkT4n", 126.05000475 * COIN},
    {"FkCxSqn1TKvpjki4ruNnQ7wAjoW9BLncer", 126.05000000 * COIN},
    {"FhF1bEVF5V8rDJwehYXGfvL36KcpzyePof", 126.05000000 * COIN},
    {"FkkSc6audxtoGMxaBnYYMUM4mLiEmzjwSD", 126.05000000 * COIN},
    {"FoeLpAmoW71JpY2YnmX62vXQAuV6vkEZ1v", 126.00028900 * COIN},
    {"FVUcHq2cDMeDPyEsamfxU7fwJiGMNVReJA", 126.00006160 * COIN},
    {"FXvKqiS79ynmMCZ6JTmnpCTXjUVqCoX2E2", 126.00004531 * COIN},
    {"FZJDFBbM6LpCaWhiK9PGgWkvvPA2vJ6RGM", 126.00004421 * COIN},
    {"FaqL6cik7vYC5EQ5WERV2u4d2BnGQf9Sjy", 126.00003741 * COIN},
    {"FVU9yhNiexKdH5RZBHao2uS3hL4Qwd3Lqq", 126.00000000 * COIN},
    {"FsS6vEzkByamZoWwbpSEcaUXL7zxkaT7VF", 126.00000000 * COIN},
    {"Fhp2DTxuq2wddQEqqhHN4igcdNU8GgR3Hc", 126.00000000 * COIN},
    {"FWHXJLgYUU2PcM9Co6S64asVTQ8qie243R", 126.00000000 * COIN},
    {"FZ72oPgRfcW4S1LB5CDQ4voQgPyFa7c8Ay", 126.00000000 * COIN},
    {"FhweoNx7jrx7FfgZ6e5nfpmKFLoCmTyv9M", 126.00000000 * COIN},
    {"FrH8Upg5UxwZYW7yPKH9BjcXgikEGx4Aiz", 126.00000000 * COIN},
    {"FmvMJLcjLYaj7gQ5tpkuZVBQ1L77t3NzVx", 126.00000000 * COIN},
    {"FdXCNBykDkJtjqqGM8g3YdvUCiAtZrEvkW", 126.00000000 * COIN},
    {"FYmqgdFwtu6KpGb5Qfk2Vsy3ryVKiEf38U", 126.00000000 * COIN},
    {"FeZU2r1U4gycQQ6qyGLzc4sRdTsoZPzUq4", 126.00000000 * COIN},
    {"FizGo3hiMSeFxDEWh83DDaHv4mg2cST4nv", 125.95002892 * COIN},
    {"FfVH2vEZ8yAtFgfbhxkzjx2iEVaE16pe4o", 125.95000440 * COIN},
    {"FmMaTxzaKad2eLw44SrPe2SzEQbSdxRVHm", 125.95000261 * COIN},
    {"Fkd5ZH1PxuoDGLa98aS1T4bvYihBUjVwkP", 125.95000191 * COIN},
    {"FXiLEJYgjfmBEbEqjmkF2KfNNZ49obPAv5", 125.95000011 * COIN},
    {"FicDoxpkMHaTePwaR7ppztDi8CTD4Tc1hE", 125.95000000 * COIN},
    {"FVTEQW5YsHHqHAMz6zDV1c41ZaF5QfRhqU", 125.95000000 * COIN},
    {"FZA4BJ7ZT6XFc7puQzinhZfGx87iRSHPLH", 125.95000000 * COIN},
    {"FkXMF5h5zQfNN9fM8cMvLjqn7uJ3JDvSXV", 125.95000000 * COIN},
    {"Fg4tiqz7D8DweUBTBBuGP3M4gGyfYSt1qC", 125.95000000 * COIN},
    {"FVekhf6rco9hBvXkMdyuH88ETgXxWmWAvz", 120.48640000 * COIN},
    {"FmzjPZCkFmMEvq9KnrSE5rWEmppFVTDPBB", 118.76838708 * COIN},
    {"FsU3uxywZtHwb3mK4rT152Mw5WySMe4cuH", 118.25980192 * COIN},
    {"FfzLNPXCaTtg5J2RB7AvWWspKqyis7o2gz", 118.09819036 * COIN},
    {"FdmjWvRNAiKtSC2iFxSRTKKxiBMpb8acth", 116.39712942 * COIN},
    {"FWPYGULLw5p9s4CGdWNyzEjpHpXooLzKpo", 115.16222027 * COIN},
    {"FZqrcNLnSNzQfnwpUKpoTfReET1cYfR2F2", 113.38049341 * COIN},
    {"FgnBdY42awtPU8SnRsUzZXqsQuFeEaJ2LK", 112.21054522 * COIN},
    {"FmYauGoSeoA6yr8m5d5m3haUC9EGNVBBd1", 109.99998111 * COIN},
    {"FpPjNwCZB1gHxGZsmY5m65YaM332xx1oJA", 107.38350853 * COIN},
    {"FfJey9VWWrkQYKxsufbZ1Z5HY8VxsfTQfn", 107.37656161 * COIN},
    {"FmrRrSuDiSM5e4KaRMomX2BHQtzXAVpGfL", 105.54168787 * COIN},
    {"FtNLfSxYD2jzPZqP4WkEsCxCuvcnpxPCvK", 103.79546714 * COIN},
    {"FbQjg158kiGkFTiGqBtnGZKBruegNbyKWJ", 102.35000000 * COIN},
    {"FbVXRLR4VR1nvvMkyqA1rwbGe3TQ9BT5v7", 102.15000000 * COIN},
    {"FrcgYphyyWMcXV6XktC5NfkkuZJ2sv4bpu", 102.06280000 * COIN},
    {"FhMk9mGxpf8EaQ5FvdbsGXQPmSizPXnqDK", 101.73308626 * COIN},
    {"Fiaocj9vhHyG8xDQnSAM9LdVcWiy5bFm4D", 101.15005177 * COIN},
    {"FafQMSqW3CMkWoP9dSwfFUXH7zbnvVk7uS", 101.10002515 * COIN},
    {"Ffc59v1KrN3B7LyVBttpqhqgGrShiHVeey", 101.10000368 * COIN},
    {"FZ52o3RCdfsc3HkRust1bAxmN2NxAyFXYL", 101.10000335 * COIN},
    {"Fh9dBmTuarjRWUEyaiU7128bTkCVJttEgP", 101.10000000 * COIN},
    {"FgyudinHAtvCDv1srPhMZTb7poFFNUWxAB", 101.10000000 * COIN},
    {"FedYiQijN6cjGDntRX7pePRAxmXu57JByo", 101.05004167 * COIN},
    {"FoJM3pME5XAEpsRPFtjECuCG3y3cDHak25", 101.05003128 * COIN},
    {"Fbm5kGmwQLsZjiTrW7tWpetaotooQyRYFn", 101.05000523 * COIN},
    {"Fk1YYeahmJdubjHDkHRTUVkDz2ZGFJccoC", 101.05000000 * COIN},
    {"FXxf7pk9QSwNLvzCncZfxf1eCW7D9oiGvN", 101.05000000 * COIN},
    {"FY5NzSDaiUzrCJTbeyThBmmNuMGreBqemA", 101.05000000 * COIN},
    {"FdXyzFsrsUxNXjsCtxra6qddNzP4tmkbY5", 101.05000000 * COIN},
    {"FbwCRfZpBdkxroTjpFBvhB7WmJSwVpDarR", 101.05000000 * COIN},
    {"FjSPAdGqmWUqW4LsRJb8desigQxoYyS7RW", 101.05000000 * COIN},
    {"Fpmr2h9aFBU8kJkz126n5unvymoUMn4qiF", 101.05000000 * COIN},
    {"FeJsh3MyDoz9sfUVasswDnvk6M2RXSy1t7", 101.05000000 * COIN},
    {"FWzwXoaGjZWtBftRybnt3Fer6cvV6vyeQc", 101.00006509 * COIN},
    {"FbURvtmwQg5C5nECKJMw16w3TkTqVbyoi4", 101.00004807 * COIN},
    {"FroU32kDXQh3UYJrKcHg3SJUSPkBEdSFxZ", 101.00001559 * COIN},
    {"FeGpw4nrCBwUYDzqKTdgyvWFMJHeHiHBkT", 101.00001144 * COIN},
    {"FYJnqrwhWtJAzPX1wHN8KijkTsqXK8iAoG", 101.00000482 * COIN},
    {"Fq4Zy2j4pja3fQ45Qc7p3EAehFtTZojoxC", 101.00000296 * COIN},
    {"Fd24Ao6BxyyAnQFEkJ9SPsZeAtnvHJ9yoE", 101.00000193 * COIN},
    {"FfEAvGXw9EVPevM3SJmVYvcbGi6yE1EP3U", 101.00000191 * COIN},
    {"FirRqGaVnr2mxMm4xMwdQfresqXY4dmjv4", 101.00000023 * COIN},
    {"FkF4iQx4iAgNqRGq2N17PqBKKbPtdhc1P9", 101.00000000 * COIN},
    {"Fh8gbwivCBf3Hk6qBnHMst66ujV2ojiHrD", 101.00000000 * COIN},
    {"FcMjHWGiYWUa4F5FTqpmGySALD6D1kpUeg", 101.00000000 * COIN},
    {"FWcsSob5VAfCEr4K45CgNeSsviM4qr7K3v", 101.00000000 * COIN},
    {"Fm74VvrVkQ3VHQQ8YxkdpxJDSsK1zssh6S", 101.00000000 * COIN},
    {"FhjxjxEuPm8RY66Q2G82mFXfJKWwpjtw3H", 101.00000000 * COIN},
    {"FXRrKP9apMfVT41kajesk2aVfTdP3Uvxge", 101.00000000 * COIN},
    {"FrZNXK3G9cbQwwAApJ5nRqLc13gjFcW6ts", 101.00000000 * COIN},
    {"FaT5R9CgXVJUqMjJgjNCEfmPHPCRULirCi", 100.95011269 * COIN},
    {"Fp4C2fhTgLECWWcZcd5cFNsz8PN8ppr18P", 100.95004374 * COIN},
    {"FgR1c3HvLVUCBo9Arx2w2nX77mnpVL3T7M", 100.95003888 * COIN},
    {"FnPZPxYSsEGrz1xVjjrF4Lo8cwu6QjuWPF", 100.95003756 * COIN},
    {"FpXrDDe6sZnk1Ba48rpnbwXNMeUs3fjbta", 100.95001094 * COIN},
    {"FnsBNRABj4zAYwxT1hJcp1JrwXD1J7jVHt", 100.95000960 * COIN},
    {"FbrrJpUZGT2a7rPWzqZYCGYkoGMSRgG2JV", 100.95000823 * COIN},
    {"FdnRNPVsRZ3MSsi2Yt5FhxCDUpdtDXfMNc", 100.95000465 * COIN},
    {"FsyAp5axhFT3wUSYba8Yt8TzGMX4wvLq3W", 100.95000405 * COIN},
    {"Ff7VVNDYWdsN3AKzMh67SMYZzgcZ5uiG36", 100.95000394 * COIN},
    {"FpgYJNtELpLuVBHLM7qpn8MMnUeP8342xb", 100.95000288 * COIN},
    {"FWzX3RJ1VKppYDa1Np3iWMMpBQWwnFvRYY", 100.95000190 * COIN},
    {"FbaiCAhiBXPFAZGmmnHrvdzSFbTNFUVchX", 100.95000000 * COIN},
    {"FmvVQMTGGuQy4Fyu7qkVXatKLdu9xmBSED", 100.95000000 * COIN},
    {"FgKqAoLiYDtnQJkBzbjxVh3ZXfi6qhRhLR", 100.95000000 * COIN},
    {"Ff7cUTLzCeYKKMAx7gp8RCbrZRPDPaU7Qd", 100.95000000 * COIN},
    {"FYhgcbsw63i2kLoFVvXNtk8PgcFpWXezaq", 100.95000000 * COIN},
    {"FrN9rbYTUvc46Tqo9aTnmyqFCdt4s8NsTK", 100.95000000 * COIN},
    {"FkeASZ9bSG3Ax8YpruRMPJKJxnqY2u52oP", 100.95000000 * COIN},
    {"FjiPpX4J9CFaegr3kxuZUSojmhpDjXLXmq", 100.95000000 * COIN},
    {"FmGcUheYoRdSt8HxHLChnpwJXfvP9u8dfY", 100.95000000 * COIN},
    {"FowxWND6cLfAyidXmWZSpCTu7mV1A6oXPo", 100.95000000 * COIN},
    {"FryyCrivrFt4zE8EbHXopBjNH5Nkf9EXay", 100.95000000 * COIN},
    {"Fs23yGPQUsc7q894qjPRF7SxMafpP4PZib", 100.95000000 * COIN},
    {"FiWeMSAtGdMYsdeErfkeqsnAw5bi22x96S", 100.95000000 * COIN},
    {"FksvEw5A97TGAoZ4JrbhcXsPeQviWUDmcC", 100.95000000 * COIN},
    {"FmxRXWUS8v4LyUvcnFrdhrdbL1TRFVPqLX", 100.95000000 * COIN},
    {"FVrQpMxN9epG95mFdnVuEodpNGMi7wFbCc", 100.95000000 * COIN},
    {"Fnqnv9k2XDDntRVhJavykB1okJG7RUV63U", 100.95000000 * COIN},
    {"FYeBCnvDkMcjEzdVVk23sHHpfDkKWBQds9", 100.95000000 * COIN},
    {"FgPrzFKMNMJ9M5kRAyVG7cjfbeXRFRxV6L", 100.44671885 * COIN},
    {"FXbMcX7Gcij3ApsEujg4vHmXPSMRTZQ2H5", 100.00000000 * COIN},
    {"Ff4ppYqg45YdBDswos74qPFWdGvTJTG7zW", 100.00000000 * COIN},
    {"FmEg6dJFWGikTdQSZTXEXTPoEoAjr7scQn", 100.00000000 * COIN},
    {"FiLz4SopxPjLXz1BcMpFGwaceCimtLWbbP", 99.99999521 * COIN},
    {"Femr3B2Z2237F36rkgcsLBJWWaMhbjLJHJ", 99.75587857 * COIN},
    {"FfeUE1szdCFHUpVYmVdVovnhp1iDt65WiE", 99.12471883 * COIN},
    {"Fqt1JyCt92CFhsYDbHGMe71GfMxBfkzXYL", 98.99999295 * COIN},
    {"FtAc3JoUUVzjsFsY9dHGe39LCRSkTkLBUi", 98.99999295 * COIN},
    {"Fo51EHjAPopUR1MpZpkAFwr8waB3Dw8R1U", 98.95234669 * COIN},
    {"FoQxQjCHdobgrZxnrGzwUEXxsqwm9gL912", 96.90643390 * COIN},
    {"FbaUzsaZdsM9WhZeQd2ktUTfQb4JiRG5e9", 92.00980967 * COIN},
    {"Fq5P9YcRVvHvAooDr4a4LFFZMKwxJUNqMy", 91.97136030 * COIN},
    {"FdQwKFQbZ7x3KPSpQFZcZPJ2dmnfTFd7ty", 87.54756376 * COIN},
    {"Fgu8exhRnbCCnRrJ3stHHcgLoDo6FFHTE7", 86.10040180 * COIN},
    {"FZbnjwg6B8RXQYP4iFk1e1b2oXWgSk44xP", 84.89827391 * COIN},
    {"Foq4Ynry1oQo9WGqrkdQZrpZ4Cu4czL8VA", 84.70640661 * COIN},
    {"FpvHG9gzoZBG6pFHwmaHJvXsAg43oWcCAH", 83.47714864 * COIN},
    {"FqWRGA2mSqCrtmyRTuaZGAoabbVg2nzYZ9", 82.96121004 * COIN},
    {"FmrJgYadeQ3oz6gCK5tLXft1fDdGvznYbw", 82.78704230 * COIN},
    {"FazU5DtEh3SZUfEC6RPiNPrNhZSJXWy3Mn", 82.61320676 * COIN},
    {"FjwceqVVjUKtPKKiuScX1c9mGzBv3dZdCz", 82.14303400 * COIN},
    {"FstG66PtdygCQ6vrnYhw6nb5v35dpB2A3b", 81.02492617 * COIN},
    {"FXpEVVFiSMBeVUxaTYT59WNCwT9gDQCzMT", 79.22090583 * COIN},
    {"FisetBTcPd14YNK4NbtW29fRbhByQXj9v9", 79.00001591 * COIN},
    {"FjuxycHvu2QgKRhnUx9wx4ZkT2GVTnwCno", 77.42177938 * COIN},
    {"Fh6KAyKNU1fzNacFESJq7VfKze7WoeyGZH", 77.25000000 * COIN},
    {"FpS9HXecyDe5PvF7U8rMLqpV4M3CcibKYh", 77.05001354 * COIN},
    {"FVadB2nvUe7FpBEotAaQp3gvtHdUc2rnCf", 76.95000000 * COIN},
    {"Fszu1hznSLWGpBHZktBZktgPssuUM6oGfn", 76.90000225 * COIN},
    {"Fi2g6cKy5d3xd1nmvKeQ2UgCo99XzK1vr7", 76.25000578 * COIN},
    {"FVh2z5zvCgH9V9MEyc4KDNTriWUFkSLqbz", 76.15000000 * COIN},
    {"FdZ3bfp1FJY4S1P3Qg3FWpLtMLS4BMEAqY", 76.10004076 * COIN},
    {"FgJw58H6rWpKZf43MAgPgfFnBTpKndYbDb", 76.10000817 * COIN},
    {"FkkfuxMk9bVtDJYqE9HNXE1UyViFjc9MK6", 76.10000000 * COIN},
    {"FiGC3iBaj7e3yiTHP6uhZfhKp3w8NJxPSd", 76.10000000 * COIN},
    {"FijtycwHVXAZtDeVH5jzVRoY4qxt1THAmh", 76.10000000 * COIN},
    {"FYykm9L6gm1jH69rB1T8hBpGBQpyPV2o6j", 76.10000000 * COIN},
    {"FfffyqysUmZ9TUA6U2xMebhgvCuFJCo688", 76.05003131 * COIN},
    {"Fm2fJBumer3nRw7SpdzBT4Wbb2Rg4Z8Y5J", 76.05003044 * COIN},
    {"FhvyjmdutzoLBBUZEYzjRSpyKZVwyxU4ri", 76.05001609 * COIN},
    {"Fe6q87oqCJ3tR5iSAzaXUDNVUdpgdJumFf", 76.05000518 * COIN},
    {"FdJWxT1Di721HwUEbMzjqgmA48Cx6qdozm", 76.05000456 * COIN},
    {"FdY4VTfGACKjJjhx8qmjWsugV3uUcTxJhN", 76.05000297 * COIN},
    {"Frs7f5NBdmckGveFPwYYh5vc8zoa9Qgbsv", 76.05000280 * COIN},
    {"FVduTkQYJpRL1G6yqrb7akCkQJchvqaVKw", 76.05000000 * COIN},
    {"Fjveiw6ACJ9KGRo1EZQLadSh9M33XFnZrw", 76.05000000 * COIN},
    {"Fb8KRwm7tErBHuvZEADUycMRF5NA3b6a95", 76.05000000 * COIN},
    {"FsMe218Q17o27ZW2tiLRwCkj1JjECeDgMZ", 76.05000000 * COIN},
    {"FbC9WR2StEgnCgQgDewg6tJiPT7fBpjpz6", 76.00002993 * COIN},
    {"Fhq7JFTKDPFQJ94CgTtqFR3YtbqxFhEut1", 76.00001827 * COIN},
    {"Fc8NjF3jCY7Q1D2tQQTQjTYZFaDERNyQS4", 76.00001525 * COIN},
    {"FeUbjtWAZG9ewwEA56wHx4NtrbtEPhUnbx", 76.00001025 * COIN},
    {"FZ5n1ycmA7kGHXngsm33bXTs9KxzVRsJ9A", 76.00000717 * COIN},
    {"FgYcmSyTMUcaFnCvM9DajtUQxDZvUhnevp", 76.00000623 * COIN},
    {"FZubSQpYFggCh1eqWv2xSy7sH3HKrrAuo9", 76.00000578 * COIN},
    {"Foyy7d4zGuibK3JieLrkDGHojR2ZdMhTQ3", 76.00000493 * COIN},
    {"FZNVHpunzYJhRNuCp2iyoRpW29Ysb6nyTL", 76.00000291 * COIN},
    {"FdosF73GFh3J9AVWqX7GHG4iNym9nqGLcX", 76.00000251 * COIN},
    {"Faj4Q3cdbWk8FZQgiCsdbRtRPmKb4PCGkr", 76.00000226 * COIN},
    {"FqFptgqF1xsFMoZwwygWoKKanUzEDkqUqH", 76.00000191 * COIN},
    {"FbMLnzRurEhNZ1fLtbQNcEpEzjFjEWCfiV", 76.00000011 * COIN},
    {"FjhkiWzNHdYXxiMY1LqLtPs8SC6iM2incx", 76.00000000 * COIN},
    {"FsQzWdrLnNu5eVZciCLxjqbmb1U3Nyv5FD", 76.00000000 * COIN},
    {"FWXBLJyF32ni3CUhN3vjJNFVZtsVGpYzE8", 76.00000000 * COIN},
    {"FbpMn1Ar7LUGk8PhKFx3eoNaBkpbJcbF66", 76.00000000 * COIN},
    {"FdGr1xxDHd7L8xTsoTC2dQbuJtdcs1GSnC", 76.00000000 * COIN},
    {"Fc6S26BfJ9YWAhcuVfGEgbnfkKLex4y7wm", 76.00000000 * COIN},
    {"FgS1bsNYbSpPdWxosQNWMBPDWSsCBkRQem", 76.00000000 * COIN},
    {"FmW2jFA2cLTu5jvPSV7VNwzAit7h3qKMds", 76.00000000 * COIN},
    {"FjMbbpvaNs5ST5sv1AxdASnFT9jUWC9zwd", 76.00000000 * COIN},
    {"FmDQzN64jkZyfKAv1MAzdX6H2P9i5PJNCr", 76.00000000 * COIN},
    {"FmGLyqSomfN6dKeNt7WnTb5RmDsgfJDJkQ", 76.00000000 * COIN},
    {"FnXBHXumCKgzbMVwdTsVgzUABW4mhEJrvn", 76.00000000 * COIN},
    {"Fi2Hj7uKVopr24zAXzYBo7Xfg9Hv5Ttvfm", 76.00000000 * COIN},
    {"Fc9idCGSCZYBeYmQcAyTqzJywtyvoHQ8eA", 76.00000000 * COIN},
    {"FWfMauEj4QvXRC1rrDYFNqW4EeHSdmSDGq", 76.00000000 * COIN},
    {"FrEr8eXRmqzHTZe7MUuBKyGtdNgYjuJX7n", 75.95007650 * COIN},
    {"FqabrdvyvVhPv48ujNVNUEURyRzMiZ3Y3B", 75.95005620 * COIN},
    {"Fsu52YpQbXwjt3QkoKV18w3euL2gsvvmei", 75.95004219 * COIN},
    {"FYNza93SdHhg5BBSMBBkLcQY79sL6U12J7", 75.95002124 * COIN},
    {"Fa9SnCSETJ4q8EtA442z9X59smpDk5ppvB", 75.95001094 * COIN},
    {"FrdwufNWyYfzFjWShntYYQHYjG4AnJ9FGe", 75.95000907 * COIN},
    {"FZnAeM3A9tiGyqXsXL714LJyTZzQH4vitY", 75.95000850 * COIN},
    {"Fo8QvFBgtrMShhgCkeSNKCczumcLRKNx3Z", 75.95000581 * COIN},
    {"FYtp598dL5XyXe13Xh9iK6m5m43vpcSJjq", 75.95000285 * COIN},
    {"FbR8HTYVedebgePMfZkc6gydNoWAcYu33z", 75.95000193 * COIN},
    {"FkvpWk2QZtmtyzwWzYwhd9BMuNkDkKDeTh", 75.95000000 * COIN},
    {"FfvL2ZrbALSbGFKf5xg19YSeN34ywABQGr", 75.95000000 * COIN},
    {"FpjxKYqkcKCexrrEGizb9vSdixuNMubNwH", 75.95000000 * COIN},
    {"Fo1dsmSr8zkUucbNp7K7eWUo3AsQHQSE69", 75.95000000 * COIN},
    {"FZvaZqDnMN1rwL8BnumtFDwgtywky85ctw", 75.95000000 * COIN},
    {"FohUg1AEnmf6pnvQsTwxZMmUZq7MhkirLU", 75.95000000 * COIN},
    {"FdQyXNkJfbdSHZKcogGA1HKazuXpb3ADfj", 75.95000000 * COIN},
    {"FbLT7ciQZEZKDPz1K6fMno2ucUHXcUSN8E", 75.95000000 * COIN},
    {"Faowa5nSsaMFAoy8pT942CuJjPpSgD5XzN", 75.95000000 * COIN},
    {"FntS2ZnF8zd87Y6Dqwid4E5b8PNi4J5fag", 75.95000000 * COIN},
    {"Fd7CpnuFwipcyeoWK9nAup6NzkhxubG8jb", 75.95000000 * COIN},
    {"FcQwX2FbarkWRGFm5taTouMfe7Ck1Dx4fQ", 75.95000000 * COIN},
    {"FVvn9d6myuRseDShJjUJcGVYLaFVR8Kas8", 75.95000000 * COIN},
    {"FXKT3YuQepsi7wc9QoV3GV8C2JL52XToAy", 75.95000000 * COIN},
    {"FdBdqaWSE3RYuxgZHcuB3a1zhYZ817eGgj", 75.95000000 * COIN},
    {"FVoUhv1KJEshEbwze8G4J17dV46Wrtr7uP", 75.95000000 * COIN},
    {"FWaTGLAEW7cxpJyNsFKzyQaQv6Rxw7Yjcq", 75.95000000 * COIN},
    {"FZUxomRz4XZWFnAg9oNby4uwCrySULk2j4", 75.95000000 * COIN},
    {"FkqwU8tUU6U41PuGTPUqy93hML4QtCDPDX", 75.35000000 * COIN},
    {"FmEtWLtjNj9N1T1pZZcZBvnvjNPZLv92if", 75.07577802 * COIN},
    {"Fc9JQm5D57ENzAfGHkYVMP2ohudxYVtKt2", 75.00242833 * COIN},
    {"Fjy7kPe2ThnVdfdWv2e7hKB3veAYsoLV7u", 75.00004503 * COIN},
    {"Fhbiw5ami5fFF51kqzYcEi41TVqqyMirZe", 75.00000926 * COIN},
    {"FhhCt9LN2Q3CJeS2UykoEFamRUvFPCdqBA", 75.00000558 * COIN},
    {"FYj96jexxZrkrouvwKcx2REJCGns9uDU6D", 75.00000000 * COIN},
    {"Fa7EhZkWd7fuvJtbPCEccma2MtZmKrHNz4", 73.06128223 * COIN},
    {"FhJhAZ49d8J3zURzqrHPxaxMaMrtBbmrKJ", 72.21664303 * COIN},
    {"FWTMszAnuxQhQgyxDLZx56t92xcc99npah", 70.89704853 * COIN},
    {"FkoBPjsyJzdDLdz7GRYNthdHHgSLnTy9eb", 69.55124345 * COIN},
    {"FfjqzozoMPmcSBmeqgZ1wa9G9DUjAGa6ns", 68.74401381 * COIN},
    {"FmrHQyykUDmRSUmak2YxtDxNoSr8dGpHit", 68.00351535 * COIN},
    {"FVLq5Yp32m5R1QwSWGJtgdVfbupHrHm2C6", 66.28253163 * COIN},
    {"FjCgKQCrhrtnwBTTjrVipbTF9uuFEvtxND", 65.48618301 * COIN},
    {"FjFRyTwgsBL5jjmMm9fcsZFEjEEAaMWhCP", 65.02792439 * COIN},
    {"FbubwJxVqsGXLPSRNVvVB2Cz5QEHhB6vtc", 60.77571932 * COIN},
    {"FXc4uRUj16NwXPVJ6qmhFHwYTeXABsAqbm", 60.01857956 * COIN},
    {"Fan8xWTg6cm2TkS7gEoJxtyibA7bMRr1CL", 59.10080792 * COIN},
    {"Fe14PgxkBabn9RNCDejS72apPLUpNAKMoG", 57.66187925 * COIN},
    {"FfCkGQRpb14UnXpvWwrMogFhRQRQcxDMDu", 56.10357537 * COIN},
    {"FVGxgoHx42eC4hc5CNFnCqztJFj9A8L597", 55.47794847 * COIN},
    {"FaF7qi6DXNdR3Fhcpf8J4ZCV8DAF6hePaT", 54.73325449 * COIN},
    {"Fj8uoHWpjey6JgZzdvk7c5novenTZY7abX", 53.81186016 * COIN},
    {"FjZeUngKbdYnhrD1n9UCSKjwKagstNRL5v", 53.75336890 * COIN},
    {"FsusbEc5adSfoji4sA4vyRyDFeggEWXRe9", 53.19417699 * COIN},
    {"FckMjnUAc4cW5iF8sdTV8A5yF1L2VDiX84", 51.95000000 * COIN},
    {"FmYA7ANJmRtxGriUR8MMJX8wcLg2aKkezg", 51.42072876 * COIN},
    {"FhzyvJkw9RurhWkK3pzcuMysKV1mWB1GsD", 51.20000193 * COIN},
    {"FcFkjmyRcm4aTiTdajam8fcQh1Fpo3Wkvo", 51.20000000 * COIN},
    {"FhvvvXn2iasE7tE6x6xwohF9kpVKCGchmF", 51.20000000 * COIN},
    {"FqnE6kK8iqRhqPj6y4cReaHRLbTS1iWQ7u", 51.15000294 * COIN},
    {"Fp62gTbhrX32QiF97z8ADDs2YkuFuSPX9K", 51.10002821 * COIN},
    {"FeA3X8ZQQXF8mzFN64SEC2A5HcxHbognZq", 51.10000000 * COIN},
    {"Fdo4PNDm9oWyrYvXShD1DRhaBKZTNePaMg", 51.05004256 * COIN},
    {"FZ5XYMsyQMQXpLNVRhdpCfVcDmhdztopZy", 51.05000028 * COIN},
    {"FeT5H5BhEAC6JnVqi3HbhGvvm7UGMVdVEJ", 51.05000000 * COIN},
    {"Fn86gNJtk9zmYpeWAsNjUL3yrubB1Yj11Q", 51.05000000 * COIN},
    {"FcoSyBALMS95ZDGATzhxU72joLmv6w48m8", 51.05000000 * COIN},
    {"Fe4ZTBv8VA6kXcJuuCdYbgkv961MW65so8", 51.05000000 * COIN},
    {"FdGL7K7Rff8HUrNx5MvnnNrhS4zp8e85au", 51.05000000 * COIN},
    {"FsoDCugzihVGeoGs8M4Foh3fbGU1ggobHd", 51.05000000 * COIN},
    {"FfWrDv15Z9i4ggNUPjT6B55DXZgtZVukR3", 51.05000000 * COIN},
    {"Fd47UF7LKxCpCuk5xgh7A2UpJdV72cDBPy", 51.00008749 * COIN},
    {"FpaptnYjE5acittHyDDmBtkLPBwkB8xaS6", 51.00003942 * COIN},
    {"FjNRkxEfrc6u33w4PC1B26b8k3oFmWr2FV", 51.00000729 * COIN},
    {"Fkjcq759jnpCBSonKo4L12NkHa8xTHf2uq", 51.00000678 * COIN},
    {"FiSHzSW1UMGBEFvrbx6Sxzk1RRWqQEoyc7", 51.00000367 * COIN},
    {"FVVnzeYHi6BZR3wdu5AT8AdFNSMsuvtsv6", 51.00000072 * COIN},
    {"FkQoCiUPMpJFECF2WxsRh4M97vxMibSqUd", 51.00000000 * COIN},
    {"Fsz23p867yAy7gigKEcyHxzrC3cw1d719g", 51.00000000 * COIN},
    {"FoDyHFtGbJvxcuHgMCS4jEGARxePpjZu5s", 51.00000000 * COIN},
    {"FprH1nCbuvuJKKJyGo1Jas8fFHZjqo3xxL", 51.00000000 * COIN},
    {"FadU6BknEc26jcEDtg726zUENZCQv1Nh5t", 51.00000000 * COIN},
    {"FsqDWkEUu8UvsskHQv28gGfmWjRkavjokk", 51.00000000 * COIN},
    {"FpZcS4T5vACd1gymTRUeA9qSegUKjWEZc1", 51.00000000 * COIN},
    {"Fp7UyL9qCvSyXEVCXZWzfwaBXJHMi9e81W", 51.00000000 * COIN},
    {"Fmh6kG9U91s1DHmzKhGXU8GQSJYgaxBTn9", 51.00000000 * COIN},
    {"FcSbe6HDZfSUVW4YkkpCoxtRxP6zCP43C5", 51.00000000 * COIN},
    {"FdH8aYSvUF2zmgM2SEYQuTBDWytS9JaHdj", 51.00000000 * COIN},
    {"FfzPWiSpGrh1jYEDqUB1RfxicDD7iDEdUK", 51.00000000 * COIN},
    {"FmVJF9pQ1oVmeaTFhHuPzqvdkEK3FxQW17", 51.00000000 * COIN},
    {"FnARtq18B6ozdnxgFpXE3UX7kf9MjvYg6d", 51.00000000 * COIN},
    {"FZavM99X21GJLW2qDLGE7SXeevuaivQHcx", 50.95002236 * COIN},
    {"FcWrwR4yHo4poZNbeEiB6sjpXrQpRV2MB3", 50.95001084 * COIN},
    {"FocQa8heCaqrwbkkJB3gmiLu4av8PPwR8S", 50.95001064 * COIN},
    {"Fq6Cr21uYktsdCHYVQYuc2zVnN8wKkAjbu", 50.95001023 * COIN},
    {"FbXvpYCGAxodS9mnzLU4SxYW4DzTtkBdTu", 50.95000101 * COIN},
    {"FY6VSXDpTcysFePrC8vERj9zcYQVdPvY5U", 50.95000000 * COIN},
    {"FeNM4qvKinYi5jYzFEMCBkJWrugSNbD8YM", 50.95000000 * COIN},
    {"FZxvf5QtGjBhXqHa7yUAMXzguKuMuQzGjN", 50.95000000 * COIN},
    {"Fh32vfryGgCRBNRYj2NTDJFyne81opJVRK", 50.95000000 * COIN},
    {"Ffp53s3zhKoXk28q2UjfwqrpJcqTVPYpaN", 50.95000000 * COIN},
    {"Fj8BWHXsy546wc5Btk3DrzhnwDHL4exU41", 50.95000000 * COIN},
    {"FqJaN3x9XP9dEUDmUfKTayixiZ3WJ9SChi", 50.95000000 * COIN},
    {"Fca2jUEYkkR5DymmSrHtEXpSjXx3XAebyE", 50.95000000 * COIN},
    {"FnFJ8VtJtZ3zjt4pq1FvaaBWiZ3Azbdh3V", 50.95000000 * COIN},
    {"FYaCYkHJxP6XQM5b3Zat6ftJqiokXsfisN", 50.95000000 * COIN},
    {"FYeSRtNAoVfR8nFWyUUbQYdDuvx6MB346U", 50.95000000 * COIN},
    {"FqV9PvsjvkqRYm2LcR5oNMSwZgCVdtmC4K", 50.95000000 * COIN},
    {"FZNKXvbd3D4JGHs1e6tZWWFbAKwjLptepf", 50.95000000 * COIN},
    {"FkDFFX4oS5UbD7xLfUkwSg43V8aZzKGxrf", 50.95000000 * COIN},
    {"FhJY5jZTXjBF152918X8x5mSydhvPhxMTw", 50.95000000 * COIN},
    {"FfFi7tf95Pk1HXnELEJLzQKpqHads5qJBT", 50.95000000 * COIN},
    {"FiAZTw2NuUkLBVvAkmmBKcfkqRAQCQLff3", 50.95000000 * COIN},
    {"FeUaK8ssCnpynPvejsR7MdoJSawUnjtvNv", 50.95000000 * COIN},
    {"FWJw3vpdFsRXLHQ1Vjq9PJFkpkmqCLemZg", 50.95000000 * COIN},
    {"FshGXu6BXVDLUWpAb1ZftDW8judbKeUeY9", 50.95000000 * COIN},
    {"FXGmz3faqwqQ6Gf6mYcp1YBaAvhZAXH8rY", 50.95000000 * COIN},
    {"Fjtfr1UddqshMKR3cWPde6wKCveqJtJg4o", 50.95000000 * COIN},
    {"Fny2VvdHMiACwEctfwPhqLuXEQKNvBLyLL", 50.95000000 * COIN},
    {"FigpdsTiEACRvzUL2YhpNsCLogQMyiqy3L", 50.95000000 * COIN},
    {"Ff16yiZt4Zfm5odjJm2DtFgisvSqJFA4fL", 50.00000883 * COIN},
    {"FeUNiPcjgLq7JTXuAsTkcVd6XHURy8aeTN", 50.00000862 * COIN},
    {"FdCRUP8S1FQcDVFxwfP77aemNvWa6vGbBk", 50.00000000 * COIN},
    {"FcYMvvY4SnyJo3boWLneMb2wNxEsNS5w5Z", 50.00000000 * COIN},
    {"FjCRCfLCtn1JH52eiHeB762bW6JH3iEmwt", 49.99987234 * COIN},
    {"FYtDtruz5nKdFNXhtJZg38eXbr8aQhbSvc", 48.99999295 * COIN},
    {"FoHfokjH1yMBgcMy8K8q23yFVqRPfJuUdB", 48.99997310 * COIN},
    {"FrGgUm84QkeyhZD1vbuueevbH3d9JFUacV", 47.79700000 * COIN},
    {"FgGCvbF4UEPpkCZwpPVSVtNPJDQAGqYcpS", 47.74621365 * COIN},
    {"Fm5RRUgZF3Q5JoCek6Sj8FEVcFCQEYmYQT", 46.93156680 * COIN},
    {"FrjRP8D3iX2R9RNxA1mBsY2vyvsgRFw9Sm", 46.39012817 * COIN},
    {"Fpd7Sm8QbCE6iR7qwg5wA6VsYJ1JFo1W3Q", 46.21487297 * COIN},
    {"FfSKHB8eKbsbzZNy4EJwu8Npfo5xKDzRto", 45.56004713 * COIN},
    {"Fivy3VP6cKCXpDGGr81qQcMTcmowK8yLqU", 45.53708400 * COIN},
    {"FghoEav2s3nYSDCHX4QBEUi7bL2fHjdkEK", 45.06070393 * COIN},
    {"FYjQ5nw617LxQsH1x3v4CvPtnaTn12PSiZ", 44.38123509 * COIN},
    {"Fght9jjJqe5M4pc97FKQim1Saw1BsjQG9K", 44.27325145 * COIN},
    {"FqhFtqEpsS2r8F6MMPC7Jzs1tCToKEm8mg", 42.55203742 * COIN},
    {"FmviW4KXXnSJSAoWhBrJUjJozEp7x8TYTu", 39.89498153 * COIN},
    {"FWgcKqwECkGaJXwFMxjLwTehjkdAHCv3Ek", 39.61851186 * COIN},
    {"Fmzgw6EcHdze2ZAC5NghNE1YN4gdKXYfwM", 39.30792298 * COIN},
    {"FXxUtqE97EQM31s8Hf44E6e5rHZt9HHsGv", 38.99998138 * COIN},
    {"FYNt7g9q19gJARZvykmZYYM2UAgbsQ2VXM", 38.73667164 * COIN},
    {"FjMjK68rwzWedPXqHHi21fGjaBxR8C7MU8", 37.25723592 * COIN},
    {"Fg8unaEPyqhDsEwURU7Mq91KSirkpkH3W8", 36.91530791 * COIN},
    {"Fb2ZK9xAn1naPJHwuSSTXHu1MbrLs2QK2k", 35.21215590 * COIN},
    {"FmCKGcroF1UAwfqTGCVZWgJCk7KcvKhXKF", 35.00519633 * COIN},
    {"Ft3PFGwFNDSQbWt1yS1BV1FEjHTGoaALht", 34.52637195 * COIN},
    {"FXNhg9HwRwk5hQF2ZeHw3DxvjpyYt5gASJ", 34.30664126 * COIN},
    {"Frr4WSGp8Dh2baVX9Kwrr8acjGRdfHj7Ep", 34.16442501 * COIN},
    {"Fq9CoSR161ZJ6AriyyHQRfFiibwwcCEjxv", 32.91757579 * COIN},
    {"FaGHdSYAJCAdqppNza1BBon5gTj7xZCqz2", 31.38902442 * COIN},
    {"Fp6nHDwuCGmhg1U5TYUXQd3n4CcwJoGxoZ", 29.88297449 * COIN},
    {"FaJ1nd7btSm6esr6R9JmoqhxX95cKMrrPh", 27.92524230 * COIN},
    {"FfdFWr3JFP9ZkDDNCo1bHubfNLB1nFvgog", 27.66694770 * COIN},
    {"FfaCt15QrQhYiqwVvkKHjgWCn1BC6kc1QU", 27.49029428 * COIN},
    {"Fb4AgTJxAQhs3XJBYRey8ENAjQptLHKNeR", 27.20306151 * COIN},
    {"Fa98zqvUgdpZd8s38Keqc692ENw3t73XLa", 26.95000000 * COIN},
    {"FsYTpq4cBMQJprJVREFRxCRDWNKh9icR59", 26.15431612 * COIN},
    {"FmEmih7pxFUUYPVBWYgxv5KuVZAVW9oegH", 26.10003840 * COIN},
    {"FktfVJcdLGE2G7dnMcFqE4a7mJ7WDEZE5B", 26.10003512 * COIN},
    {"FiSKTdvbhM8HQX1CpTuC2e3q28jLSJhksL", 26.10000000 * COIN},
    {"Fa9aos2LWJj2Wz2fNH3WqFbZsL8jgMLUxz", 26.06894531 * COIN},
    {"FpVxYHrsKNhkeN6LVRfj4vrqJHTts7Bvu8", 26.05000000 * COIN},
    {"FkYKfBsNturcYbPM77v5NPTwEmoH8Zogwc", 26.05000000 * COIN},
    {"FknfYXfyahiXvpNPEG7r5UJaUi4FMczU9W", 26.05000000 * COIN},
    {"FsnpEBXzjhnegHmLHDFPjvM9hftTXKAx2B", 26.00000561 * COIN},
    {"FYbpPpgoUyTKaFKCikQXsPqNPaT6f4pje5", 26.00000000 * COIN},
    {"FbGpSwCYgJSMdpHnGsVRBjmf6pRHUK7Z7Q", 26.00000000 * COIN},
    {"Fgs1HWwE5fQM861k9cNqmqwKvrpVPkDtZn", 26.00000000 * COIN},
    {"FqXH5ChWgJHZZkWvx2PdiaMwQEq9iKHN8j", 26.00000000 * COIN},
    {"FWN3gDZSgtQ8xuFrAUvzx6Urz3jpoJZruj", 26.00000000 * COIN},
    {"FkukJdm65sYc6wbpoa1VW3khxXwdQ25LTA", 26.00000000 * COIN},
    {"FhffENYJuiqSVHbsUTeC2u9LTkwxLsDRyo", 26.00000000 * COIN},
    {"FsExPKsXCi9XMHpQenwgaXi2g1AojKPcjh", 26.00000000 * COIN},
    {"FqzdnY28Q12P4aE7YGH2E6ebhHPRUiRy84", 26.00000000 * COIN},
    {"Fcxj1Sj9MrgwsZyZx5zUpJ7Ec5LBox5MAT", 26.00000000 * COIN},
    {"FszMmEPVcdZU3LBm8ShuAhUHSuTMETKCZo", 26.00000000 * COIN},
    {"FYyHJNgZLV1zPSrRtS77JsTjtWXtERAAHz", 26.00000000 * COIN},
    {"FZWMTSedQm4F2AJVPbjdzgqM2e4E1cz2eT", 26.00000000 * COIN},
    {"FVojPfdnQwfwwokXoMTTZE4RJuoCYgZ72w", 25.95003626 * COIN},
    {"FbbZBF5pXuw3gnR5uHcBgPqCxiyxDYD5LC", 25.95000457 * COIN},
    {"FfHbQUaD4ypUQjToBnAtBQAmrzunux3F8W", 25.95000281 * COIN},
    {"Fr48wahNMG2fHjGNUHzrU6Ux6s4A3gKbBz", 25.95000194 * COIN},
    {"FhZnqR3wsGvF3th52gqHS1a5WFGvRUGzKL", 25.95000000 * COIN},
    {"Fmts9BVX9K4YjnbvL9Kxqm1aGeTZFAUhSe", 25.95000000 * COIN},
    {"Fk4oh5cBF4adhnn8xyoyDPnTEXRWd8D5xk", 25.95000000 * COIN},
    {"FbwpMd5eDEmncZKn2FZEyj7UP3ULbuhnB9", 25.95000000 * COIN},
    {"FdMBFE5nDMh8qJtPwBgb9mEcLKXWcdr6b9", 25.95000000 * COIN},
    {"FkKtVf81JpxN878Fovh5Ji56dy7o2FtEmD", 25.95000000 * COIN},
    {"FtDWWZdxqywZsJMWjd9i8tdTvz1S58ZZdJ", 25.95000000 * COIN},
    {"FpzLPHnB5ToqFX7Pp3oJ3KTVDALxpjmrHb", 25.95000000 * COIN},
    {"Fgh7JXqk4M8gcHWWhWCVNmAekkcrhMMdtb", 25.95000000 * COIN},
    {"FbHT2xyBMRkV4hrszjNfw8GcFkwkkDgG2t", 25.95000000 * COIN},
    {"FrhsuCqsrxjKPUgihWKgDBFURZL7iuiUhA", 25.95000000 * COIN},
    {"FZXz43DZ7gXeRhQWZbrro8LDZLyfPtQJXg", 25.95000000 * COIN},
    {"FfiXXkPzXLu45upq2QFnnnFSZW36vgQ2Bn", 25.95000000 * COIN},
    {"FqjzZHwaVuMD5Ywg1cpE88Dvp9dVUmyVH8", 25.95000000 * COIN},
    {"FZ2uC473EBKWw9JwHsg6k6CKAUBtzA3kwD", 25.95000000 * COIN},
    {"FnuBAKViWBgJweiy8Nr5kVB5LuQMAM9fCp", 25.95000000 * COIN},
    {"Fa9mdw7eHBZwhZ6pgMFvq1rCotn9qAjrYx", 25.95000000 * COIN},
    {"FYjkoxkLmfZKX4jQNnB8KDD3rNuLkW4mZT", 25.95000000 * COIN},
    {"FknCNyzi1vkAKUKTg8FvTncoDxmvFgDTJN", 25.95000000 * COIN},
    {"FYCycab5mHW2KAEvy3smt4nnB9KZWA5Dov", 25.95000000 * COIN},
    {"FsKcJHcc9VUJvVBhmhtb8dchucsJ5xscWR", 25.95000000 * COIN},
    {"FjVTuYYBzDLeWuvmPe4vzHrTEihEMHMNRM", 25.95000000 * COIN},
    {"FpVWLm7yZ8yRVP19S8YYeVhTMGkBLMdP4J", 25.05000000 * COIN},
    {"FdkapFiDGWrNzLFVk4xxSHbfMJfQ48xhpB", 25.00001555 * COIN},
    {"FZjiZ6ZWnCQFhBTZB8DgYVugnkNKHggegn", 25.00000887 * COIN},
    {"FZMDpAveYcBcCS1LPuwFQn2fRDvKoDmqs4", 25.00000545 * COIN},
    {"FkJ2BGMEAxYm6KGb3yy3yDyu7VgFRzg4tA", 25.00000011 * COIN},
    {"Fdt7VvHf797mRc47Vaie18z4BmiqafJrdW", 25.00000000 * COIN},
    {"FkFWu7CPnKsUX4cN1jJ61K44B13UKjRDRi", 25.00000000 * COIN},
    {"FWGfri8HD9WukhSADiVEcSm6Lc6sZiJuFj", 25.00000000 * COIN},
    {"FpqJyHrfkXta92d7gxX6oYXoSZ2zyDTrgJ", 25.00000000 * COIN},
    {"FZCnwayXdmrRJ7h5rWRt2EcyuuBBx6Ypex", 25.00000000 * COIN},
    {"Fb3a3UpYXtku6oMt1pH4brdMyLjiBm1qCV", 25.00000000 * COIN},
    {"FdjfRzeryUU2q6ijGAjKM52ChXHEbHnoTX", 25.00000000 * COIN},
    {"FqLoxopaZHiFjqo1fbpfmo8FzvyV4idYFz", 25.00000000 * COIN},
    {"FtWB4a2cFuViMpqJLw9uH5Y3AC714dD3ca", 25.00000000 * COIN},
    {"Fk3unM5t9wmXPXQbTnntM3TwpmQx6qdJEa", 25.00000000 * COIN},
    {"FjSLZTntdhRFA7KjR57dakvzN6hEN2MAHZ", 25.00000000 * COIN},
    {"FbCQaDDY1KLVxhvFz1shYgtaTemrPrRPE4", 25.00000000 * COIN},
    {"Feg93NQ8rQf33PmGusb8HNs7S5g9vWTu9v", 24.99999677 * COIN},
    {"Fq1U2QfvhHDXYq7Lgsq9w9GzqmtpT4UH85", 24.99907241 * COIN},
    {"FeSv7SEWfHEC1BFdJ1m7KPPoiM38TdSLnb", 24.99846291 * COIN},
    {"FpnP6fvpZJVvV2SU9tAswcoqJnsrmnG92Q", 24.99670671 * COIN},
    {"FpfBzZZdD7K3hTAEgi7qs8v7kUXddR1WAe", 24.98940065 * COIN},
    {"Fk9E1GJ5JTtHMsjyiniFRF9ic89zKG6Dby", 24.98353678 * COIN},
    {"FeGCU8oKMNbq1LJwoeD2FaHbWhnkBnY4hT", 24.95393678 * COIN},
    {"Fnu5drCGJ1yRtJSXTHZ358Zv9Uey6cEgGP", 24.93402236 * COIN},
    {"Fqpj57PP4fNXCkYbwnBFuKnNppoq127SAW", 23.99990236 * COIN},
    {"FiLc4aUxcwcG73EJYWKeiCLFZv5EcQWBrt", 23.82150000 * COIN},
    {"FiwXH5gNBT4nXLaUKSvde2q4RsUGs5zPJE", 23.19059037 * COIN},
    {"FoL5VubVnqNZbkYxoehy19JQBXG6q2sXLV", 22.48961769 * COIN},
    {"FkXo1YqjZFje28nY4rfrJRYFaLEucbgHRz", 22.30942688 * COIN},
    {"FVLTkKN8ihpapR526V19wJqauudT2R72zV", 22.04476836 * COIN},
    {"FoYhpxckzYX7i44U25rWxs31Uzv7LdNHi8", 21.84551545 * COIN},
    {"FfL5gYTH7z1vTLJtgCxW1B1BGNjvv8pxqe", 21.33807720 * COIN},
    {"FeekQo9SzMz4hjjsWsu2jNmghRcxfYqQqa", 21.18046068 * COIN},
    {"FVxWhWugq6fanmJyyyPVS92XggvhwnR7GP", 20.99999521 * COIN},
    {"Fp7bhgwgvxervst3da1aQwiXMU3G3MiYjz", 20.99999521 * COIN},
    {"Fmz3PertrVMF8TNjU2j4SxsePkBKQtKPE7", 20.81591654 * COIN},
    {"FdhbrsufpcL5nrZAr19tiXFR44mkoueF4i", 19.65071582 * COIN},
    {"FgbtFkmS5vBhqurMkCmYMmva1kjF8gSFGo", 19.06712247 * COIN},
    {"FoWxERSSxyw4gB7neqBkDeteyVV5vnWXob", 18.78742576 * COIN},
    {"FfAZAzsZEiNNSQLMosN3Jc6J5grC86ihrd", 18.12610000 * COIN},
    {"FdSGEtP1VEVquk1GypWyyjtYaaeaf6jKqG", 18.00000000 * COIN},
    {"FnwBSH2KNDDewsATpb28aLyZ8fimGox44h", 17.50940027 * COIN},
    {"FrdGutAgByMVhGsC3bSqKUJQ4C1AWWpyJ8", 16.69844560 * COIN},
    {"FZk9VHP4Y9H81jpgYUPQetVPvQrvfwAcYj", 16.44570493 * COIN},
    {"FpAFsEc6Tu9NtH1v1xwnV6xFULgRXNztoK", 16.11187316 * COIN},
    {"FeYhFqC2i5EoskXMsRE8Dzx5MK8bAqUdMQ", 15.66825598 * COIN},
    {"FsXSzCUmbrxTuMvX8tTFrYymkpgXz3TPPP", 15.50250470 * COIN},
    {"FkpHjpafCBw4Fquxx2ZBpsixYtAHwCpEk2", 15.34846168 * COIN},
    {"FtHSGumUNAnRoEpkJDHbvcD5xXNnQBwP9h", 14.80169351 * COIN},
    {"FpAT3QphDt6msSmzVJvmpzGx4kaXbwyt3n", 14.71956688 * COIN},
    {"FbNjsgFNLv4p7F2BLx5SZ4R2SWP6VZyzvJ", 14.57594075 * COIN},
    {"FiuHSMVg4A217T1YfzGQfdAHC21Xs3zTZC", 14.10164568 * COIN},
    {"FVUVDggagqoB8R8KNGtCXD1woLCxdSnWKe", 14.06134041 * COIN},
    {"Fp5LiUcx9UtnvWNhofBPGk8xkbG2gw1hTk", 13.97767264 * COIN},
    {"FWizXzZHa8Bsthmc3AdaDN4hW5gTuPv9Ei", 13.53909891 * COIN},
    {"FZhfEBaHyGk1RYJns599omGAZKY8VUpA1x", 13.33085214 * COIN},
    {"FdHwG2vfmzyCHrPVJ6SeLJyzvHaGwbXmpK", 13.12962065 * COIN},
    {"Frhkhac6K45udk2rpEDbZeYXU4QZLFzthj", 13.08812425 * COIN},
    {"FjzSZocBCveEV83GNpS48TRxCBqwJpqQBr", 12.74529605 * COIN},
    {"FWWy6bcASem3sBYSaeUmW8rqHp6QZ1H9um", 12.60548203 * COIN},
    {"Fg3epUCLhgQA1szd9VvmtUGnp7ZXfvT87N", 12.52342072 * COIN},
    {"FbhXkcRFbubkgkVFoyXWreEmyCrGVkHZ4p", 12.45391114 * COIN},
    {"FrW7YLhJVVdr5CeL5vmV9PmBNN5xrZ97vK", 12.03724977 * COIN},
    {"FohR5L9EiYnYXz3TBS33MwBp7SEKDfNA2A", 11.89414196 * COIN},
    {"FYB6H3R4GqiKvzjkCPdFWkKxHCL5ZX4M6p", 11.86903236 * COIN},
    {"FdkoqWvMdp668KP8AtJXmPZ9FZMcKAvsH6", 11.74227863 * COIN},
    {"FW7Dr73mst4jhuTNqKY9EsCbm73ZDqUXYR", 11.48630833 * COIN},
    {"FfPvQNMFYGkPFGy68qRYVkxybfarpsDQ65", 11.46145995 * COIN},
    {"Fjc8kkxLwFz4S3e5jB9M1eFXhjyNgEAyeS", 11.29107833 * COIN},
    {"FbuhTw16YwzrCWc7Se9TYps8nVqa3mUsFi", 11.20616289 * COIN},
    {"FXskqUgD2Z74Sf5KjwExZF6ABkx4yvsBSp", 11.16784496 * COIN},
    {"FpA6zHDoavSqXhGgAdSWLwRopcSHbRY4h7", 11.11659870 * COIN},
    {"FrMj7dWNRSKHhpccDLDkvHMBE5CC512bQn", 10.99999521 * COIN},
    {"FX8cnLWsjpXL49CqX1miGeZwf5kpGqzACg", 10.97000000 * COIN},
    {"FrdT1VShuMfBeHBcQbbQNjH258W9214MGj", 10.90320000 * COIN},
    {"FedGoXv7H1pnTB8MyCSn4nANztizuesirj", 10.86917243 * COIN},
    {"FjbPbVYShyv9fziEMB92houNi4F2atiRiY", 10.65602504 * COIN},
    {"Fp2W8bqjv5aaJGKuQVtNdCvCPik71fD9Ym", 10.63738967 * COIN},
    {"Fft3UVnuEDfcK1DdjCAVt2q6C1cQ5g1yed", 10.58754532 * COIN},
    {"FspFoj3xMYWLRiJc8WCfCYx27kn9MBymG6", 10.54021851 * COIN},
    {"Fi8FbsP8LDecWQqu1JqoNHYu9KftsiSnNC", 10.46470000 * COIN},
    {"FY5GcJV6WaDFX79iYWUyris88hyJv4MofF", 10.46336765 * COIN},
    {"FVgUX3rrpyK3kNhBkiyDFiLvb3s4EQj44m", 10.26843517 * COIN},
    {"Fg2bNBfCmrSVkV3fHTrDJ8kAdY8LnfhVYJ", 10.18219749 * COIN},
    {"FmEDxbm8ecmPio5eCKHojk6hqd1tLms4FB", 10.05491545 * COIN},
    {"Fdm8ZDEH3YjMqp8VryN4tKvTci9MXdvWjZ", 10.02480324 * COIN},
    {"FbH34v6vvb2QYWFwb9geNYsK45o4Yan7qR", 10.02331791 * COIN},
    {"Fj5KnGXdgPuH7RL27rMeYaWVYRDSVRr4p6", 10.01955906 * COIN},
    {"FZ1CYTUjSBCpaaDvmVCTMa54eXjFSWWcAH", 10.01868817 * COIN},
    {"FcmjGFJF1fLb3ihnMzBaYjKrYwus6mWLfo", 10.00000000 * COIN},
    {"Fruj3eqErLFzFQYK9x31PwFnGKJXHJ9Zzj", 10.00000000 * COIN},
    {"FruKwKj7D6YFBxWZKZjYyPdcMxUGcDVTvj", 9.99997658 * COIN},
    {"FpTaw8AQdZg7kY2xsaEZNkWcMrUXJc9zFw", 9.36002795 * COIN},
    {"FZYWdP6UfZraG2GVQhCwDfFr4J97mgdr8Y", 8.61033490 * COIN},
    {"FZTnYxT4twdbYi5HMFzz15mRPY58jJUdGw", 8.31022680 * COIN},
    {"FrU4FHcZgMYAfmojWytYk6zDB5CksLYJ2Q", 7.69505542 * COIN},
    {"FXefjsjWJkG4THAWybj3WEUCkBwCUsYR7E", 7.43832460 * COIN},
    {"FkUFhXfkqKtqcWLJjkmW9mvaUaEQstApex", 7.14373773 * COIN},
    {"FfKC6bTNT6q3PvacNRLoRzAzeB2jacQCVU", 7.07669290 * COIN},
    {"FdxuBMdYoEJQe8YwD2Vp5GUNn9rdJfw5S7", 7.06470535 * COIN},
    {"FiFK47H8BjKpjTbvykcnbaDYJZ3zPrScWV", 6.62152651 * COIN},
    {"FkXh4PkAaFhtaXRKXHD4ELRoZpUWnGC88x", 6.08846001 * COIN},
    {"Fi8Lu5eXJ9PsCYzgto61bi6H9yfV2F3AQE", 6.05007461 * COIN},
    {"FfZ2TRNwmdCG6BHnQKk3aHjEQEZianDoNj", 5.79981026 * COIN},
    {"FhwynS7UD3Z9dePEfi7JeHpedevpza4EQn", 5.19300772 * COIN},
    {"FWjDxGGGuA8ToiFf11qT94DL4gVR8E1Fcp", 5.18004157 * COIN},
    {"FVJZVTHGQAbao3dKUu9vF1hGmeSJREdt6o", 5.05970000 * COIN},
    {"FpW5im781V72eA76fBNUNnLB9YMkeBFNcg", 5.00000000 * COIN},
    {"FW8jyL1rtmvPV3VAATAoSLhWfAAs4dGewv", 5.00000000 * COIN},
    {"FgT3LhprkS6PadTLfJFVcAMWDnSQGpjhtj", 5.00000000 * COIN},
    {"Fg5xTQBmyEfMD6BhHoKpsQem5uw8UD15Pd", 4.97859821 * COIN},
    {"FWPnHC5QMZx976NNvV6TFE2jkADyVhtVKV", 4.27351785 * COIN},
    {"Fokbobhgsd6jJoMC4ndTcs2xuTMQdFMvCj", 3.91120000 * COIN},
    {"FfPxjS2RuodJLGQBAjjcMUxPQiRiVPReML", 3.82790000 * COIN},
    {"FeinDdxE86wEk3VmVr87AiJpBaYEJKnuuj", 3.75000282 * COIN},
    {"FoJvJz6hdd6MLocod8KUDoSLC6uTK1kPuq", 3.75000195 * COIN},
    {"FjTZqnbxgs6NLBNGFrXDcpD21v5dLYSKar", 3.75000170 * COIN},
    {"FgSGdvUd8b4mPYS65BdHbPpJRShzVUZKPY", 3.75000000 * COIN},
    {"Fm8NjsUxD41BUSaYcMzZ2Hf6MVgP3dXVmV", 3.75000000 * COIN},
    {"FXdueJjzMuowDFitBZ6Zk54gqRgugE3yKb", 3.73389762 * COIN},
    {"FbtWonkv8XMLs7qwJkQEX5WPk7PC78s9aE", 3.55627660 * COIN},
    {"FkoG7DQcYwVesVJtQ27WosV86kRNX3cQBs", 3.43062519 * COIN},
    {"FeLQDcxLQUZ4pHoNNuaBAzRerybgvh2RUn", 3.17298246 * COIN},
    {"FVdqxj3Y2udVAaV93VzKpRUP5bFoNkVEg8", 3.05594254 * COIN},
    {"FmB8dTTvFrofFjsjETtNgEohiEBFXvayFw", 3.00000000 * COIN},
    {"Foptqeqs8cH4vLdviWoF4TKnnVzMc386ed", 2.94164377 * COIN},
    {"FdCy5HafBCP4MhkrgcrF2XF2yHhVJrHdL7", 2.92830000 * COIN},
    {"FrQ8L4VZvwSTv79yCvNgVXryn2cuePkoq8", 2.72324696 * COIN},
    {"Fq2i7yMXpQdUUrvWHt4h5Zw5Cbp5ikXAJ8", 2.51540000 * COIN},
    {"FoULx2XUQ7mDHGZUhE957nJcVUm1x7dDvs", 2.44604159 * COIN},
    {"Fnu9RTcsqiZ9fHYJV7DReNLcR8ouwy7qAu", 2.40063779 * COIN},
    {"FkPNgFX6RqzChwXZqLvWcrdTriUPikEpVW", 2.24999626 * COIN},
    {"FoqLjfbwrnUSaquF8VNnNCxRnLYkLcCgXk", 2.19443623 * COIN},
    {"Fer2mQ63i6chVy7YUuTWM6snGtgB6p5ybk", 2.13550000 * COIN},
    {"FYzjzLAh5S7ZHRzTcEAcSfnFAypmUUeW1f", 2.13065007 * COIN},
    {"FoqmF547ERAW6nZf4NPpL3uim7EdaweMor", 2.06135061 * COIN},
    {"FZz2fn2VHxMF3oq8oKrz8KjJyWEiknrHCF", 2.00000000 * COIN},
    {"FZEXn4DZHUx7QKwmR274PSk1UqBY45a8Pg", 1.99999521 * COIN},
    {"FkzkMraJnuYQBMAmWNLuqqCowRTMxf89Mw", 1.99995039 * COIN},
    {"Ft7gAbmRmVKkTTmZAJCqvc7bmw5hoEcQQv", 1.99972982 * COIN},
    {"FhZ9E1Ni17S6Z9wrDuD4zAFRJeNCicU3Sw", 1.87503923 * COIN},
    {"FXJuGiRCWmUaoUzXiWjHaTkN95CMS94Kyw", 1.87500853 * COIN},
    {"FfXpSaARpEovCPu8yNw5W5CcYkh3bi6KV6", 1.87500000 * COIN},
    {"Fb7KQZ9TYbCL2C736vEvHWSZ1e321bqqG5", 1.87500000 * COIN},
    {"Ff51RLBRMRxNMvdCH1VAEsRRz7jRMAvYPz", 1.87500000 * COIN},
    {"FrSzZozT15fHcHWteycRS2UiCjERs8QExE", 1.87500000 * COIN},
    {"FfsMKoCUT9jtV8VivyoVcR5onRUmD9jqTv", 1.87500000 * COIN},
    {"FWFt2CZS5V6VfjppGnAy2ZqN99oSCmgCP2", 1.87500000 * COIN},
    {"FtThbTeBdbj3zCSo8yK4HChFSaanKw3Fog", 1.87500000 * COIN},
    {"FeMpxET1sZLczHAupDSbyMsW83QVKfFY9u", 1.87500000 * COIN},
    {"FdkUgALzwoeYvb6UDrXTiQ12fn5x7zj7aK", 1.87500000 * COIN},
    {"Fo48q8R3boLLtjXzHrhuahcKN85iuYeCbH", 1.87500000 * COIN},
    {"FqhGsLQkqSWmEeCVizgfjhAgysuU5qWdh2", 1.87497296 * COIN},
    {"FsRMkRfezFTd1j22Ct8fd4UzvX5xTJpoMe", 1.80006712 * COIN},
    {"FnjTbPuM19bTm2AS6bAALBJZg1r9nxLccH", 1.78991171 * COIN},
    {"FePMoTeVJns6izSt8jK6vsfG29X8KVnUBA", 1.77870000 * COIN},
    {"FeZK6m6wJhm9md4qPJFiJ2heDZJK4EgnDr", 1.76805248 * COIN},
    {"FWJESDNRd6fGZRZ2pCWXdQXX838VkNzRNm", 1.69999774 * COIN},
    {"FfxCB4of2wkyowygz18jLp4J7MWQaL9Ar6", 1.69999774 * COIN},
    {"FaJDzkgdfHGkkUXenTvUaYEVyRyMkCwVXh", 1.69999774 * COIN},
    {"FZz2NsueKMVZcjsAsYUBs7CauJHXoMiTJK", 1.65771302 * COIN},
    {"Fgysq39QtSGgZ3q2bxe1QdWRGv6pFSuzn1", 1.57917087 * COIN},
    {"FbS7jEEuUxmHgGbEmefqsn2MTrN4Xdx7Ny", 1.12499774 * COIN},
    {"FZGo8vxYxBB4qkowtN49oVtzrb8NHBfsqt", 1.12499756 * COIN},
    {"FsYQv1Crv2Sjv2rjCuWyfWorG2fRQKzpqB", 1.12498828 * COIN},
    {"FYgGN81q2cpB3QgRHtx5mmhCE5NvEGzNiJ", 1.05800754 * COIN},
    {"Fh9gdC2GpCgHHhNVNyxLhT3RjdForPB7GB", 1.05000000 * COIN},
    {"FjNcjwLBcDtdyveBMBdjYGUFfXje5jWDkh", 1.05000000 * COIN},
    {"FdMNbQJr7Xu2WnCZEtBckEoDNz5WSqdY1c", 1.05000000 * COIN},
    {"FctpPbrhnsgRBd5F7QwJoPNM9J7HvL4kt5", 1.03540000 * COIN},
    {"FbrfbP75XQjCEQdWy9qPg6Ky6DsuKmcSn4", 1.00154185 * COIN},
    {"FVcA1T5wqpVnDbMMVmU1ngyUSDUHDSwmRy", 1.00000001 * COIN},
    {"FXwALS64A5nA5QHQPdh6zEVnBwNTVue5cE", 1.00000001 * COIN},
    {"FbMFLjR7sUuDbVqDnjDCSU3nxabdvDmKP3", 1.00000001 * COIN},
    {"FbY7oYcXEfxA9p1tVRao5VGZFixVA8LYPq", 1.00000001 * COIN},
    {"Fdh2fqKRWG3jgvD7VjcaJYffbJKU2b7kMp", 1.00000001 * COIN},
    {"Fe5pjuVAqYBCH5xuqadx9629MSSmtoH18S", 1.00000001 * COIN},
    {"FesR6TPCo7TvTendvAPoFYgWyJLzumz7u2", 1.00000001 * COIN},
    {"FiX5vNzm3UvCZeRMT4NkET1D5ChdFEQBCG", 1.00000001 * COIN},
    {"FkL8q26n26BqWo4sxhPamCk4yZTbwu5e1L", 1.00000001 * COIN},
    {"FocYiSB8GqgTppSB3gbkCKSsCbAsBS8HgL", 1.00000001 * COIN},
    {"Fp5DG5ZDGHR43aw8oHB5S3GhTzKvT5UGv1", 1.00000001 * COIN},
    {"FgeDNZnqV5yLcoQH5zhMuB7nSXvxgWuYRk", 1.00000000 * COIN},
    {"FhBELkngfLWwuBXW8K3c6X3RvPgThLyumP", 1.00000000 * COIN},
    {"FnaXr9v8UM2BUZ3k6epGkoUqrY3WqdgawL", 1.00000000 * COIN},
    {"FbrgjEXontVVfpPk5AbPjvorBxbuXd8c6n", 0.99999774 * COIN},
    {"FdGhjxkNt4mgnJ7YPXvbz6p49BqHZjRavi", 0.99999774 * COIN},
    {"FkbHkvxy7nZtbRFY4JMNeKeuphEXrSUc8s", 0.99999521 * COIN},
    {"Frd2oLDp2rGoyyL5Yi4tFZ5tPi6oGpJhzL", 0.99999147 * COIN},
    {"FVwatakS3E9Ez6AFgTcZKQ4rhw2UffNexF", 0.99997527 * COIN},
    {"Fgwc1ruy7xdbMBP1vNJ9tHyMfqF1XSV4EQ", 0.99984533 * COIN},
    {"Fpvj9EdgzTbqVYYpSedoqiztjpSjuYYqRV", 0.99964797 * COIN},
    {"Fgeksxo34CGpmwPLCR8vSr2CzTEqaSJbpG", 0.95000000 * COIN},
    {"FpzfuCGuNzsfLz9FR1XqUtfgRduVs35Yi9", 0.95000000 * COIN},
    {"Fcoe35oDnZKp2pgn1iyLa8EAfEKjvFiWt2", 0.95000000 * COIN},
    {"Fkd7LEqxLPp2bTDJESSzbkNCTypz9jiWkT", 0.95000000 * COIN},
    {"FsrWyAEhet6E2seKESH95KQw8oxB5AJ9FN", 0.95000000 * COIN},
    {"FigRo1XhbDkjub2oUbVgPGDEH7nr4LCMrr", 0.95000000 * COIN},
    {"FYe9ygFfrcUBtuWmaCL13PsQmfKXsWcbaG", 0.95000000 * COIN},
    {"FVqrPXZf7qQbPKFEL1HdzCYidAKyHARxtU", 0.95000000 * COIN},
    {"FeuhWtMX3YFSHUkHLJmxUBAWhjMuDYaS5D", 0.95000000 * COIN},
    {"FfWzvuCi7iczShKr5TJtscHAJpdgmWUJjF", 0.95000000 * COIN},
    {"FqYtF2j3Cv7XFBpsDkkXCWvdajujXt2USM", 0.95000000 * COIN},
    {"FXKf45ZFNKNByXNT6DUiyeQqDCvBXza7vL", 0.92330000 * COIN},
    {"FWweHYsKjAuwD3ibR2wyfwZ22xZdgr8GED", 0.91831046 * COIN},
    {"FaxgY3KAk7VJNzV8oSC7sWZ5Bptad2nrru", 0.90848572 * COIN},
    {"Fbr1Q7Cup1LbmK1yLRTnzQvKSRpXFi6KV3", 0.90675405 * COIN},
    {"FsFFnwLLvX6dvvZsFRWk5BeePb83qQhvvv", 0.85065269 * COIN},
    {"ForYQNFU1iPcRVrm9C9B6NbxL33HBTineZ", 0.82105300 * COIN},
    {"FWRRv2QDcRH5r6RMMyHaHTBQtEDkR7w73S", 0.75693000 * COIN},
    {"FWSP9jbLLvLsH4BKadYKqNZHzzYRPfpMrw", 0.75004891 * COIN},
    {"Fensw9SADVKr5TUwGGzXcEUkGvWZmk9DuU", 0.61596308 * COIN},
    {"FXogV3S7oTrMTMcp8eBJ5gzLmRfjNEn8oP", 0.61210569 * COIN},
    {"Fh4f6yNbPZNUkVKkTtB27UGLk1U4Z2oYbd", 0.57013700 * COIN},
    {"FabR4vUfsc1fF97DHG8HitCN7orhAujxEi", 0.53850000 * COIN},
    {"FghuEbQYV2hoAqjt8WWuWDWNFaB8Utsyeg", 0.50062225 * COIN},
    {"FXvPv4z8gMast6SE2bHfn5wxc1QwgByi9a", 0.45100000 * COIN},
    {"Fc93cmUyjnWeE2pDw8BnqGUKWWNJiDtuj2", 0.43994243 * COIN},
    {"FaMV1pHNYMaFLgVqYMh3D3ihgV7NHqcq4b", 0.42826955 * COIN},
    {"FVdzLdWnw6ragQnKGdyFCqx6q9XMubKH3v", 0.40358311 * COIN},
    {"FiVfkEKbsFcZBATK6XLZ64KSe1QDtTUTLD", 0.40000000 * COIN},
    {"Fm2nALt4tCpGPb1cy89DkUa38kaBPiMASi", 0.40000000 * COIN},
    {"Fii76Q3C666fiErH7Mg1PJPGcJubsVAUCT", 0.40000000 * COIN},
    {"FVXQy6zRugWd4RoCgStXkE4mHov8AioV1c", 0.38461500 * COIN},
    {"FmuRRzFPbaS9mJKjSfva6qjhNPcks1TLYn", 0.38181800 * COIN},
    {"Fb8qejZpoiifckK9XkCWCZVvDXTxfwd17K", 0.34659950 * COIN},
    {"FquzCPg1jF2gA8D5LsNpwRQWLdNC4LhDUX", 0.33380000 * COIN},
    {"FYyxFdEoQmhKwTf4puZEQhX3RnhGgdRaia", 0.30740000 * COIN},
    {"Fpqp9yESGcwE7PwDvBQ2akiVg7dXbvgBr4", 0.29806681 * COIN},
    {"FXDE1JtR8bdFTzuz5Rp7RSKcE7ezDsTsJf", 0.29434474 * COIN},
    {"Fn9exDLJPezrzzsxexkaauonjVeHbDwPdG", 0.28038879 * COIN},
    {"FfqAsV6mSNVx4QEer5ZqVkT19upnmhLgm1", 0.24508008 * COIN},
    {"FWmX8chMuPtjKhYAHgwhsnreLA5kfYkE4C", 0.22845291 * COIN},
    {"FZjdNzAtMxyEntAez2SFTxw3SnHjdkdZsH", 0.19964160 * COIN},
    {"Fcc3J61kRAjVGnTX1xHGqEHSTLpwyNQpMk", 0.18997078 * COIN},
    {"Fj8wKftm2hu25KgiXBxeSWZAznSyFALiKq", 0.18896660 * COIN},
    {"FfpL9xhrMMkBt6VnxbGgNwULYeZTXD9niR", 0.17158342 * COIN},
    {"FevSE1NVWhfhvuPiSeoJobYW1DaHDpJfdt", 0.14365911 * COIN},
    {"FejAdfPDpng7yTDXVwvdWf1VbrELtDgyHL", 0.14027138 * COIN},
    {"FayJL9fpr1ZggoHLM6FgQZLswG4QwpN4gS", 0.13730353 * COIN},
    {"FdweFEjFHEHMvFqipYT9gEBAWHnTkwhjTa", 0.13210000 * COIN},
    {"Fd2qBwYPVjqKCwLjtBCS2f7y4AhVUZr3FG", 0.12405000 * COIN},
    {"FobpEzLzj8SjQ89HViuwzDjhxnw388T9Wx", 0.11602400 * COIN},
    {"Fq9jPB8H4V2jzCwTxzvUC4qxuzaNfNrcCh", 0.10399525 * COIN},
    {"FieWy8TxGNCjoXUokCYxhoZ32tbJ9HMJzy", 0.10254466 * COIN},
    {"FasNYPq4fnQ4TUF1d94nQDchhuYXHMxZPa", 0.10001722 * COIN},
    {"FnwQdgdqXTSAUubwdLo2CoYKDYvBS8u1i7", 0.10000000 * COIN},
    {"FmHCuizPwZuGRR3xS3nPDLjrsjRVwK4hvF", 0.08195497 * COIN},
    {"Fkqud4KgQBv8NJQaaaDw3YUV6MQa2ka8Jp", 0.06207969 * COIN},
    {"Fi9LtSVqJsgGuFJDE7y7QSfDdVC4RtdiUM", 0.05501236 * COIN},
    {"Fj3Dzj1knxmv4wFALkWzdRhG9zTQPdnz5S", 0.05081588 * COIN},
    {"FppmcvFyqSmwtQfhaCRA9vqUoAsTnYqwm8", 0.05000000 * COIN},
    {"FdBoBf2oC9ErTXHWPBwafbbpGUZkDFaNMo", 0.05000000 * COIN},
    {"Fsv8yx1e6dFuqKVxfBPfXouTHJh9F26N5Q", 0.03923045 * COIN},
    {"FZ4WLdaUM4xmi9Jpt1iWrMHhpsMR5pgtPT", 0.03595743 * COIN},
    {"FqHff8PuuTQJhdzDnjM9XSUNVnPqsd8qpp", 0.03470390 * COIN},
    {"FdF5DELEwhyFCviSeHEdxZHJ5UcJi4zJb4", 0.03241459 * COIN},
    {"FYkRKfSk29tyuzpdUBbGeBhKGjmQChdPcj", 0.03130815 * COIN},
    {"FazyYxAcqWmtD6DMy8uq5ACzkVnX3xDDbP", 0.02908070 * COIN},
    {"FWUAeFx3WqRAuReEp6PXdgDEjVPd8wzniw", 0.02665655 * COIN},
    {"FtJh9rGfA2xnWeYdXmY3k1WR9apPvn9sKr", 0.02609104 * COIN},
    {"FmP5VXhg6Pc6HwHyfECn7PF54xiFBFphLP", 0.02346605 * COIN},
    {"FcioJESaXsHSXbbZK5KJdxr1yfE48rghsP", 0.02005530 * COIN},
    {"FmmkAe5cQvJ66RuMzKeo1ZB8kevGvyrQi7", 0.02000000 * COIN},
    {"FXRdNckMLYRZwhreeaQCHrSQ4oEL8sFrCr", 0.01839575 * COIN},
    {"FhR9Hz4x1VHRoMv7XdbmivioXx7F6yvVGb", 0.01829533 * COIN},
    {"FmiZVdc3LoQ2GCbv97uPqStBN4eYKddyRW", 0.01822529 * COIN},
    {"FZmMxw2WriFLYHThACG7EDNnmXwjDqPHJY", 0.01807517 * COIN},
    {"FoFbwwC2jDM2d9vmhBtXmMCBaDbLppoWFj", 0.01791202 * COIN},
    {"Fa7Ni8KuE4NkdmwR4LUqtAMv65TXoqu6nR", 0.01790913 * COIN},
    {"FfUJfhY5unbf4CUob2E2e7Ynj5FGp2MgyQ", 0.01758911 * COIN},
    {"FjEhgjs4kL8v1c1xmtEoGD3VG673jYiabb", 0.01750000 * COIN},
    {"FeynG6q9URQRGeGfMDcwAczgC9VzPqA4vy", 0.01742006 * COIN},
    {"FnjR8brmxj5vyh4MPg8CCXiicx3azzb33U", 0.01600816 * COIN},
    {"Faj397VB53Tms18oJxorcBm9X8NqH5QspX", 0.01578187 * COIN},
    {"FpFSiqQYDxtN4tGjijDDbDvjXyPSPgkAzE", 0.01550953 * COIN},
    {"FgBkUpTHdKk1XpCKr6QTh2ikWyPndkEvbk", 0.01466691 * COIN},
    {"Fac9BhqgF77HZ8ZMQ59jx6a5BNM5afk6nv", 0.01450260 * COIN},
    {"FdiuFBbwhNaqGwo178YBXeULLdxjquvdTw", 0.01392099 * COIN},
    {"FfrYmabaGGbbSvwArgG5ByVyRVrrPiPZQb", 0.01360452 * COIN},
    {"FfgZjVqgFFeRW8KkJJr87sRBbc5HWQwS5Q", 0.01337134 * COIN},
    {"Fh59UnsUvmcCg5CmZ1Z1kYHFG9cA2cyFmw", 0.01291177 * COIN},
    {"FjkL6biXJXbMMcLuZZEXXwnztwqToFGZA3", 0.01242518 * COIN},
    {"FhJCiVtT3SQoPB4qnjLRKPAeyeibNbD5SK", 0.01233895 * COIN},
    {"FacHUyCATRXRWvj2ERmjLnku4zY3NXxqmc", 0.01215634 * COIN},
    {"Feu1xpTo5EdoMX6RcG2NY1n4YHDfuYQRDY", 0.01187356 * COIN},
    {"FZHKoEkNojQuS3mVq4eSBnizfKUi2xGrcs", 0.01147129 * COIN},
    {"FqGWn6JaztFJ9waYwL2FAkJML8bGStqSzz", 0.01134612 * COIN},
    {"Fexiygggx2SuA57YtG33h5U6h2ybM5ApLA", 0.01133469 * COIN},
    {"Fh9JMCFkh5JkRvfYTHyhkPp7D5ozqVq326", 0.01098483 * COIN},
    {"FerTPzmVURsjsr1JdKLkjDn8yQJYed8CL3", 0.01081118 * COIN},
    {"FX792v4Z42MWCvifMvLHa7U1mEPd9oVYTf", 0.01074101 * COIN},
    {"FpVvsEhto7T9URqAz86fyYDyjLExNUcSjd", 0.01019738 * COIN},
    {"ForLgobW7n4jH8jU6TM3sZKYpQ61Xrzi7m", 0.01017785 * COIN},
    {"Fs6qGzxdFrhdzUpyGgApnz4H9AVmwnKD2h", 0.01017445 * COIN},
    {"FmQd7ENKQZHvff93NWCWsbhVMtjX1xmMVZ", 0.01017415 * COIN},
    {"FkKFNWAF1m6DYG5jRRpzCmqUVMLxxyw8G5", 0.01015732 * COIN},
    {"FamZx9HkpRwF1uFRWuKwhW75zqLaioszdu", 0.01012483 * COIN},
    {"Fg9iVgQoAZ9oKvNiuBbZCttcEmHmsT7Pj4", 0.01011788 * COIN},
    {"Fkfqv2zyHLxp51zH4HAKwJg9gcPqUEecNp", 0.01011189 * COIN},
    {"Frvr8XZhJiXej7LWrt1JpTj3YYrx4s2uo8", 0.01001668 * COIN},
    {"FiJueMp7rbUH1HgsBMND6mvC3SvedWFCUx", 0.00998354 * COIN},
    {"FfB8rX1xP2KV2LTMSpzifYHXcJWhojJg8H", 0.00997656 * COIN},
    {"FqTuRknDgpXZvveH2GVvbT5j2TCqrWcEzH", 0.00997536 * COIN},
    {"FnaNGbcyhi8gbbKNAWzCquxV8mUssGSBbg", 0.00994165 * COIN},
    {"Fmdaj7CS1CRohukFnW6sPQ73b2kd8PhZWv", 0.00992395 * COIN},
    {"FjgqpsPC3FvpvbnsQXiVjpjL8w17qSTjuR", 0.00992392 * COIN},
    {"FqM82bwjjbbnpppT6RdvcLb5BqzJajYGbM", 0.00987144 * COIN},
    {"FZMGZ8SYJh9EdhmVqVNchEKSfiCN5RivXc", 0.00975840 * COIN},
    {"Fhb5mTTNVkBhEUBwmgybiFxxGJVR3VBHDh", 0.00969794 * COIN},
    {"FhvKCJquMV9wcUFH79jZU7Aukp4iFeT64M", 0.00952309 * COIN},
    {"FpLYjL83jXLtmBvuYKXU5kzdaH46TWvxa2", 0.00922054 * COIN},
    {"FtRGKE9ZJGM74v8p4y2KcZFk5kmBug1Ko4", 0.00798780 * COIN},
    {"FmYhTRWm4DhtJCHREAM5sytKj5Tqp1skfP", 0.00737783 * COIN},
    {"FsB1vztZb8rVkhCTufmBm9XLu4tRBefucv", 0.00681074 * COIN},
    {"Fcepj314CEfXNMjw3o3ksiS8n7NPnx2b3n", 0.00665251 * COIN},
    {"Fow2UARYSKiRadS5gBvgw8kyvTW1WXjtie", 0.00637415 * COIN},
    {"FeuXwNSyVvihxbzZLAHPSRJXvJRPHdEGcF", 0.00612481 * COIN},
    {"FYAapBFgjv1k8CksoW3wgj3uW3Tx9YSdii", 0.00486351 * COIN},
    {"FXK7okfJRVUmvVjAzZWviHTjnwpdvzDUaH", 0.00375151 * COIN},
    {"FXWqj91mRsJiZ7GdciRhhpjiYrZkdQx5j1", 0.00332660 * COIN},
    {"FrvjLUEVaE4JdcADqY4xr86P34QPcneBRN", 0.00276476 * COIN},
    {"FsExUa5varLPcXDNiVitx57K3NVG81dYKy", 0.00234469 * COIN},
    {"FftK6xcwwHNGC1EYms2zDVGH4HcCGzbvvc", 0.00034470 * COIN},
    {"FsWL1XsRedStQqi96M1jgJgSqjv8syJTFQ", 0.00012120 * COIN},
    {"FZAYMWjmRJEsupQ8MqEGs94n4FtRSZMk6v", 0.00004796 * COIN},
    {"FkXgpHP3Z7o6s4pWJBwYbjG2ZW1B7tEcNB", 0.00003580 * COIN},
    {"FmsbXDtJ5w4w4cw4jLwDV6GJKHHqCyz4u2", 0.00001836 * COIN},
};

std::unique_ptr <CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript &scriptPubKeyIn) {
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if (!pblocktemplate.get())
        return nullptr;
    pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vSpecialTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    LOCK2(cs_main, m_mempool.cs);

    CBlockIndex *pindexPrev = ::ChainActive().Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    bool fDIP0003Active_context = chainparams.GetConsensus().DIP0003Enabled;
    bool fDIP0008Active_context = chainparams.GetConsensus().DIP0008Enabled;

    pblock->nVersion = Updates().ComputeBlockVersion(pindexPrev);

    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);

    pblock->nTime = GetAdjustedTime();
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                      ? nMedianTimePast
                      : pblock->GetBlockTime();

    if (fDIP0003Active_context) {
        for (const Consensus::LLMQParams &params: llmq::CLLMQUtils::GetEnabledQuorumParams(pindexPrev)) {
            CTransactionRef qcTx;
            if (llmq::quorumBlockProcessor->GetMineableCommitmentTx(params, nHeight, qcTx)) {
                pblock->vtx.emplace_back(qcTx);
                pblocktemplate->vTxFees.emplace_back(0);
                pblocktemplate->vSpecialTxFees.emplace_back(0);
                pblocktemplate->vTxSigOps.emplace_back(0);
                nBlockSize += qcTx->GetTotalSize();
                ++nBlockTx;
            }
        }
    }

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated);

    int64_t nTime1 = GetTimeMicros();

    nLastBlockTx = nBlockTx;
    nLastBlockSize = nBlockSize;
    LogPrintf("CreateNewBlock(): total size %u txs: %u fees: %ld specialTxFee: %ld sigops %d\n", nBlockSize, nBlockTx,
              nFees, nSpecialTxFees, nBlockSigOps);

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    
    // NOTE: unlike in bitcoin, we need to pass PREVIOUS block height here
    CAmount normalBlockReward =
        nFees + GetBlockSubsidy(pindexPrev->nBits, pindexPrev->nHeight, Params().GetConsensus());
    CAmount blockRewardWithSpecialtx = normalBlockReward + nSpecialTxFees;


    
    if (nHeight == 1) {
        coinbaseTx.vout.resize(0);
        for (const auto& reward : swapAddresses) {
            CAmount amount = reward.second;

            CTxDestination address = DecodeDestination(reward.first);

            CScript scriptPubKey = GetScriptForDestination(address);
            coinbaseTx.vout.emplace_back(amount, scriptPubKey);
            // nSubsidy -= amount;
        }
    } else {
        coinbaseTx.vout.resize(1);
        coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
        // Compute regular coinbase transaction.
        coinbaseTx.vout[0].nValue = blockRewardWithSpecialtx;
    }
    




    if (!fDIP0003Active_context) {
        coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    } else {
        coinbaseTx.vin[0].scriptSig = CScript() << OP_RETURN;

        coinbaseTx.nVersion = 3;
        coinbaseTx.nType = TRANSACTION_COINBASE;

        CCbTx cbTx;
        cbTx.nVersion = 2;
        cbTx.nHeight = nHeight;

        CValidationState state;
        if (!CalcCbTxMerkleRootMNList(*pblock, pindexPrev, cbTx.merkleRootMNList, state,
                                      ::ChainstateActive().CoinsTip())) {
            if (state.IsInvalid() && state.GetRejectCode() == REJECT_INVALID && state.GetRejectReason() == "bad-protx-hash") {
                LogPrintf("%s: Skipping invalid Masternode transaction due to bad-protx-hash\n", __func__);

                cbTx.merkleRootMNList.SetNull(); //  Clean MerkleRootMNList
            } else {
                throw std::runtime_error(
                    strprintf("%s: CalcCbTxMerkleRootMNList failed: %s", __func__, FormatStateMessage(state)));
            }
        }
        if (fDIP0008Active_context) {
            if (!CalcCbTxMerkleRootQuorums(*pblock, pindexPrev, cbTx.merkleRootQuorums, state)) {
                throw std::runtime_error(
                        strprintf("%s: CalcCbTxMerkleRootQuorums failed: %s", __func__, FormatStateMessage(state)));
            }
        }
        LogPrintf("cbTx.merkleRootQuorums %s\n", cbTx.merkleRootQuorums.GetHex().c_str());
        SetTxPayload(coinbaseTx, cbTx);
    }

    // Update coinbase transaction with additional info about smartnode and governance payments,
    // get some info back to pass to getblocktemplate
    FillBlockPayments(coinbaseTx, nHeight, normalBlockReward, pblocktemplate->voutSmartnodePayments,
                      pblocktemplate->voutSuperblockPayments, nSpecialTxFees);
    FortunePayment fortunePayment = chainparams.GetConsensus().nFortunePayment;
    fortunePayment.FillFortunePayment(coinbaseTx, nHeight, normalBlockReward, pblock->txoutFortune);
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
    pblocktemplate->vTxFees[0] = -nFees;
    pblocktemplate->vSpecialTxFees[0] = -nSpecialTxFees;


    // Fill in header
    pblock->hashPrevBlock = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    pblock->nNonce = 0;
    pblocktemplate->nPrevBits = pindexPrev->nBits;
    pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(*pblock->vtx[0]);

    CValidationState state;
    if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCHMARK,
             "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n",
             0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1),
             0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries &testSet) {
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end();) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        } else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, unsigned int packageSigOps) const {
    if (nBlockSize + packageSize >= nBlockMaxSize)
        return false;
    if (nBlockSigOps + packageSigOps >= MaxBlockSigOps(fDIP0001ActiveAtTip))
        return false;
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - safe TXs in regard to ChainLocks
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries &package) const {
    for (CTxMemPool::txiter it: package) {
        if (!IsFinalTx(it->GetTx(), nHeight, nLockTimeCutoff))
            return false;
        if (!llmq::chainLocksHandler->IsTxSafeForMining(it->GetTx().GetHash())) {
            return false;
        }
    }
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter) {
    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vSpecialTxFees.push_back(iter->GetSpecialTxFee());
    pblocktemplate->vTxSigOps.push_back(iter->GetSigOpCount());
    nBlockSize += iter->GetTxSize();
    ++nBlockTx;
    nBlockSigOps += iter->GetSigOpCount();
    nFees += iter->GetFee();
    nSpecialTxFees += iter->GetSpecialTxFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries &alreadyAdded,
                                           indexed_modified_transaction_set &mapModifiedTx) {
    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it: alreadyAdded) {
        CTxMemPool::setEntries descendants;
        m_mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc: descendants) {
            if (alreadyAdded.count(desc))
                continue;
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCountWithAncestors -= it->GetSigOpCount();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx,
                                    CTxMemPool::setEntries &failedTx) {
    assert(it != m_mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void
BlockAssembler::SortForBlock(const CTxMemPool::setEntries &package, std::vector <CTxMemPool::txiter> &sortedEntries) {
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated) {
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = m_mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != m_mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty()) {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != m_mempool.mapTx.get<ancestor_score>().end() &&
            SkipMapTxEntry(m_mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == m_mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = m_mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                CompareTxMemPoolEntryByAncestorFee()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        unsigned int packageSigOps = iter->GetSigOpCountWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOps = modit->nSigOpCountWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOps)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockSize > nBlockMaxSize - 1000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        m_mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final and safe
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector <CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        for (size_t i = 0; i < sortedEntries.size(); ++i) {
            AddToBlock(sortedEntries[i]);
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void IncrementExtraNonce(CBlock *pblock, const CBlockIndex *pindexPrev, unsigned int &nExtraNonce) {
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight + 1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

static bool ProcessBlockFound(const CBlock *pblock, const CChainParams &chainparams, uint256 &hash, NodeContext &node) {
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0]->vout[0].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != ::ChainActive().Tip()->GetBlockHash())
            return error("ProcessBlockFound -- generated block is stale");
    }

    // Inform about the new block
    GetMainSignals().BlockFound(hash);

    // Process this block the same as if we had received it from another node
    //CValidationState state;
    //std::shared_ptr<CBlock> shared_pblock = std::make_shared<CBlock>(pblock);
    ChainstateManager &chainman = EnsureChainman(node);
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
    if (!chainman.ProcessNewBlock(chainparams, shared_pblock, true, nullptr))
        return error("ProcessBlockFound -- ProcessNewBlock() failed, block not accepted");

    return true;
}

void static FortuneblockMiner(const CChainParams& chainparams, NodeContext& node) {
    LogPrintf("FortuneblockMiner -- started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    util::ThreadRename("fortuneblock-miner");

    unsigned int nExtraNonce = 0;

        CWallet * pWallet = NULL;

    #ifdef ENABLE_WALLET
        pWallet = GetFirstWallet();

  		  // TODO: either add this function back in, or update this for more appropriate wallet functionality
        // if (!EnsureWalletIsAvailable(pWallet, false)) {
        //     LogPrintf("FortuneblockMiner -- Wallet not available\n");
        // }
    #endif

    if (pWallet == NULL)
    {
        LogPrintf("pWallet is NULL\n");
        return;
    }


    std::shared_ptr<CReserveScript> coinbaseScript;
    pWallet->GetScriptForMining(coinbaseScript);

    if (!coinbaseScript)
        LogPrintf("coinbaseScript is NULL\n");

    if (coinbaseScript->reserveScript.empty())
        LogPrintf("coinbaseScript is empty\n");

    try {
        // Throw an error if no script was provided.  This can happen
        // due to some internal error but also if the keypool is empty.
        // In the latter case, already the pointer is NULL.

        if (!coinbaseScript || coinbaseScript->reserveScript.empty()) {
            throw std::runtime_error("No coinbase script available (mining requires a wallet)");
        }


        while (true) {
            if (chainparams.MiningRequiresPeers()) {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                do {
                    // NodeContext& node = EnsureNodeContext(request.context);
                    // if (node.connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0)
                    if ((node.connman->GetNodeCount(CConnman::CONNECTIONS_ALL) > 0) &&
                        !::ChainstateActive().IsInitialBlockDownload()) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                } while (true);
            }


            //
            // Create new block
            //
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            CBlockIndex *pindexPrev = ::ChainActive().Tip();
            if (!pindexPrev) break;
            std::unique_ptr <CBlockTemplate> pblocktemplate(
                    BlockAssembler(mempool, Params()).CreateNewBlock(coinbaseScript->reserveScript));

            if (!pblocktemplate.get()) {
                LogPrintf(
                        "FortuneblockMiner -- Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                return;
            }
            CBlock *pblock = &pblocktemplate->block;
            HashSelection hashSelection(pblock->hashPrevBlock, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
                                        {0, 1, 2, 3, 4, 5});
            alsoHashString.clear();
            alsoHashString.append(hashSelection.getHashSelectionString());
            LogPrintf("Algos: %s\n", hashSelection.getHashSelectionString());
            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            LogPrintf("FortuneblockMiner -- Running miner with %u transactions in block (%u bytes)\n", pblock->vtx.size(),
                      ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

            //
            // Search
            //
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);
            while (true) {
                uint256 hash;
                while (true) {
                    hash = pblock->ComputeHash();
                    if (UintToArith256(hash) <= hashTarget) {
                        // Found a solution
                        SetThreadPriority(THREAD_PRIORITY_NORMAL);
                        LogPrintf("FortuneblockMiner:\n  proof-of-work found\n  hash: %s\n  target: %s\n", hash.GetHex(),
                                  hashTarget.GetHex());
                        ProcessBlockFound(pblock, chainparams, hash, node);
                        SetThreadPriority(THREAD_PRIORITY_LOWEST);
                        coinbaseScript->KeepScript();
                        // In regression test mode, stop mining after a block is found. This
                        // allows developers to controllably generate a block on demand.
                        if (chainparams.MineBlocksOnDemand())
                            throw boost::thread_interrupted();

                        break;
                    }
                    pblock->nNonce += 1;
                    nHashesDone += 1;
                    if (nHashesDone % 1000 == 0) {   //Calculate hashing speed
                        nHashesPerSec = nHashesDone / (((GetTimeMicros() - nMiningTimeStart) / 1000000.00) + 1);
                        LogPrintf("nNonce: %d, hashRate %f\n", pblock->nNonce, nHashesPerSec);
                        //LogPrintf("FortuneblockMiner:\n  proof-of-work in progress \n  hash: %s\n  target: %s\n, different=%s\n", hash.GetHex(), hashTarget.GetHex(), (UintToArith256(hash) - hashTarget));
                    }
                    if ((pblock->nNonce & 0xFF) == 0)
                        break;
                }

                // Check for stop or if block needs to be rebuilt
                boost::this_thread::interruption_point();
                // Regtest mode doesn't require peers
                //if (vNodes.empty() && chainparams.MiningRequiresPeers())
                //    break;
                if (pblock->nNonce >= 0xffff0000)
                    break;
                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    break;
                if (pindexPrev != ::ChainActive().Tip())
                    break;

                // Update nTime every few seconds
                if (UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev) < 0)
                    break; // Recreate the block if the clock has run backwards,
                // so that we can use the correct time.
                if (chainparams.GetConsensus().fPowAllowMinDifficultyBlocks) {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
            }
        }
    }
    catch (const boost::thread_interrupted &) {
        LogPrintf("FortuneblockMiner -- terminated\n");
        throw;
    }
    catch (const std::runtime_error &e) {
        LogPrintf("FortuneblockMiner -- runtime error: %s\n", e.what());
        return;
    }
}

// TODO: add reference node, get the conn man from there
int GenerateFortuneblocks(bool fGenerate, int nThreads, const CChainParams &chainparams, NodeContext &node) {
    static boost::thread_group *minerThreads = NULL;

    int numCores = GetNumCores();
    if (nThreads < 0)
        nThreads = numCores;

    if (minerThreads != NULL) {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate) {
        return numCores;
    }

    minerThreads = new boost::thread_group();

    //Reset metrics
    nMiningTimeStart = GetTimeMicros();
    nHashesDone = 0;
    nHashesPerSec = 0;

    for (int i = 0; i < nThreads; i++) {
        minerThreads->create_thread(
                boost::bind(&FortuneblockMiner, boost::cref(chainparams), boost::ref(node)));
    }
    return (numCores);
}