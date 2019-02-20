// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chain.h"

using namespace std;

/**
 * CChain implementation
 */

int64_t CChain::GetNetworkHashPS(int lookup, int height)
{
    --height;

    CBlockIndex *pb = this->Tip();

    if (height >= 0 && height < this->Height())
        pb = vChain[height];

    if (!(pb && pb->nHeight))
        return 0;

    CBlockIndex* pb0 = pb;
    int64_t minTime = pb0->GetBlockTime();
    int64_t maxTime = minTime;

    for (int i = lookup <= 0 ? std::numeric_limits<int>::max() : lookup; i && pb0->pprev; --i) {

        if(lookup <= 0 && pb0->pprev->nBits != pb->nBits)
            break;

        pb0 = pb0->pprev;
        int64_t time = pb0->GetBlockTime();
        minTime = std::min(time, minTime);
        maxTime = std::max(time, maxTime);
    }

    // In case there's a situation where minTime == maxTime, we don't want a divide by zero exception.
    if (minTime == maxTime)
        return 0;

    uint256 workDiff = pb->nChainWork - pb0->nChainWork;
    int64_t timeDiff = maxTime - minTime;

    return workDiff.getdouble() / timeDiff;
}

void CChain::SetTip(CBlockIndex* pindex)
{
    if (pindex == NULL) {
        vChain.clear();
        return;
    }
    vChain.resize(pindex->nHeight + 1);
    while (pindex && vChain[pindex->nHeight] != pindex) {
        vChain[pindex->nHeight] = pindex;
        pindex = pindex->pprev;
    }
}

CBlockLocator CChain::GetLocator(const CBlockIndex* pindex) const
{
    int nStep = 1;
    std::vector<uint256> vHave;
    vHave.reserve(32);

    if (!pindex)
        pindex = Tip();
    while (pindex) {
        vHave.push_back(pindex->GetBlockHash());
        // Stop when we have added the genesis block.
        if (pindex->nHeight == 0)
            break;
        // Exponentially larger steps back, plus the genesis block.
        int nHeight = std::max(pindex->nHeight - nStep, 0);
        if (Contains(pindex)) {
            // Use O(1) CChain index if possible.
            pindex = (*this)[nHeight];
        } else {
            // Otherwise, use O(log n) skiplist.
            pindex = pindex->GetAncestor(nHeight);
        }
        if (vHave.size() > 10)
            nStep *= 2;
    }

    return CBlockLocator(vHave);
}

const CBlockIndex* CChain::FindFork(const CBlockIndex* pindex) const
{
    if (pindex->nHeight > Height())
        pindex = pindex->GetAncestor(Height());
    while (pindex && !Contains(pindex))
        pindex = pindex->pprev;
    return pindex;
}

uint256 CBlockIndex::GetBlockTrust() const
{
    uint256 bnTarget;
    bnTarget.SetCompact(nBits);
    if (bnTarget <= 0)
        return 0;

    if (IsProofOfStake()) {
        // Return trust score as usual
        return (uint256(1) << 256) / (bnTarget + 1);
    } else {
        // Calculate work amount for block
        uint256 bnPoWTrust = ((~uint256(0) >> 20) / (bnTarget + 1));
        return bnPoWTrust > 1 ? bnPoWTrust : 1;
    }
}
