// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The XDNA Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeman.h"
#include "activemasternode.h"
#include "addrman.h"
#include "masternode.h"
#include "obfuscation.h"
#include "spork.h"
#include "util.h"
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>

/** Masternode manager */
CMasternodeMan mnodeman;

struct CompareLastPaid {
    bool operator()(const pair<int64_t, CTxIn>& t1,
        const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};

struct CompareScoreTxIn {
    bool operator()(const pair<int64_t, CTxIn>& t1,
        const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};

struct CompareScoreMN {
    bool operator()(const pair<int64_t, CMasternode>& t1,
        const pair<int64_t, CMasternode>& t2) const
    {
        return t1.first < t2.first;
    }
};

//
// CMasternodeDB
//

CMasternodeDB::CMasternodeDB()
{
    pathMN = GetDataDir() / "mncache.dat";
    strMagicMessage = "MasternodeCache";
}

bool CMasternodeDB::Write(const CMasternodeMan& mnodemanToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssMasternodes(SER_DISK, CLIENT_VERSION);
    ssMasternodes << strMagicMessage;                   // masternode cache file specific magic message
    ssMasternodes << FLATDATA(Params().MessageStart()); // network specific magic number
    ssMasternodes << mnodemanToSave;
    uint256 hash = Hash(ssMasternodes.begin(), ssMasternodes.end());
    ssMasternodes << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fopen(pathMN.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathMN.string());

    // Write and commit header, data
    try {
        fileout << ssMasternodes;
    } catch (std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    //    FileCommit(fileout);
    fileout.fclose();

    LogPrintf("Written info to mncache.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrintf("  %s\n", mnodemanToSave.ToString());

    return true;
}

CMasternodeDB::ReadResult CMasternodeDB::Read(CMasternodeMan& mnodemanToLoad, bool fDryRun)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fopen(pathMN.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathMN.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathMN);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    vector<unsigned char> vchData;
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

    CDataStream ssMasternodes(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssMasternodes.begin(), ssMasternodes.end());
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (masternode cache file specific magic message) and ..

        ssMasternodes >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid masternode cache magic message", __func__);
            return IncorrectMagicMessage;
        }

        // de-serialize file header (network specific magic number) and ..
        ssMasternodes >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }
        // de-serialize data into CMasternodeMan object
        ssMasternodes >> mnodemanToLoad;
    } catch (std::exception& e) {
        mnodemanToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrintf("Loaded info from mncache.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrintf("  %s\n", mnodemanToLoad.ToString());
    if (!fDryRun) {
        LogPrintf("Masternode manager - cleaning....\n");
        mnodemanToLoad.CheckAndRemove(true);
        LogPrintf("Masternode manager - result:\n");
        LogPrintf("  %s\n", mnodemanToLoad.ToString());
    }

    return Ok;
}

void DumpMasternodes()
{
    int64_t nStart = GetTimeMillis();

    CMasternodeDB mndb;
    CMasternodeMan tempMnodeman;

    LogPrintf("Verifying mncache.dat format...\n");
    CMasternodeDB::ReadResult readResult = mndb.Read(tempMnodeman, true);
    // there was an error and it was not an error on file opening => do not proceed
    if (readResult == CMasternodeDB::FileError)
        LogPrintf("Missing masternode cache file - mncache.dat, will try to recreate\n");
    else if (readResult != CMasternodeDB::Ok) {
        LogPrintf("Error reading mncache.dat: ");
        if (readResult == CMasternodeDB::IncorrectFormat)
            LogPrintf("magic is ok but data has invalid format, will try to recreate\n");
        else {
            LogPrintf("file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrintf("Writting info to mncache.dat...\n");
    mndb.Write(mnodeman);

    LogPrintf("Masternode dump finished  %dms\n", GetTimeMillis() - nStart);
}

CMasternodeMan::CMasternodeMan()
{
    nDsqCount = 0;
}

CValidationState CMasternodeMan::GetInputCheckingTx(const CTxIn& vin, CMutableTransaction& tx)
{
    CValidationState state;
    CAmount          deposit;

    if(!CMasternode::IsDepositCoins(vin, deposit)) {
        state.Invalid(false, 0, "MN input checking tx: invalid vin amount");
        return state;
    }

    CMutableTransaction chk_tx;

    chk_tx.vin.push_back(vin);
//    chk_tx.vout.push_back(CTxOut(9999.99 * COIN, obfuScationPool.collateralPubKey));
    chk_tx.vout.push_back(CTxOut(deposit - 0.01 * COIN, obfuScationPool.collateralPubKey));

    tx = chk_tx;

    return state;
}

bool CMasternodeMan::Add(const CMasternode& mn)
{
    LOCK(cs);

    if(!mn.IsEnabled())
        return false;

    if(Find(mn.vin))
        return false;

    LogPrint("masternode", "CMasternodeMan: Adding new Masternode %s - %i now\n", mn.vin.prevout.hash.ToString(), size() + 1);
    vMasternodes.push_back(mn);
    return true;
}

std::vector<CMasternode> CMasternodeMan::GetFullMasternodeMap()
{
    LOCK(cs);
    return vMasternodes;
}

void CMasternodeMan::AskForMN(CNode* pnode, CTxIn& vin)
{
    std::map<COutPoint, int64_t>::iterator i = mWeAskedForMasternodeListEntry.find(vin.prevout);
    if (i != mWeAskedForMasternodeListEntry.end()) {
        int64_t t = (*i).second;
        if (GetTime() < t) return; // we've asked recently
    }

    // ask for the mnb info once from the node that sent mnp

    LogPrint("masternode", "CMasternodeMan::AskForMN - Asking node for missing entry, vin: %s\n", vin.prevout.hash.ToString());
    pnode->PushMessage("dseg", vin);
    int64_t askAgain = GetTime() + MASTERNODE_MIN_MNP_SECONDS;
    mWeAskedForMasternodeListEntry[vin.prevout] = askAgain;
}

void CMasternodeMan::Check()
{
    LOCK(cs);

    BOOST_FOREACH (CMasternode& mn, vMasternodes) {
        mn.Check();
    }
}

void CMasternodeMan::CheckAndRemove(bool forceExpiredRemoval)
{
    Check();

    LOCK(cs);

    //remove inactive and outdated
    vector<CMasternode>::iterator it = vMasternodes.begin();
    while (it != vMasternodes.end()) {
        if ((*it).activeState == CMasternode::MASTERNODE_REMOVE ||
            (*it).activeState == CMasternode::MASTERNODE_VIN_SPENT ||
            (forceExpiredRemoval && (*it).activeState == CMasternode::MASTERNODE_EXPIRED) ||
            (*it).protocolVersion < masternodePayments.GetMinMasternodePaymentsProto()) {
            LogPrint("masternode", "CMasternodeMan: Removing inactive Masternode %s - %i now\n", (*it).vin.prevout.hash.ToString(), size() - 1);

            //erase all of the broadcasts we've seen from this vin
            // -- if we missed a few pings and the node was removed, this will allow is to get it back without them
            //    sending a brand new mnb
            map<uint256, CMasternodeBroadcast>::iterator it3 = mapSeenMasternodeBroadcast.begin();
            while (it3 != mapSeenMasternodeBroadcast.end()) {
                if ((*it3).second.vin == (*it).vin) {
                    masternodeSync.mapSeenSyncMNB.erase((*it3).first);
                    mapSeenMasternodeBroadcast.erase(it3++);
                } else {
                    ++it3;
                }
            }

            // allow us to ask for this masternode again if we see another ping
            map<COutPoint, int64_t>::iterator it2 = mWeAskedForMasternodeListEntry.begin();
            while (it2 != mWeAskedForMasternodeListEntry.end()) {
                if ((*it2).first == (*it).vin.prevout) {
                    mWeAskedForMasternodeListEntry.erase(it2++);
                } else {
                    ++it2;
                }
            }

            it = vMasternodes.erase(it);
        } else {
            ++it;
        }
    }

    // check who's asked for the Masternode list
    map<CNetAddr, int64_t>::iterator it1 = mAskedUsForMasternodeList.begin();
    while (it1 != mAskedUsForMasternodeList.end()) {
        if ((*it1).second < GetTime()) {
            mAskedUsForMasternodeList.erase(it1++);
        } else {
            ++it1;
        }
    }

    // check who we asked for the Masternode list
    it1 = mWeAskedForMasternodeList.begin();
    while (it1 != mWeAskedForMasternodeList.end()) {
        if ((*it1).second < GetTime()) {
            mWeAskedForMasternodeList.erase(it1++);
        } else {
            ++it1;
        }
    }

    // check which Masternodes we've asked for
    map<COutPoint, int64_t>::iterator it2 = mWeAskedForMasternodeListEntry.begin();
    while (it2 != mWeAskedForMasternodeListEntry.end()) {
        if ((*it2).second < GetTime()) {
            mWeAskedForMasternodeListEntry.erase(it2++);
        } else {
            ++it2;
        }
    }

    // check who's asked for the winner Masternode list
    map<CNetAddr, int64_t>::iterator it5 = mAskedUsForWinnerMasternodeList.begin();
    while (it5 != mAskedUsForWinnerMasternodeList.end()) {
        if ((*it5).second < GetTime()) {
            mAskedUsForWinnerMasternodeList.erase(it5++);
        } else {
            ++it5;
        }
    }

    // check who we asked for the wineer Masternode list
    it5 = mWeAskedForWinnerMasternodeList.begin();
    while (it5 != mWeAskedForWinnerMasternodeList.end()) {
        if ((*it5).second < GetTime()) {
            mWeAskedForWinnerMasternodeList.erase(it5++);
        } else {
            ++it5;
        }
    }

    // remove expired mapSeenMasternodeBroadcast
    map<uint256, CMasternodeBroadcast>::iterator it3 = mapSeenMasternodeBroadcast.begin();
    while (it3 != mapSeenMasternodeBroadcast.end()) {
        if ((*it3).second.lastPing.sigTime < GetTime() - (MASTERNODE_REMOVAL_SECONDS * 2)) {
            mapSeenMasternodeBroadcast.erase(it3++);
            masternodeSync.mapSeenSyncMNB.erase((*it3).second.GetHash());
        } else {
            ++it3;
        }
    }

    // remove expired mapSeenMasternodePing
    map<uint256, CMasternodePing>::iterator it4 = mapSeenMasternodePing.begin();
    while (it4 != mapSeenMasternodePing.end()) {
        if ((*it4).second.sigTime < GetTime() - (MASTERNODE_REMOVAL_SECONDS * 2)) {
            mapSeenMasternodePing.erase(it4++);
        } else {
            ++it4;
        }
    }
}

void CMasternodeMan::Clear()
{
    LOCK(cs);
    vMasternodes.clear();
    mAskedUsForMasternodeList.clear();
    mWeAskedForMasternodeList.clear();
    mWeAskedForMasternodeListEntry.clear();
    mAskedUsForWinnerMasternodeList.clear();
    mWeAskedForWinnerMasternodeList.clear();
    mapSeenMasternodeBroadcast.clear();
    mapSeenMasternodePing.clear();
    nDsqCount = 0;
}

int CMasternodeMan::size(unsigned mnlevel)
{
    auto check_level = mnlevel != CMasternode::LevelValue::UNSPECIFIED;

    return std::count_if(vMasternodes.begin(), vMasternodes.end(), [=](CMasternode& mn){

        if(check_level && mnlevel != mn.Level())
            return false;

        return true;
    });
}

int CMasternodeMan::stable_size(unsigned mnlevel)
{
    int nStable_size = 0;
    int nMinProtocol = ActiveProtocol();
    int64_t nMasternode_Min_Age = GetSporkValue(SPORK_6_MN_WINNER_MINIMUM_AGE);
    int64_t nMasternode_Age = 0;

    auto check_level = mnlevel != CMasternode::LevelValue::UNSPECIFIED;

    for(auto& mn : vMasternodes) {

        if(mn.protocolVersion < nMinProtocol)
            continue; // Skip obsolete versions

        if(check_level && mnlevel != mn.Level())
            continue;

        if(IsSporkActive(SPORK_4_MASTERNODE_PAYMENT_ENFORCEMENT)) {

            nMasternode_Age = GetAdjustedTime() - mn.sigTime;

            if(nMasternode_Age < nMasternode_Min_Age)
                continue; // Skip masternodes younger than (default) 8000 sec (MUST be > MASTERNODE_REMOVAL_SECONDS)
        }

        mn.Check();

        if(!mn.IsEnabled())
            continue; // Skip not-enabled masternodes

        ++nStable_size;
    }

    return nStable_size;
}

unsigned CMasternodeMan::CountEnabled(unsigned mnlevel, int protocolVersion)
{
    if(protocolVersion == -1)
        protocolVersion = masternodePayments.GetMinMasternodePaymentsProto();

    auto check_level = mnlevel != CMasternode::LevelValue::UNSPECIFIED;

    return std::count_if(vMasternodes.begin(), vMasternodes.end(), [=](CMasternode& mn){

        mn.Check();

        if(check_level && mnlevel != mn.Level())
            return false;

        return mn.protocolVersion >= protocolVersion && mn.IsEnabled();
    });
}

std::map<unsigned, unsigned> CMasternodeMan::CountEnabledByLevels(int protocolVersion)
{
    if(protocolVersion == -1)
        protocolVersion = masternodePayments.GetMinMasternodePaymentsProto();

    std::map<unsigned, unsigned> result;

    for(unsigned l = CMasternode::LevelValue::MIN; l <= CMasternode::LevelValue::MAX; ++l)
        result.emplace(l, 0u);

    for(auto& mn : vMasternodes)
    {
        mn.Check();

        bool enabled = mn.protocolVersion >= protocolVersion && mn.IsEnabled();

        if(!enabled)
            continue;

        ++result[mn.Level()];
    };

    return result;
}

void CMasternodeMan::CountNetworks(int protocolVersion, int& ipv4, int& ipv6, int& onion)
{
    protocolVersion = protocolVersion == -1 ? masternodePayments.GetMinMasternodePaymentsProto() : protocolVersion;

    BOOST_FOREACH (CMasternode& mn, vMasternodes) {
        mn.Check();
        std::string strHost;
        int port;
        SplitHostPort(mn.addr.ToString(), port, strHost);
        CNetAddr node = CNetAddr(strHost, false);
        int nNetwork = node.GetNetwork();
        switch (nNetwork) {
            case 1 :
                ipv4++;
                break;
            case 2 :
                ipv6++;
                break;
            case 3 :
                onion++;
                break;
        }
    }
}

bool CMasternodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

    if (Params().NetworkID() == CBaseChainParams::MAIN) {
        if (!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForMasternodeList.find(pnode->addr);
            if (it != mWeAskedForMasternodeList.end()) {
                if (GetTime() < (*it).second) {
                    LogPrint("masternode", "dseg - we already asked peer %i for the list; skipping...\n", pnode->GetId());
                    return false;
                }
            }
        }
    }

    pnode->PushMessage("dseg", CTxIn());
    int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
    mWeAskedForMasternodeList[pnode->addr] = askAgain;
    return true;
}

bool CMasternodeMan::WinnersUpdate(CNode* node)
{
    LOCK(cs);

    if (Params().NetworkID() == CBaseChainParams::MAIN) {
        if (!(node->addr.IsRFC1918() || node->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForWinnerMasternodeList.find(node->addr);
            if (it != mWeAskedForWinnerMasternodeList.end()) {
                if (GetTime() < (*it).second) {
                    LogPrint("masternode", "mnget - we already asked peer %i for the winners list; skipping...\n", node->GetId());
                    return false;
                }
            }
        }
    }

    auto mn_counts = mnodeman.CountEnabledByLevels();

    unsigned max_mn_count = 0u;

    for(const auto& count : mn_counts)
        max_mn_count = std::max(max_mn_count, count.second);

    node->PushMessage("mnget", max_mn_count);
    int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
    mWeAskedForWinnerMasternodeList[node->addr] = askAgain;
    return true;
}

CMasternode* CMasternodeMan::Find(const CScript& payee)
{
    LOCK(cs);
    CScript payee2;

    BOOST_FOREACH (CMasternode& mn, vMasternodes) {
        payee2 = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());
        if (payee2 == payee)
            return &mn;
    }
    return nullptr;
}

CMasternode* CMasternodeMan::Find(const CTxIn& vin)
{
    LOCK(cs);

    BOOST_FOREACH (CMasternode& mn, vMasternodes) {
        if (mn.vin.prevout == vin.prevout)
            return &mn;
    }
    return nullptr;
}


CMasternode* CMasternodeMan::Find(const CPubKey& pubKeyMasternode)
{
    LOCK(cs);

    BOOST_FOREACH (CMasternode& mn, vMasternodes) {
        if (mn.pubKeyMasternode == pubKeyMasternode)
            return &mn;
    }
    return nullptr;
}

CMasternode* CMasternodeMan::Find(const CService& service)
{
    LOCK(cs);

    for(auto& mn : vMasternodes) {
        if (mn.addr == service)
            return &mn;
    }

    return nullptr;
}

//
// Deterministically select the oldest/best masternode to pay on the network
//
CMasternode* CMasternodeMan::GetNextMasternodeInQueueForPayment(int nBlockHeight, unsigned mnlevel, bool fFilterSigTime, unsigned& nCount)
{
    LOCK(cs);

    std::vector<std::pair<int64_t, CTxIn> > vecMasternodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */

    int nMnCount = CountEnabled(mnlevel);

    for(CMasternode& mn : vMasternodes) {

        if(mn.Level() != mnlevel)
            continue;

        //check protocol version
        if(mn.protocolVersion < masternodePayments.GetMinMasternodePaymentsProto())
            continue;

        mn.Check();

        if(!mn.IsEnabled())
            continue;

        //it's in the list -- so let's skip it
        if(masternodePayments.IsScheduled(mn, nMnCount, nBlockHeight))
            continue;

        //it's too new, wait for a cycle
        if(fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime())
            continue;

        //make sure it has as many confirmations as there are masternodes
        if(mn.GetMasternodeInputAge() < nMnCount)
            continue;

        vecMasternodeLastPaid.emplace_back(mn.SecondsSincePayment(), mn.vin);
    }

    nCount = vecMasternodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if (fFilterSigTime && nCount < nMnCount / 3)
        return GetNextMasternodeInQueueForPayment(nBlockHeight, mnlevel, false, nCount);

    // Sort them high to low
    sort(vecMasternodeLastPaid.rbegin(), vecMasternodeLastPaid.rend(), CompareLastPaid());

    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the scheduled blocks, allowing for double payments very rarely

    int     nCountTenth = nMnCount / 10;
    uint256 nHigh = 0;

    CMasternode* pBestMasternode = nullptr;

    for(const auto& s : vecMasternodeLastPaid) {

        CMasternode* pmn = Find(s.second);

        if(!pmn)
            continue;

        uint256 n = pmn->CalculateScore(1, nBlockHeight - 100);

        if(n > nHigh) {
            nHigh = n;
            pBestMasternode = pmn;
        }

        if(--nCountTenth > 0)
            break;
    }

    return pBestMasternode;
}

CMasternode* CMasternodeMan::FindRandomNotInVec(unsigned mnlevel, std::vector<CTxIn>& vecToExclude, int protocolVersion)
{
    LOCK(cs);

    protocolVersion = protocolVersion == -1 ? masternodePayments.GetMinMasternodePaymentsProto() : protocolVersion;

    int nCountEnabled = CountEnabled(mnlevel, protocolVersion);
    LogPrint("masternode", "CMasternodeMan::FindRandomNotInVec - nCountEnabled - vecToExclude.size() %d\n", nCountEnabled - vecToExclude.size());
    if (nCountEnabled - vecToExclude.size() < 1) return nullptr;

    int rand = GetRandInt(nCountEnabled - vecToExclude.size());
    LogPrint("masternode", "CMasternodeMan::FindRandomNotInVec - rand %d\n", rand);
    bool found;

    for (CMasternode& mn : vMasternodes) {

        if(mnlevel != CMasternode::LevelValue::UNSPECIFIED && mn.Level() != mnlevel)
            continue;

        if (mn.protocolVersion < protocolVersion || !mn.IsEnabled()) continue;
        found = false;
        for (CTxIn& usedVin : vecToExclude) {
            if (mn.vin.prevout == usedVin.prevout) {
                found = true;
                break;
            }
        }
        if (found) continue;
        if (--rand < 1) {
            return &mn;
        }
    }

    return nullptr;
}

CMasternode* CMasternodeMan::GetCurrentMasterNode(unsigned mnlevel, int mod, int64_t nBlockHeight, int minProtocol)
{
    int64_t score = 0;
    CMasternode* winner = nullptr;

    auto check_mnlevel = mnlevel != CMasternode::LevelValue::UNSPECIFIED;

    // scan for winner
    for(CMasternode& mn : vMasternodes) {
        mn.Check();

        if(check_mnlevel && mn.Level() != mnlevel)
            continue;

        if(mn.protocolVersion < minProtocol || !mn.IsEnabled())
            continue;

        // calculate the score for each Masternode
        uint256 n  = mn.CalculateScore(mod, nBlockHeight);
        int64_t n2 = n.GetCompact(false);

        // determine the winner
        if (n2 > score) {
            score = n2;
            winner = &mn;
        }
    }

    return winner;
}

int CMasternodeMan::GetMasternodeRank(const CTxIn& vin, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<pair<int64_t, CTxIn> > vecMasternodeScores;
    int64_t nMasternode_Min_Age = GetSporkValue(SPORK_6_MN_WINNER_MINIMUM_AGE);
    int64_t nMasternode_Age = 0;

    //make sure we know about this block
    uint256 hash = 0;
    if(!GetBlockHash(hash, nBlockHeight))
        return -1;

    // scan for winner
    for(CMasternode& mn : vMasternodes) {
        if(mn.protocolVersion < minProtocol) {
            LogPrintf("Skipping Masternode with obsolete version %d\n", mn.protocolVersion);
            continue;
        }

        if(IsSporkActive(SPORK_4_MASTERNODE_PAYMENT_ENFORCEMENT)) {

            nMasternode_Age = GetAdjustedTime() - mn.sigTime;

            if(nMasternode_Age < nMasternode_Min_Age) {
                if(fDebug)
                    LogPrintf("Skipping just activated Masternode. Age: %ld\n", nMasternode_Age);

                continue;
            }
        }

        if(fOnlyActive) {

            mn.Check();
            if(!mn.IsEnabled())
                continue;

        }

        uint256 n  = mn.CalculateScore(1, nBlockHeight);
        int64_t n2 = n.GetCompact(false);

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareScoreTxIn());

    int rank = 0;

    for(auto& s : vecMasternodeScores) {
        ++rank;
        if(s.second.prevout == vin.prevout)
            return rank;
    }

    return -1;
}

std::vector<pair<int, CMasternode> > CMasternodeMan::GetMasternodeRanks(int64_t nBlockHeight, int minProtocol)
{
    std::vector<pair<int64_t, CMasternode> > vecMasternodeScores;
    std::vector<pair<int, CMasternode> > vecMasternodeRanks;

    //make sure we know about this block
    uint256 hash = 0;
    if (!GetBlockHash(hash, nBlockHeight)) return vecMasternodeRanks;

    // scan for winner
    BOOST_FOREACH (CMasternode& mn, vMasternodes) {
        mn.Check();

        if (mn.protocolVersion < minProtocol) continue;

        if (!mn.IsEnabled()) {
            vecMasternodeScores.push_back(make_pair(9999, mn));
            continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        int64_t n2 = n.GetCompact(false);

        vecMasternodeScores.push_back(make_pair(n2, mn));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareScoreMN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CMasternode) & s, vecMasternodeScores) {
        rank++;
        vecMasternodeRanks.push_back(make_pair(rank, s.second));
    }

    return vecMasternodeRanks;
}

CMasternode* CMasternodeMan::GetMasternodeByRank(int nRank, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<pair<int64_t, CTxIn> > vecMasternodeScores;

    // scan for winner
    BOOST_FOREACH (CMasternode& mn, vMasternodes) {
        if (mn.protocolVersion < minProtocol) continue;
        if (fOnlyActive) {
            mn.Check();
            if (!mn.IsEnabled()) continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        int64_t n2 = n.GetCompact(false);

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareScoreTxIn());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CTxIn) & s, vecMasternodeScores) {
        rank++;
        if (rank == nRank) {
            return Find(s.second);
        }
    }

    return nullptr;
}

void CMasternodeMan::ProcessMasternodeConnections()
{
    //we don't care about this for regtest
    if (Params().NetworkID() == CBaseChainParams::REGTEST) return;

    LOCK(cs_vNodes);
    BOOST_FOREACH (CNode* pnode, vNodes) {
        if (pnode->fObfuScationMaster) {
            if (obfuScationPool.pSubmittedToMasternode && pnode->addr == obfuScationPool.pSubmittedToMasternode->addr)
                continue;

            LogPrintf("Closing Masternode connection peer=%i \n", pnode->GetId());
            pnode->fObfuScationMaster = false;
            pnode->Release();
        }
    }
}

void CMasternodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (fLiteMode) return; //disable all Obfuscation/Masternode related functionality
    if (!masternodeSync.IsBlockchainSynced()) return;

    LOCK(cs_process_message);

    if (strCommand == "mnb") { //Masternode Broadcast
        CMasternodeBroadcast mnb;
        vRecv >> mnb;

        auto pmn = mnodeman.Find(mnb.addr);

        if(pmn && pmn->vin != mnb.vin)
        {
            pmn->Check(true);

            if(pmn->IsEnabled())
            {
                LogPrintf("mnb - More than one vin used for single IP address\n");
                Misbehaving(pfrom->GetId(), 100);
                return;
            }
        }

        if (mapSeenMasternodeBroadcast.count(mnb.GetHash())) { //seen
            masternodeSync.AddedMasternodeList(mnb.GetHash());
            return;
        }

        mapSeenMasternodeBroadcast.insert(make_pair(mnb.GetHash(), mnb));

        int nDoS = 0;
        if (!mnb.CheckAndUpdate(nDoS)) {
            if (nDoS > 0)
                Misbehaving(pfrom->GetId(), nDoS);

            //failed
            return;
        }

        // make sure the vout that was signed is related to the transaction that spawned the Masternode
        //  - this is expensive, so it's only done once per Masternode
        if (!obfuScationSigner.IsVinAssociatedWithPubkey(mnb.vin, mnb.pubKeyCollateralAddress)) {
            LogPrintf("mnb - Got mismatched pubkey and vin\n");
            Misbehaving(pfrom->GetId(), 33);
            return;
        }

        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckObfuScationPool()
        if (mnb.CheckInputsAndAdd(nDoS)) {
            // use this as a peer
            addrman.Add(CAddress(mnb.addr), pfrom->addr, 2 * 60 * 60);
            masternodeSync.AddedMasternodeList(mnb.GetHash());
        } else {
            LogPrintf("mnb - Rejected Masternode entry %s\n", mnb.vin.prevout.hash.ToString());
            if (nDoS > 0) {
                Misbehaving(pfrom->GetId(), nDoS);
                return;
            }
        }
    }

    else if (strCommand == "mnp") { //Masternode Ping
        CMasternodePing mnp;
        vRecv >> mnp;

        LogPrint("masternode", "mnp - Masternode ping, vin: %s\n", mnp.vin.prevout.hash.ToString());

        if (mapSeenMasternodePing.count(mnp.GetHash()))  //seen
            return;

        mapSeenMasternodePing.insert(make_pair(mnp.GetHash(), mnp));

        int nDoS = 0;
        if (mnp.CheckAndUpdate(nDoS)) return;

        if (nDoS > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDoS);
        } else {
            // if nothing significant failed, search existing Masternode list
            CMasternode* pmn = Find(mnp.vin);
            // if it's known, don't ask for the mnb, just return
            if (pmn) return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a masternode entry once
        AskForMN(pfrom, mnp.vin);

    }

    else if (strCommand == "dseg") { //Get Masternode list or specific entry

        CTxIn vin;
        vRecv >> vin;

        if (vin == CTxIn()) { //only should ask for this once
            //local network
            bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

            if (!isLocal && Params().NetworkID() == CBaseChainParams::MAIN) {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForMasternodeList.find(pfrom->addr);
                if (i != mAskedUsForMasternodeList.end()) {
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrintf("dseg - peer already asked me for the list\n");
                        return;
                    }
                }
                int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
                mAskedUsForMasternodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok


        int nInvCount = 0;

        BOOST_FOREACH (CMasternode& mn, vMasternodes) {
            if (mn.addr.IsRFC1918()) continue; //local network

            if (mn.IsEnabled()) {
                LogPrint("masternode", "dseg - Sending Masternode entry - %s \n", mn.vin.prevout.hash.ToString());
                if (vin == CTxIn() || vin == mn.vin) {
                    CMasternodeBroadcast mnb = CMasternodeBroadcast(mn);
                    uint256 hash = mnb.GetHash();
                    pfrom->PushInventory(CInv(MSG_MASTERNODE_ANNOUNCE, hash));
                    nInvCount++;

                    if (!mapSeenMasternodeBroadcast.count(hash)) mapSeenMasternodeBroadcast.insert(make_pair(hash, mnb));

                    if (vin == mn.vin) {
                        LogPrint("masternode", "dseg - Sent 1 Masternode entry to peer %i\n", pfrom->GetId());
                        return;
                    }
                }
            }
        }

        if (vin == CTxIn()) {
            pfrom->PushMessage("ssc", MASTERNODE_SYNC_LIST, nInvCount);
            LogPrintf("dseg - Sent %d Masternode entries to %s\n", nInvCount, pfrom->addr.ToString());
        }
    }

    else if (strCommand == "mnget") { //Get winnign Masternode list

        int nCountNeeded;
        vRecv >> nCountNeeded;

        bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

        if (!isLocal && Params().NetworkID() == CBaseChainParams::MAIN) {
            std::map<CNetAddr, int64_t>::iterator i = mAskedUsForWinnerMasternodeList.find(pfrom->addr);
            if (i != mAskedUsForWinnerMasternodeList.end()) {
                int64_t t = (*i).second;
                if (GetTime() < t) {
                    Misbehaving(pfrom->GetId(), 34);
                    LogPrintf("mnget - peer already asked me for the list\n");
                    return;
                }
            }
            int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
            mAskedUsForWinnerMasternodeList[pfrom->addr] = askAgain;
        }

        masternodePayments.Sync(pfrom, nCountNeeded);
        LogPrint("mnpayments", "mnget - Sent Masternode winners to peer %i\n", pfrom->GetId());
    }

}

void CMasternodeMan::Remove(CTxIn vin)
{
    LOCK(cs);

    vector<CMasternode>::iterator it = vMasternodes.begin();
    while (it != vMasternodes.end()) {
        if ((*it).vin == vin) {
            LogPrint("masternode", "CMasternodeMan: Removing Masternode %s - %i now\n", (*it).vin.prevout.hash.ToString(), size() - 1);
            vMasternodes.erase(it);
            break;
        }
        ++it;
    }
}

void CMasternodeMan::UpdateMasternodeList(CMasternodeBroadcast mnb)
{
    LOCK(cs);
    mapSeenMasternodePing.insert(std::make_pair(mnb.lastPing.GetHash(), mnb.lastPing));
    mapSeenMasternodeBroadcast.insert(std::make_pair(mnb.GetHash(), mnb));

    LogPrintf("CMasternodeMan::UpdateMasternodeList -- masternode=%s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());

    CMasternode* pmn = Find(mnb.vin);

    if (!pmn) {
        if (Add(CMasternode{mnb}))
            masternodeSync.AddedMasternodeList(mnb.GetHash());

    } else if (pmn->UpdateFromNewBroadcast(mnb)) {
        masternodeSync.AddedMasternodeList(mnb.GetHash());
    }
}

std::string CMasternodeMan::ToString() const
{
    std::ostringstream info;

    info << "Masternodes: " << vMasternodes.size() << ", peers who asked us for Masternode list: " << (int)mAskedUsForMasternodeList.size() << ", peers we asked for Masternode list: " << (int)mWeAskedForMasternodeList.size() << ", entries in Masternode list we asked for: " << mWeAskedForMasternodeListEntry.size() << ", nDsqCount: " << nDsqCount;

    return info.str();
}
