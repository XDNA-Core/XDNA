// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The Bulwark developers
// Copyright (c) 2017-2018 The XDNA Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode-payments.h"
#include "addrman.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "obfuscation.h"
#include "spork.h"
#include "sync.h"
#include "util.h"
#include "utilmoneystr.h"
#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>

/** Object for who's going to get paid on which blocks */
CMasternodePayments masternodePayments;

CCriticalSection cs_vecPayments;
CCriticalSection cs_mapMasternodeBlocks;
CCriticalSection cs_mapMasternodePayeeVotes;

bool IsBlockValueValid(const CBlock& block, CAmount nExpectedValue, CAmount nMinted)
{
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (!pindexPrev) return true;

    int nHeight = 0;
    if (pindexPrev->GetBlockHash() == block.hashPrevBlock) {
        nHeight = pindexPrev->nHeight + 1;
    } else { //out of order
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi != mapBlockIndex.end() && mi->second)
            nHeight = mi->second->nHeight + 1;
    }

    if (!nHeight) {
        LogPrintf("IsBlockValueValid() : WARNING: Couldn't find previous block\n");
    }

    if (nMinted > nExpectedValue) {
       return false;
    }

    return true;
}

bool IsBlockPayeeValid(const CBlock& block, int nBlockHeight)
{
    if (!masternodeSync.IsSynced()) { //there is no data to use to check anything -- find the longest chain
        LogPrint("mnpayments", "Client not synced, skipping block payee checks\n");
        return true;
    }

    // disable mn payments checks till fork activation
    if(Params().F2ActivationTime() <= block.nTime)
        return true;

    const CTransaction& txNew = (nBlockHeight > Params().LAST_POW_BLOCK() ? block.vtx[1] : block.vtx[0]);

    //check for masternode payee
    if (masternodePayments.IsTransactionValid(txNew, nBlockHeight, block.nTime))
        return true;

    LogPrintf("Invalid mn payment detected %s\n", txNew.ToString().c_str());

    if (IsSporkActive(SPORK_4_MASTERNODE_PAYMENT_ENFORCEMENT))
        return false;

    LogPrintf("Masternode payment enforcement is disabled, accepting block\n");

    return true;
}

std::string GetRequiredPaymentsString(int nBlockHeight)
{
    return masternodePayments.GetRequiredPaymentsString(nBlockHeight);
}

CAmount CMasternodePayments::FillBlockPayee(CMutableTransaction& txNew, uint32_t nTime, CAmount block_value, bool fProofOfStake)
{
    CBlockIndex* pindexPrev = chainActive.Tip();

    if (!pindexPrev)
        return 0;

    CAmount mn_payments_total = 0;

    for(unsigned mnlevel = CMasternode::LevelValue::MIN; mnlevel <= CMasternode::LevelValue::MAX; ++mnlevel) {

        CScript payee;

        //spork
        if (!masternodePayments.GetBlockPayee(pindexPrev->nHeight + 1, mnlevel, payee)) {
            //no masternode detected
            CMasternode* winningNode = mnodeman.GetCurrentMasterNode(mnlevel, 1);

            if(!winningNode)
                continue;

            payee = GetScriptForDestination(winningNode->pubKeyCollateralAddress.GetID());
        }

        CAmount masternodePayment = GetMasternodePayment(pindexPrev->nHeight, nTime, mnlevel, block_value);

        if(!masternodePayment)
            continue;

        txNew.vout.emplace_back(masternodePayment, payee);

        mn_payments_total += masternodePayment;

        CTxDestination address1;
        ExtractDestination(payee, address1);
        CBitcoinAddress address2(address1);

        LogPrintf("Masternode payment of %s to %s\n", FormatMoney(masternodePayment).c_str(), address2.ToString().c_str());
    }

    return mn_payments_total;
}

int CMasternodePayments::GetMinMasternodePaymentsProto()
{
    return ActiveProtocol();
}

void CMasternodePayments::ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (!masternodeSync.IsBlockchainSynced()) return;

    if (fLiteMode) return; //disable all Obfuscation/Masternode related functionality

    if (strCommand == "mnw") { //Masternode Payments Declare Winner
        //this is required in litemodef
        CMasternodePaymentWinner winner;
        vRecv >> winner;

        if (pfrom->nVersion < ActiveProtocol()) return;

        int nHeight;
        {
            TRY_LOCK(cs_main, locked);
            if (!locked || chainActive.Tip() == NULL) return;
            nHeight = chainActive.Tip()->nHeight;
        }

        CTxDestination address1;
        ExtractDestination(winner.payee, address1);
        CBitcoinAddress payee_addr(address1);

        auto winner_mn = mnodeman.Find(winner.payee);

        if (!winner_mn) {
            LogPrint("mnpayments", "mnw - unknown payee %s\n", payee_addr.ToString().c_str());
            return;
        }

        winner.payeeLevel = winner_mn->Level();

        if (masternodePayments.mapMasternodePayeeVotes.count(winner.GetHash())) {
            LogPrint("mnpayments", "mnw - Already seen - %s bestHeight %d\n", winner.GetHash().ToString().c_str(), nHeight);
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
            return;
        }

        std::string strError = "";
        if (!winner.IsValid(pfrom, strError)) {
            // if(strError != "") LogPrintf("mnw - invalid message - %s\n", strError);
            return;
        }

        int nFirstBlock = nHeight - mnodeman.CountEnabled(winner.payeeLevel) * 125 / 100;
        if (winner.nBlockHeight < nFirstBlock || winner.nBlockHeight > nHeight + 20) {
            LogPrint("mnpayments", "mnw - winner out of range - FirstBlock %d Height %d bestHeight %d\n", nFirstBlock, winner.nBlockHeight, nHeight);
            return;
        }

        if (!masternodePayments.CanVote(winner.vinMasternode.prevout, winner.nBlockHeight, winner.payeeLevel)) {
            //  LogPrint("mnpayments", "mnw - masternode already voted - %s (%d - %d)\n", winner.vinMasternode.prevout.ToStringShort(), winner.payeeLevel, winner.nBlockHeight);
            return;
        }

        if (!winner.SignatureValid()) {
            // LogPrintf("mnw - invalid signature\n");
            if (masternodeSync.IsSynced()) Misbehaving(pfrom->GetId(), 20);
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, winner.vinMasternode);
            return;
        }

        //   LogPrint("mnpayments", "mnw - winning vote - Addr %s Height %d bestHeight %d - %s\n", payee_addr.ToString().c_str(), winner.nBlockHeight, nHeight, winner.vinMasternode.prevout.ToStringShort());

        if (masternodePayments.AddWinningMasternode(winner)) {
            winner.Relay();
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
        }
    }
}

bool CMasternodePaymentWinner::Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode)
{
    std::string errorMessage;
    std::string strMasterNodeSignMessage;

    std::string strMessage = vinMasternode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             payee.ToString();

    if (!obfuScationSigner.SignMessage(strMessage, errorMessage, vchSig, keyMasternode)) {
        LogPrintf("CMasternodePing::Sign() - Error: %s\n", errorMessage.c_str());
        return false;
    }

    if (!obfuScationSigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrintf("CMasternodePing::Sign() - Error: %s\n", errorMessage.c_str());
        return false;
    }

    return true;
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, unsigned mnlevel, CScript& payee)
{
    auto block = mapMasternodeBlocks.find(nBlockHeight);

    if(block == mapMasternodeBlocks.cend())
        return false;

    return block->second.GetPayee(mnlevel, payee);
}

// Is this masternode scheduled to get paid soon?
bool CMasternodePayments::IsScheduled(CMasternode& mn, int nSameLevelMNCount, int nNotBlockHeight) const
{
    LOCK(cs_mapMasternodeBlocks);

    int64_t nHeight;
    {
        TRY_LOCK(cs_main, locked);

        if(!locked)
            return false;

        auto chain_tip = chainActive.Tip();

        if(!chain_tip)
            return false;

        nHeight = chain_tip->nHeight;
    }

    CScript mnpayee = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

    for(int64_t h_upper_bound = nHeight + 10, h = h_upper_bound - std::min(10, nSameLevelMNCount - 1); h < h_upper_bound; ++h) {

        if(h == nNotBlockHeight)
            continue;

        auto block_payees = mapMasternodeBlocks.find(h);

        if(block_payees == mapMasternodeBlocks.cend())
            continue;

        CScript payee;

        if(!block_payees->second.GetPayee(mn.Level(), payee))
            continue;

        if(mnpayee == payee)
            return true;
    }

    return false;
}

bool CMasternodePayments::CanVote(const COutPoint& outMasternode, int nBlockHeight, unsigned mnlevel)
{
    LOCK(cs_mapMasternodePayeeVotes);

    uint256 key = ((outMasternode.hash + outMasternode.n) << 4) + mnlevel;

    auto ins_res = mapMasternodesLastVote.emplace(key, nBlockHeight);

    if(!ins_res.second) {

        auto& last_vote = ins_res.first->second;

        if(last_vote >= nBlockHeight)
            return false;

        last_vote = nBlockHeight;
    }

    return true;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    uint256 blockHash;

    if (!GetBlockHash(blockHash, winnerIn.nBlockHeight - 100))
        return false;

    {
        LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

        auto vote_ins_res = mapMasternodePayeeVotes.emplace(winnerIn.GetHash(), winnerIn);

        if(!vote_ins_res.second)
            return false;

        auto mnblock = mapMasternodeBlocks.emplace(winnerIn.nBlockHeight, winnerIn.nBlockHeight).first;

        mnblock->second.AddPayee(winnerIn.payeeLevel, winnerIn.payee, 1);
    }

    return true;
}

bool CMasternodeBlockPayees::IsTransactionValid(const CTransaction& txNew, uint32_t nTime)
{
    LOCK(cs_vecPayments);

    std::map<unsigned, int> max_signatures;

    //require at least 6 signatures
    for(CMasternodePayee& payee : vecPayments) {

        if(payee.nVotes < MNPAYMENTS_SIGNATURES_REQUIRED)
            continue;

        auto ins_res = max_signatures.emplace(payee.mnlevel, payee.nVotes);

        if(ins_res.second)
            continue;

        if(payee.nVotes >= ins_res.first->second)
            ins_res.first->second = payee.nVotes;
    }

    // if we don't have at least 6 signatures on a payee, approve whichever is the longest chain
    if(!max_signatures.size())
        return true;

    CAmount nReward = GetBlockValue(nBlockHeight, nTime);

    std::string strPayeesPossible;

    for(const CMasternodePayee& payee : vecPayments) {

        if(payee.nVotes < MNPAYMENTS_SIGNATURES_REQUIRED)
            continue;

        auto requiredMasternodePayment = GetMasternodePayment(nBlockHeight, nTime, payee.mnlevel, nReward);

        auto payee_out = std::find_if(txNew.vout.cbegin(), txNew.vout.cend(), [&payee, &requiredMasternodePayment](const CTxOut& out){

            auto is_payee          = payee.scriptPubKey == out.scriptPubKey;
            auto is_value_required = out.nValue >= requiredMasternodePayment;

            if(is_payee && !is_value_required)
                LogPrintf("Masternode payment is out of drift range. Paid=%s Min=%s\n", FormatMoney(out.nValue).c_str(), FormatMoney(requiredMasternodePayment).c_str());

            return is_payee && is_value_required;
        });

        if(payee_out != txNew.vout.cend()) {

            max_signatures.erase(payee.mnlevel);

            if(max_signatures.size())
                continue;

            return true;
        }

        CTxDestination address1;
        ExtractDestination(payee.scriptPubKey, address1);

        auto address2 = std::to_string(payee.mnlevel) + ":" + CBitcoinAddress{address1}.ToString();

        if(strPayeesPossible == "")
            strPayeesPossible += address2;
        else
            strPayeesPossible += "," + address2;
    }

    LogPrintf("CMasternodePayments::IsTransactionValid - Missing required payment to %s\n", strPayeesPossible.c_str());
    return false;
}

std::string CMasternodeBlockPayees::GetRequiredPaymentsString()
{
    LOCK(cs_vecPayments);

    std::string ret = "Unknown";

    for(CMasternodePayee& payee : vecPayments) {
        CTxDestination address1;
        ExtractDestination(payee.scriptPubKey, address1);
        CBitcoinAddress address2(address1);

        std::string payee_str = address2.ToString() + ":"
                              + boost::lexical_cast<std::string>(payee.mnlevel) + ":"
                              + boost::lexical_cast<std::string>(payee.nVotes);

        if (ret != "Unknown") {
            ret += "," + payee_str;
        } else {
            ret = payee_str;
        }
    }

    return ret;
}

std::string CMasternodePayments::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    auto mn_block = mapMasternodeBlocks.find(nBlockHeight);

    if(mn_block == mapMasternodeBlocks.end())
        return "Unknown";

    return mn_block->second.GetRequiredPaymentsString();
}

bool CMasternodePayments::IsTransactionValid(const CTransaction& txNew, int nBlockHeight, uint32_t nTime)
{
    LOCK(cs_mapMasternodeBlocks);

    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].IsTransactionValid(txNew, nTime);
    }

    return true;
}

void CMasternodePayments::CleanPaymentList()
{
    LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || !chainActive.Tip()) return;
        nHeight = chainActive.Tip()->nHeight;
    }

    //keep up to five cycles for historical sake
    int nLimit = std::max(mnodeman.size() * 125 / 100, 1000);

    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = it->second;

        if (nHeight - winner.nBlockHeight > nLimit) {
            LogPrint("mnpayments", "CMasternodePayments::CleanPaymentList - Removing old Masternode payment - block %d\n", winner.nBlockHeight);
            masternodeSync.mapSeenSyncMNW.erase(it->first);
            mapMasternodeBlocks.erase(winner.nBlockHeight);
            it = mapMasternodePayeeVotes.erase(it);
        } else {
            ++it;
        }
    }
}

bool CMasternodePaymentWinner::IsValid(CNode* pnode, std::string& strError)
{
    CMasternode* pmn = mnodeman.Find(vinMasternode);

    if (!pmn) {
        strError = strprintf("Unknown Masternode %s", vinMasternode.prevout.hash.ToString());
        LogPrintf("CMasternodePaymentWinner::IsValid - %s\n", strError);
        mnodeman.AskForMN(pnode, vinMasternode);
        return false;
    }

    if (pmn->protocolVersion < ActiveProtocol()) {
        strError = strprintf("Masternode protocol too old %d - req %d", pmn->protocolVersion, ActiveProtocol());
        LogPrintf("CMasternodePaymentWinner::IsValid - %s\n", strError);
        return false;
    }

    int n = mnodeman.GetMasternodeRank(vinMasternode, nBlockHeight - 100, ActiveProtocol());

    if(n == -1) {
        strError = strprintf("Unknown Masternode (rank==-1) %s", vinMasternode.prevout.hash.ToString());
        LogPrintf("CMasternodePaymentWinner::IsValid - %s\n", strError);
        return false;
    }

    if (n > MNPAYMENTS_SIGNATURES_TOTAL) {
        //It's common to have masternodes mistakenly think they are in the top 10
        // We don't want to print all of these messages, or punish them unless they're way off
        if (n > MNPAYMENTS_SIGNATURES_TOTAL * 2) {
            strError = strprintf("Masternode not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL * 2, n);
            LogPrintf("CMasternodePaymentWinner::IsValid - %s\n", strError);
            if (masternodeSync.IsSynced()) Misbehaving(pnode->GetId(), 20);
        }
        return false;
    }

    return true;
}

bool CMasternodePayments::ProcessBlock(int nBlockHeight)
{
    if(!fMasterNode)
        return false;

    auto nWinnerBlockHeight = nBlockHeight + 10;

    //reference node - hybrid mode

    if(nWinnerBlockHeight <= nLastBlockHeight)
        return false;

    int n = mnodeman.GetMasternodeRank(activeMasternode.vin, nWinnerBlockHeight - 100, ActiveProtocol());

    if(n == -1) {
        LogPrint("mnpayments", "CMasternodePayments::ProcessBlock - Unknown Masternode\n");
        return false;
    }

    if(n > MNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint("mnpayments", "CMasternodePayments::ProcessBlock - Masternode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, n);
        return false;
    }

    LogPrintf("CMasternodePayments::ProcessBlock() Start nHeight %d - vin %s. \n", nWinnerBlockHeight, activeMasternode.vin.prevout.hash.ToString());
    // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough

    std::string errorMessage;
    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if (!obfuScationSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode)) {
        LogPrintf("CMasternodePayments::ProcessBlock() - Error upon calling SetKey: %s\n", errorMessage.c_str());
        return false;
    }

    std::vector<CMasternodePaymentWinner> winners;

    for(unsigned mnlevel = CMasternode::LevelValue::MIN; mnlevel <= CMasternode::LevelValue::MAX; ++mnlevel) {

        unsigned nCount = 0;

        auto pmn = mnodeman.GetNextMasternodeInQueueForPayment(nWinnerBlockHeight, mnlevel, true, nCount);

        if(!pmn) {
            LogPrintf("CMasternodePayments::ProcessBlock() Failed to find masternode level %d to pay \n", mnlevel);
            continue;
        }

        auto payee = GetScriptForDestination(pmn->pubKeyCollateralAddress.GetID());

        CMasternodePaymentWinner newWinner{activeMasternode.vin};

        newWinner.nBlockHeight = nWinnerBlockHeight;
        newWinner.AddPayee(payee, mnlevel);

        CTxDestination address1;
        ExtractDestination(payee, address1);
        CBitcoinAddress address2{address1};

        LogPrintf("CMasternodePayments::ProcessBlock() Winner payee %s nHeight %d level %d. \n", address2.ToString().c_str(), newWinner.nBlockHeight, mnlevel);

        LogPrintf("CMasternodePayments::ProcessBlock() - Signing Winner level %d\n", mnlevel);

        if(!newWinner.Sign(keyMasternode, pubKeyMasternode))
            continue;

        LogPrintf("CMasternodePayments::ProcessBlock() - AddWinningMasternode level %d\n", mnlevel);

        if(!AddWinningMasternode(newWinner))
            continue;

        winners.emplace_back(newWinner);
    }

    if(winners.empty())
        return false;

    for(auto& winner : winners) {
        winner.Relay();
    }

    nLastBlockHeight = nWinnerBlockHeight;

    return true;
}

void CMasternodePaymentWinner::Relay()
{
    CInv inv(MSG_MASTERNODE_WINNER, GetHash());
    RelayInv(inv);
}

bool CMasternodePaymentWinner::SignatureValid()
{
    CMasternode* pmn = mnodeman.Find(vinMasternode);

    if (!pmn)
        return false;

    std::string strMessage = vinMasternode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             payee.ToString();

    std::string errorMessage;

    if (!obfuScationSigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        return error("CMasternodePaymentWinner::SignatureValid() - Got bad Masternode address signature %s\n", vinMasternode.prevout.hash.ToString());
    }

    return true;
}

void CMasternodePayments::Sync(CNode* node, int nCountNeeded)
{
    LOCK(cs_mapMasternodePayeeVotes);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if(!locked || !chainActive.Tip())
            return;

        nHeight = chainActive.Tip()->nHeight;
    }

    auto mn_counts = mnodeman.CountEnabledByLevels();

    unsigned max_mn_count = 0u;

    for(auto& count : mn_counts)
        max_mn_count = std::max(max_mn_count, count.second * 125 / 100);

    if(max_mn_count > nCountNeeded)
        max_mn_count = nCountNeeded;

    int nInvCount = 0;

    for(const auto& vote : mapMasternodePayeeVotes) {

        const auto& winner = vote.second;

        bool push =  winner.nBlockHeight >= nHeight - max_mn_count
                  && winner.nBlockHeight <= nHeight + 20;

        if(!push)
            continue;

         node->PushInventory(CInv(MSG_MASTERNODE_WINNER, winner.GetHash()));
         ++nInvCount;
    }

    node->PushMessage("ssc", MASTERNODE_SYNC_MNW, nInvCount);
}

std::string CMasternodePayments::ToString() const
{
    std::ostringstream info;

    info << "Votes: " << (int)mapMasternodePayeeVotes.size() << ", Blocks: " << (int)mapMasternodeBlocks.size();

    return info.str();
}
