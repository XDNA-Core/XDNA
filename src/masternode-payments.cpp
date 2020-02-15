// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The Bulwark developers
// Copyright (c) 2017-2020 The XDNA Core developers
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

//

// CMasternodePaymentDB
//

CMasternodePaymentDB::CMasternodePaymentDB()
{
    pathDB = GetDataDir() / "mnpayments.dat";
    strMagicMessage = "MasternodePayments";
}

bool CMasternodePaymentDB::Write(const CMasternodePayments& objToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssObj(SER_DISK, CLIENT_VERSION);
    ssObj << strMagicMessage;                   // masternode cache file specific magic message
    ssObj << FLATDATA(Params().MessageStart()); // network specific magic number
    ssObj << objToSave;
    uint256 hash = Hash(ssObj.begin(), ssObj.end());
    ssObj << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathDB.string());

    // Write and commit header, data
    try {
        fileout << ssObj;
    } catch (std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    fileout.fclose();

    LogPrint("masternode","Written info to mnpayments.dat  %dms\n", GetTimeMillis() - nStart);

    return true;
}

CMasternodePaymentDB::ReadResult CMasternodePaymentDB::Read(CMasternodePayments& objToLoad, bool fDryRun)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathDB.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathDB);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char*)&vchData[0], dataSize);
        filein >> hashIn;
    } catch (std::exception& e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssObj(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssObj.begin(), ssObj.end());
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (masternode cache file specific magic message) and ..
        ssObj >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid masternode payement cache magic message", __func__);
            return IncorrectMagicMessage;
        }


        // de-serialize file header (network specific magic number) and ..
        ssObj >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize data into CMasternodePayments object
        ssObj >> objToLoad;
    } catch (std::exception& e) {
        objToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint("masternode","Loaded info from mnpayments.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint("masternode","  %s\n", objToLoad.ToString());
    if (!fDryRun) {
        LogPrint("masternode","Masternode payments manager - cleaning....\n");
        objToLoad.CleanPaymentList();
        LogPrint("masternode","Masternode payments manager - result:\n");
        LogPrint("masternode","  %s\n", objToLoad.ToString());
    }

    return Ok;
}

void DumpMasternodePayments()
{
    int64_t nStart = GetTimeMillis();

    CMasternodePaymentDB paymentdb;
    CMasternodePayments tempPayments;

    LogPrint("masternode","Verifying mnpayments.dat format...\n");
    CMasternodePaymentDB::ReadResult readResult = paymentdb.Read(tempPayments, true);
    // there was an error and it was not an error on file opening => do not proceed
    if (readResult == CMasternodePaymentDB::FileError)
        LogPrint("masternode","Missing budgets file - mnpayments.dat, will try to recreate\n");
    else if (readResult != CMasternodePaymentDB::Ok) {
        LogPrint("masternode","Error reading mnpayments.dat: ");
        if (readResult == CMasternodePaymentDB::IncorrectFormat)
            LogPrint("masternode","magic is ok but data has invalid format, will try to recreate\n");
        else {
            LogPrint("masternode","file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrint("masternode","Writting info to mnpayments.dat...\n");
    paymentdb.Write(masternodePayments);

    LogPrint("masternode","Budget dump finished  %dms\n", GetTimeMillis() - nStart);
}

bool IsBlockValueValid(const CBlock& block, CAmount nExpectedValue, CAmount nMinted)
{
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (pindexPrev == NULL) return true;

    int nHeight = 0;
    if (pindexPrev->GetBlockHash() == block.hashPrevBlock) {
        nHeight = pindexPrev->nHeight + 1;
    } else { //out of order
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi != mapBlockIndex.end() && (*mi).second)
            nHeight = (*mi).second->nHeight + 1;
    }

    if (nHeight == 0) {
        LogPrint("masternode","IsBlockValueValid() : WARNING: Couldn't find previous block\n");
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
    if(Params().F3Activation() >= nBlockHeight)
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
    if (!pindexPrev) return 0;


    CAmount mn_payments_total = 0;

    for(unsigned mnlevel = CMasternode::LevelValue::MIN; mnlevel <= CMasternode::LevelValue::MAX; ++mnlevel) {

        CScript payee;

        //spork
        if (!masternodePayments.GetBlockPayee(pindexPrev->nHeight + 1, mnlevel, payee)) {
            //no masternode detected
            CMasternode* winningNode = mnodeman.GetCurrentMasterNode(mnlevel, 1);

            if(!winningNode) {
                LogPrint("masternode","CreateNewBlock: Failed to detect masternode level %d to pay\n", mnlevel);
                continue;
            }

            payee = GetScriptForDestination(winningNode->pubKeyCollateralAddress.GetID());
        }

        CAmount masternodePayment = GetMasternodePayment(pindexPrev->nHeight + 1, nTime, mnlevel, block_value);

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

    if (strCommand == "mnget") { //Masternode Payments Request Sync

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if (Params().NetworkID() == CBaseChainParams::MAIN) {

            if (pfrom->HasFulfilledRequest("mnget")) {
                LogPrintf("mnget - peer already asked me for the list\n");
                Misbehaving(pfrom->GetId(), 20);
                return;
            }
        }

        pfrom->FulfilledRequest("mnget");
        masternodePayments.Sync(pfrom, nCountNeeded);
        LogPrint("mnpayments", "CMasternodePayments - mnget - Sent Masternode winners to peer %i\n", pfrom->GetId());
    } else

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

        CMasternode* winner_mn;

        // If the payeeVin is empty mean that winner object come from an old version, so I use the old logic
        if (winner.payeeVin == CTxIn()) {
            winner_mn = mnodeman.Find(winner.payee);

            if (winner_mn != NULL)
            {
                winner.payeeLevel = winner_mn->Level();
                winner.payeeVin = winner_mn->vin;
            }
        } else {
            winner_mn = mnodeman.Find(winner.payeeVin);
        }

        if (!winner_mn) {
            LogPrint("mnpayments", "mnw - unknown payee from peer=%s ip=%s - %s\n", pfrom->GetId(), pfrom->addr.ToString().c_str(), payee_addr.ToString().c_str());

            // If I received an unknown payee I try to ask to the peer the updaded version of the masternode list
            // however the DsegUpdate function do that only 1 time every 3h
            if (winner.payeeVin == CTxIn())
                mnodeman.DsegUpdate(pfrom);
            else
                mnodeman.AskForMN(pfrom, winner.payeeVin);

            return;
        }

        std::string logString = strprintf("mnw - peer=%s ip=%s v=%d addr=%s winHeight=%d vin=%s",
            pfrom->GetId(),
            pfrom->addr.ToString().c_str(),
            pfrom->nVersion,
            payee_addr.ToString().c_str(),
            winner.nBlockHeight,
            winner.vinMasternode.prevout.ToStringShort() );

        if (masternodePayments.mapMasternodePayeeVotes.count(winner.GetHash())) {
            LogPrint("mnpayments", "mnw - Already seen - %s bestHeight %d\n", winner.GetHash().ToString().c_str(), nHeight);
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
            return;
        }

        int nFirstBlock = nHeight - (mnodeman.CountEnabled(winner.payeeLevel) * 1.25);
        if (winner.nBlockHeight < nFirstBlock || winner.nBlockHeight > nHeight + 20) {
            LogPrint("mnpayments", "mnw - winner out of range - FirstBlock %d Height %d bestHeight %d\n", nFirstBlock, winner.nBlockHeight, nHeight);
            return;
        }

        std::string strError = "";
        if (!winner.IsValid(pfrom, strError)) {
            if(strError != "") LogPrint("mnpayments", "mnw - invalid message from peer=%s ip=%s - %s\n", pfrom->GetId(), pfrom->addr.ToString().c_str(), strError);
            return;
        }

        if (!masternodePayments.CanVote(winner.vinMasternode.prevout, winner.nBlockHeight, winner.payeeLevel)) {
            LogPrint("mnpayments", "%s - already voted\n", logString.c_str());
            return;
        }

        if (!winner.SignatureValid()) {
            if (masternodeSync.IsSynced()) {
                LogPrintf("CMasternodePayments::ProcessMessageMasternodePayments() : mnw - invalid signature from peer=%s ip=%s\n", pfrom->GetId(), pfrom->addr.ToString().c_str());

                // Ban after 5 times
                TRY_LOCK(cs_main, locked);
                if (locked) Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, winner.vinMasternode);
            return;
        }

        LogPrint("mnpayments", "mnw - winning vote - Addr %s Height %d bestHeight %d - %s\n", payee_addr.ToString().c_str(), winner.nBlockHeight, nHeight, winner.vinMasternode.prevout.ToStringShort());

        if (masternodePayments.AddWinningMasternode(winner)) {
            winner.Relay();
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
        }
    }

    if (strCommand == "mnwp") { //Masternode Payments Declare Winner pack
        if (pfrom->nVersion < ActiveProtocol())
            return;
        LogPrint("mnpayments", "mnwp - recived from peer %d %s, size=%d\n", pfrom->GetId(), pfrom->addr.ToString(), vRecv.size());
        int nHeight;
        {
            LOCK(cs_main);
            nHeight = chainActive.Tip()->nHeight;
        }

        bool bRelay = false;
        vRecv >> bRelay;
        std::vector<CMasternodePaymentWinner> winners;
        while (!vRecv.empty()) {
            CMasternodePaymentWinner winner;
            vRecv >> winner;

            CTxDestination address1;
            ExtractDestination(winner.payee, address1);
            CBitcoinAddress payee_addr(address1);

            auto winner_mn = mnodeman.Find(winner.payee);

            if (!winner_mn) {
                LogPrintf("mnwp - unknown payee %s\n", payee_addr.ToString().c_str());
                continue;
            }

            winner.payeeLevel = winner_mn->Level();

            if (masternodePayments.mapMasternodePayeeVotes.count(winner.GetHash())) {
                LogPrint("mnpayments", "mnwp - Already seen - %s bestHeight %d\n", winner.GetHash().ToString().c_str(), nHeight);
                LogPrint("mnpayments", "winner: %s\n", winner.ToString());
                masternodeSync.AddedMasternodeWinner(winner.GetHash());
                continue;
            }

            std::string strError = "";
            if (!winner.IsValid(pfrom, strError)) {
                if(strError != "") LogPrintf("mnwp - invalid message - %s\n", strError);
                continue;
            }

            int nFirstBlock = nHeight - int(mnodeman.CountEnabled(winner.payeeLevel) * 1.25) - 1; // / 100 * 125;
            if (winner.nBlockHeight < nFirstBlock || winner.nBlockHeight > nHeight + 20) {
                LogPrint("mnpayments", "mnwp - winner out of range - FirstBlock %d Height %d bestHeight %d\n", nFirstBlock, winner.nBlockHeight, nHeight);
                continue;
            }

            if (!masternodePayments.CanVote(winner.vinMasternode.prevout, winner.nBlockHeight, winner.payeeLevel) && bRelay) {
                LogPrint("mnpayments", "mnwp - masternode already voted - %s block %d\n", winner.vinMasternode.prevout.ToStringShort(), winner.nBlockHeight);
                continue;
            }

            if (!winner.SignatureValid()) {
                LogPrint("mnpayments", "mnwp - invalid signature\n");
                if (masternodeSync.IsSynced()) Misbehaving(pfrom->GetId(), 20);
                // it could just be a non-synced masternode
                mnodeman.AskForMN(pfrom, winner.vinMasternode);
                continue;
            }

            LogPrint("mnpayments", "mnwp - winning vote - Addr %s Height %d bestHeight %d - %s\n", payee_addr.ToString().c_str(), winner.nBlockHeight, nHeight, winner.vinMasternode.prevout.ToStringShort());

            if (masternodePayments.AddWinningMasternode(winner)) {
                LogPrint("mnpayments", "add winner %s\n", winner.ToString());
                if (bRelay)
                    winners.emplace_back(winner);
                masternodeSync.AddedMasternodeWinner(winner.GetHash());
            }
        }

        if(winners.empty())
            return;
        LogPrint("mnpayments", "mnwp - winners to send: %d\n", winners.size());
        if (bRelay) {
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss << bRelay;
            for (auto& winner : winners)
                ss << winner;
            {
                LOCK(cs_vNodes);
                for (CNode* pnode : vNodes)
                    if (pfrom->GetId() != pnode->GetId()) pnode->PushMessage("mnwp", ss);
            }
        }
    }
}

bool CMasternodePaymentWinner::Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode)
{
    std::string errorMessage;
    std::string strMasterNodeSignMessage;

    std::string strMessage = vinMasternode.prevout.ToStringShort() + std::to_string(nBlockHeight) + payee.ToString();

    if (!obfuScationSigner.SignMessage(strMessage, errorMessage, vchSig, keyMasternode)) {
        LogPrint("masternode","CMasternodePing::Sign() - Error: %s\n", errorMessage.c_str());
        return false;
    }

    if (!obfuScationSigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrint("masternode","CMasternodePing::Sign() - Error: %s\n", errorMessage.c_str());
        return false;
    }

    return true;
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, unsigned mnlevel, CScript& payee)
{
    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetPayee(mnlevel, payee);
    }

        return false;
}

// Is this masternode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 winners
bool CMasternodePayments::IsScheduled(CMasternode& mn, int nNotBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    int64_t nHeight;
    {
        TRY_LOCK(cs_main, locked);

        if (!locked)
            return false;

        auto chain_tip = chainActive.Tip();

        if(!chain_tip)
            return false;

        nHeight = chain_tip->nHeight;
    }

    CScript mnpayee = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

    CScript payee;
    for (int64_t h = nHeight; h <= nHeight + 8; ++h) {
        if (h == nNotBlockHeight) continue;
        if (mapMasternodeBlocks.count(h)) {
            if (mapMasternodeBlocks.at(h).GetPayee(mn.Level(), payee)) {
                if (mnpayee == payee) {
                    return true;
                }
            }
        }
    }

            return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    uint256 blockHash = 0;

    if (!GetBlockHash(blockHash, winnerIn.nBlockHeight - 100)) {
        return false;
    }

    {
        LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

        if (mapMasternodePayeeVotes.count(winnerIn.GetHash())) {
            return false;
        }

        mapMasternodePayeeVotes[winnerIn.GetHash()] = winnerIn;

        if (!mapMasternodeBlocks.count(winnerIn.nBlockHeight)) {
            CMasternodeBlockPayees blockPayees(winnerIn.nBlockHeight);
            mapMasternodeBlocks[winnerIn.nBlockHeight] = blockPayees;
        }
    }

    mapMasternodeBlocks[winnerIn.nBlockHeight].AddPayee(winnerIn.payeeLevel, winnerIn.payee, winnerIn.payeeVin, 1);

    return true;
}

bool CMasternodeBlockPayees::IsTransactionValid(const CTransaction& txNew, uint32_t nTime)
{
    LOCK(cs_vecPayments);

    std::map<unsigned, int> max_signatures;

    //require at least 6 signatures
    for (CMasternodePayee& payee : vecPayments) {
        if (payee.nVotes < MNPAYMENTS_SIGNATURES_REQUIRED || (payee.mnlevel != CMasternode::LevelValue::MAX))
            continue;

        std::pair<std::map<unsigned, int>::iterator, bool> ins_res = max_signatures.emplace(payee.mnlevel, payee.nVotes);

        if(ins_res.second)
            continue;

        if (payee.nVotes > ins_res.first->second)
            ins_res.first->second = payee.nVotes;
    }
    LogPrint("mnpayments", "-- Selecting signatures end -- signatures size: %d\n", max_signatures.size());

    // if we don't have no one signatures on a payee, approve whichever is the longest chain
    if (!max_signatures.size()) {
        LogPrint("mnpayments","CMasternodePayments::IsTransactionValid - Not enough signatures, accepting\n");
        return true;
    }

    CAmount nReward = GetBlockValue(nBlockHeight, nTime);

    std::string strPayeesPossible;

    for (const CMasternodePayee& payee : vecPayments) {
        auto requiredMasternodePayment = GetMasternodePayment(nBlockHeight, nTime, payee.mnlevel, nReward);

        if (strPayeesPossible != "")
            strPayeesPossible += ",";

        CTxDestination address1;
        ExtractDestination(payee.scriptPubKey, address1);

        strPayeesPossible += std::to_string(payee.mnlevel) + ":" + CBitcoinAddress(address1).ToString() + "(" + std::to_string(payee.nVotes) + ")=" + FormatMoney(requiredMasternodePayment).c_str();

        if (payee.nVotes < MNPAYMENTS_SIGNATURES_REQUIRED || (payee.mnlevel != CMasternode::LevelValue::MAX)) {
            LogPrint("mnpayments","CMasternodePayments::IsTransactionValid - Payment level %d found to %s vote=%d **\n", payee.mnlevel, CBitcoinAddress(address1).ToString(), payee.nVotes);
            continue;
        }

        auto payee_out = std::find_if(txNew.vout.cbegin(), txNew.vout.cend(), [&payee, &requiredMasternodePayment](const CTxOut& out){
            auto is_payee          = payee.scriptPubKey == out.scriptPubKey;
            auto is_value_required = out.nValue >= requiredMasternodePayment;

            if (is_payee && !is_value_required)
                LogPrint("masternode","Masternode payment is out of drift range. Paid=%s Min=%s\n", FormatMoney(out.nValue).c_str(), FormatMoney(requiredMasternodePayment).c_str());

            return is_payee && is_value_required;
        });

        if (payee_out != txNew.vout.cend()) {
            auto it = max_signatures.find(payee.mnlevel);
            if (it != max_signatures.end())
                max_signatures.erase(payee.mnlevel);

            LogPrint("mnpayments","CMasternodePayments::IsTransactionValid - Payment level %d found to %s vote=%d\n", payee.mnlevel, CBitcoinAddress(address1).ToString(), payee.nVotes);

            if (max_signatures.size())
                continue;

            LogPrint("mnpayments","CMasternodePayments::IsTransactionValid - Payment accepted to %s\n", strPayeesPossible.c_str());
            return true;
        }

        LogPrint("mnpayments","CMasternodePayments::IsTransactionValid - Payment level %d NOT found to %s vote=%d\n", payee.mnlevel, CBitcoinAddress(address1).ToString(), payee.nVotes);
    }

    LogPrint("mnpayments","CMasternodePayments::IsTransactionValid - Missing required payment to %s\n", strPayeesPossible.c_str());
    LogPrint("mnpayments","CMasternodePayments::IsTransactionValid - TX Contents:\n");
    for (auto it = std::begin(txNew.vout); it != std::end(txNew.vout); ++it) {
        CTxDestination address1;
        ExtractDestination((*it).scriptPubKey, address1);
        LogPrint("mnpayments","CMasternodePayments::IsTransactionValid -     Address %s Value %s\n", CBitcoinAddress(address1).ToString(), FormatMoney((*it).nValue).c_str());
    }

    // If I haven't found the valid winners I try to ask to the other peers the list of updated masternode winners list
    for (CNode* pnode : vNodes) {
        if(mnodeman.WinnersUpdate(pnode))
            LogPrint("mnpayments","Sending mnget: peer=%d ip=%s v=%d\n", pnode->id, pnode->addr.ToString().c_str(), pnode->nVersion);
    }

    LogPrint("masternode","CMasternodePayments::IsTransactionValid - Missing required payment to %s\n", strPayeesPossible.c_str());
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
                              + std::to_string(payee.mnlevel) + ":"
                              + std::to_string(payee.nVotes);

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

    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetRequiredPaymentsString();
    }

        return "Unknown";
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
        if (!locked || chainActive.Tip() == NULL) return;
        nHeight = chainActive.Tip()->nHeight;
    }

    //keep up to five cycles for historical sake
    int nLimit = std::max(int(mnodeman.size() * 1.25), 1000);

    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = (*it).second;

        if (nHeight - winner.nBlockHeight > nLimit) {
            LogPrint("mnpayments", "CMasternodePayments::CleanPaymentList - Removing old Masternode payment - block %d\n", winner.nBlockHeight);
            masternodeSync.mapSeenSyncMNW.erase((*it).first);
            mapMasternodePayeeVotes.erase(it++);
            mapMasternodeBlocks.erase(winner.nBlockHeight);
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
        LogPrint("masternode","CMasternodePaymentWinner::IsValid - %s\n", strError);
        mnodeman.AskForMN(pnode, vinMasternode);
        return false;
    }

    if (pmn->protocolVersion < ActiveProtocol()) {
        strError = strprintf("Masternode protocol too old %d - req %d", pmn->protocolVersion, ActiveProtocol());
        LogPrint("masternode","CMasternodePaymentWinner::IsValid - %s\n", strError);
        return false;
    }

    int n = mnodeman.GetMasternodeRank(vinMasternode, nBlockHeight - 100, ActiveProtocol());

    if(n == -1) {
        strError = strprintf("Unknown Masternode (rank==-1) %s", vinMasternode.prevout.hash.ToString());
        LogPrint("masternode","CMasternodePaymentWinner::IsValid - %s\n", strError);
        return false;
    }

    if (n > MNPAYMENTS_SIGNATURES_TOTAL) {
        //It's common to have masternodes mistakenly think they are in the top 10
        // We don't want to print all of these messages, or punish them unless they're way off
        if (n > MNPAYMENTS_SIGNATURES_TOTAL * 2) {
            strError = strprintf("Masternode not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL * 2, n);
            LogPrint("masternode","CMasternodePaymentWinner::IsValid - %s\n", strError);
            if (masternodeSync.IsSynced()) {                         TRY_LOCK(cs_main, locked);
                if (locked) Misbehaving(pnode->GetId(), 20);
        }
        }
        return false;
    }

    return true;
}

bool CMasternodePayments::ProcessBlock(int nBlockHeight)
{
    if(!fMasterNode)
        return false;

    //reference node - hybrid mode

    if (nBlockHeight <= nLastBlockHeight)
        return false;

    int n = mnodeman.GetMasternodeRank(activeMasternode.vin, nBlockHeight - 100, ActiveProtocol());

    if(n == -1) {
        LogPrint("mnpayments", "CMasternodePayments::ProcessBlock - Unknown Masternode\n");
        return false;
    }

    if(n > MNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint("mnpayments", "CMasternodePayments::ProcessBlock - Masternode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, n);
        return false;
    }

    LogPrint("masternode","CMasternodePayments::ProcessBlock() Start nHeight %d - vin %s. \n", nBlockHeight, activeMasternode.vin.prevout.hash.ToString());
    // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough

    std::string errorMessage;
    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if (!obfuScationSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode)) {
        LogPrint("masternode","CMasternodePayments::ProcessBlock() - Error upon calling SetKey: %s\n", errorMessage.c_str());
        return false;
    }

    std::vector<CMasternodePaymentWinner> winners;

    for(unsigned mnlevel = CMasternode::LevelValue::MIN; mnlevel <= CMasternode::LevelValue::MAX; ++mnlevel) {
            CMasternodePaymentWinner newWinner(activeMasternode.vin);

        unsigned nCount = 0;

            CMasternode* pmn = mnodeman.GetNextMasternodeInQueueForPayment(nBlockHeight, mnlevel, true, nCount);

        if(!pmn) {
            LogPrint("mnpayments", "CMasternodePayments::ProcessBlock() Failed to find masternode level %d to pay \n", mnlevel);
            continue;
        }

        CScript payee = GetScriptForDestination(pmn->pubKeyCollateralAddress.GetID());

            newWinner.nBlockHeight = nBlockHeight;
            newWinner.AddPayee(payee, mnlevel, pmn->vin);

            CTxDestination address1;
            ExtractDestination(payee, address1);
            CBitcoinAddress address2(address1);

            LogPrint("masternode","CMasternodePayments::ProcessBlock() Winner payee %s nHeight %d level %d. \n", address2.ToString().c_str(), newWinner.nBlockHeight, mnlevel);

            LogPrint("masternode","CMasternodePayments::ProcessBlock() - Signing Winner level %d\n", mnlevel);

            if (!newWinner.Sign(keyMasternode, pubKeyMasternode))
                continue;

            LogPrint("masternode","CMasternodePayments::ProcessBlock() - AddWinningMasternode level %d\n", mnlevel);

            if (!AddWinningMasternode(newWinner))
                continue;

            winners.emplace_back(newWinner);
        }

    if(winners.empty())
        return false;

    for (CMasternodePaymentWinner& winner : winners) {
        winner.Relay();
    }

    nLastBlockHeight = nBlockHeight;

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

    if (pmn != NULL) {
        std::string strMessage = vinMasternode.prevout.ToStringShort() + std::to_string(nBlockHeight) + payee.ToString();

        std::string errorMessage = "";
        if (!obfuScationSigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage)) {
            return error("CMasternodePaymentWinner::SignatureValid() - Got bad Masternode address signature %s\n", vinMasternode.prevout.hash.ToString());
        }

        return true;
}

    return false;
}

void CMasternodePayments::Sync(CNode* node, int nCountNeeded)
{
    LOCK(cs_mapMasternodePayeeVotes);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL) return;
        nHeight = chainActive.Tip()->nHeight;
    }

    auto mn_counts = mnodeman.CountEnabledByLevels();
    unsigned max_mn_count = 0u;

    for(auto& count : mn_counts) {
        max_mn_count = std::max(max_mn_count, unsigned(count.second * 1.25));
        count.second = unsigned(count.second * 1.25) + 1;
    }
    if(max_mn_count > nCountNeeded) max_mn_count = nCountNeeded;

    int nInvCount = 0;

    for(const auto& vote : mapMasternodePayeeVotes) {
        const CMasternodePaymentWinner& winner = vote.second;

        bool push = winner.nBlockHeight >= nHeight - mn_counts[winner.payeeLevel]
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


int CMasternodePayments::GetOldestBlock()
{
    LOCK(cs_mapMasternodeBlocks);

    int nOldestBlock = std::numeric_limits<int>::max();

    std::map<int, CMasternodeBlockPayees>::iterator it = mapMasternodeBlocks.begin();
    while (it != mapMasternodeBlocks.end()) {
        if ((*it).first < nOldestBlock) {
            nOldestBlock = (*it).first;
        }
        it++;
    }

    return nOldestBlock;
}


int CMasternodePayments::GetNewestBlock()
{
    LOCK(cs_mapMasternodeBlocks);

    int nNewestBlock = 0;

    std::map<int, CMasternodeBlockPayees>::iterator it = mapMasternodeBlocks.begin();
    while (it != mapMasternodeBlocks.end()) {
        if ((*it).first > nNewestBlock) {
            nNewestBlock = (*it).first;
        }
        it++;
    }

    return nNewestBlock;
}
