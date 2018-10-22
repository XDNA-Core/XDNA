// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The XDNA Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode.h"
#include "addrman.h"
#include "masternodeman.h"
#include "obfuscation.h"
#include "sync.h"
#include "util.h"

#include <boost/lexical_cast.hpp>

// cache block hashes as we calculate them
std::map<int64_t, uint256> mapCacheBlockHashes;

//Get the last hash that matches the modulus given. Processed in reverse order
bool GetBlockHash(uint256& hash, int nBlockHeight)
{
    auto active_tip = chainActive.Tip();

    if(!active_tip)
        return false;

    if(nBlockHeight <= 0)
        nBlockHeight = active_tip->nHeight;

    if(active_tip->nHeight < nBlockHeight)
        return false;

    const auto& cached_hash = mapCacheBlockHashes.find(nBlockHeight);

    if(cached_hash != mapCacheBlockHashes.cend()) {
        hash = cached_hash->second;
        return true;
    }

    for(auto BlockReading = active_tip; BlockReading; BlockReading = BlockReading->pprev) {

        if(BlockReading->nHeight != nBlockHeight)
            continue;

        auto ins_res = mapCacheBlockHashes.emplace(nBlockHeight, BlockReading->GetBlockHash());

        hash = ins_res.first->second;

        return true;
    }

    return false;
}

CMasternode::CMasternode()
{
    LOCK(cs);
    vin = CTxIn();
    addr = CService();
    pubKeyCollateralAddress = CPubKey();
    pubKeyMasternode = CPubKey();
    sig = std::vector<unsigned char>();
    activeState = MASTERNODE_ENABLED;
    deposit = 0 * COIN;
    sigTime = GetAdjustedTime();
    lastPing = CMasternodePing();
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    protocolVersion = PROTOCOL_VERSION;
    nLastDsq = 0;
    lastTimeChecked = 0;
}

CMasternode::CMasternode(const CMasternode& other)
{
    LOCK(cs);
    vin = other.vin;
    addr = other.addr;
    pubKeyCollateralAddress = other.pubKeyCollateralAddress;
    pubKeyMasternode = other.pubKeyMasternode;
    sig = other.sig;
    activeState = other.activeState;
    deposit = other.deposit;
    sigTime = other.sigTime;
    lastPing = other.lastPing;
    cacheInputAge = other.cacheInputAge;
    cacheInputAgeBlock = other.cacheInputAgeBlock;
    unitTest = other.unitTest;
    allowFreeTx = other.allowFreeTx;
    protocolVersion = other.protocolVersion;
    nLastDsq = other.nLastDsq;
    lastTimeChecked = 0;
}

CMasternode::CMasternode(const CMasternodeBroadcast& mnb)
{
    LOCK(cs);
    vin = mnb.vin;
    addr = mnb.addr;
    pubKeyCollateralAddress = mnb.pubKeyCollateralAddress;
    pubKeyMasternode = mnb.pubKeyMasternode;
    sig = mnb.sig;

    if(IsDepositCoins(mnb.vin, deposit))
        activeState = MASTERNODE_ENABLED;
    else
    {
        deposit = 0u;
        activeState = MASTERNODE_REMOVE;
    }

    sigTime = mnb.sigTime;
    lastPing = mnb.lastPing;
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    protocolVersion = mnb.protocolVersion;
    nLastDsq = mnb.nLastDsq;
    lastTimeChecked = 0;
}

//
// When a new masternode broadcast is sent, update our information
//
bool CMasternode::UpdateFromNewBroadcast(CMasternodeBroadcast& mnb)
{
    if(mnb.sigTime <= sigTime)
        return false;

    pubKeyMasternode = mnb.pubKeyMasternode;
    pubKeyCollateralAddress = mnb.pubKeyCollateralAddress;
    sigTime = mnb.sigTime;
    sig = mnb.sig;
    protocolVersion = mnb.protocolVersion;
    addr = mnb.addr;
    lastTimeChecked = 0;
    int nDoS = 0;
    if (mnb.lastPing == CMasternodePing() || (mnb.lastPing != CMasternodePing() && mnb.lastPing.CheckAndUpdate(nDoS, false))) {
        lastPing = mnb.lastPing;
        mnodeman.mapSeenMasternodePing.insert(make_pair(lastPing.GetHash(), lastPing));
    }

    return true;
}

//
// Deterministically calculate a given "score" for a Masternode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
uint256 CMasternode::CalculateScore(int mod, int64_t nBlockHeight)
{
    if (chainActive.Tip() == NULL) return 0;

    uint256 hash = 0;
    uint256 aux = vin.prevout.hash + vin.prevout.n;

    if (!GetBlockHash(hash, nBlockHeight)) {
        LogPrintf("CalculateScore ERROR - nHeight %d - Returned 0\n", nBlockHeight);
        return 0;
    }

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << hash;
    uint256 hash2 = ss.GetHash();

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << hash;
    ss2 << aux;
    uint256 hash3 = ss2.GetHash();

    uint256 r = (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);

    return r;
}

void CMasternode::Check(bool forceCheck)
{
    if(ShutdownRequested())
        return;

    if(!forceCheck && (GetTime() - lastTimeChecked < MASTERNODE_CHECK_SECONDS))
        return;

    lastTimeChecked = GetTime();

    //once spent, stop doing the checks
    if (activeState == MASTERNODE_VIN_SPENT)
        return;

    if (!IsPingedWithin(MASTERNODE_REMOVAL_SECONDS)) {
        activeState = MASTERNODE_REMOVE;
        return;
    }

    if (!IsPingedWithin(MASTERNODE_EXPIRATION_SECONDS)) {
        activeState = MASTERNODE_EXPIRED;
        return;
    }

    if (!unitTest) {

/*
        CMutableTransaction tx = CMutableTransaction();
        CTxOut vout = CTxOut(9999.99 * COIN, obfuScationPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);
*/
        CMutableTransaction tx;

        CValidationState state = CMasternodeMan::GetInputCheckingTx(vin, tx);

        if(!state.IsValid()) {
            activeState = MASTERNODE_VIN_SPENT;
            return;
        }

        {
            TRY_LOCK(cs_main, lockMain);

            if (!lockMain)
                return;

            if (!AcceptableInputs(mempool, state, CTransaction(tx), false, nullptr)) {
                activeState = MASTERNODE_VIN_SPENT;
                return;
            }
        }
    }

    activeState = MASTERNODE_ENABLED; // OK
}

int64_t CMasternode::SecondsSincePayment()
{
    int64_t sec = (GetAdjustedTime() - GetLastPaid());
    int64_t month = 60 * 60 * 24 * 30;

    if (sec < month)
        return sec; //if it's less than 30 days, give seconds

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << sigTime;
    uint256 hash = ss.GetHash();

    // return some deterministic value for unknown/unpaid but force it to be more than 30 days old
    return month + hash.GetCompact(false);
}

int64_t CMasternode::GetLastPaid()
{
    CBlockIndex* pindexPrev = chainActive.Tip();

    if (!pindexPrev)
        return 0;

    CScript mnpayee;
    mnpayee = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << sigTime;
    uint256 hash = ss.GetHash();

    // use a deterministic offset to break a tie -- 1.5 minutes
    int64_t nOffset = hash.GetCompact(false) % 90;

    const CBlockIndex* BlockReading = pindexPrev;

    int nMnCount = mnodeman.CountEnabled(Level()) * 125 / 100;
    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (n >= nMnCount) {
            return 0;
        }
        n++;

        if (masternodePayments.mapMasternodeBlocks.count(BlockReading->nHeight)) {
            /*
                Search for this payee, with at least 2 votes. This will aid in consensus allowing the network
                to converge on the same payees quickly, then keep the same schedule.
            */
            if (masternodePayments.mapMasternodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2)) {
                return BlockReading->nTime - nOffset;
            }
        }

        BlockReading = BlockReading->pprev;
    }

    return 0;
}

bool CMasternode::IsValidNetAddr()
{
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkID() == CBaseChainParams::REGTEST ||
           (IsReachable(addr) && addr.IsRoutable());
}

unsigned CMasternode::Level(CAmount vin_val)
{
    switch(vin_val) {
        case 1000  * COIN: return 1;
        case 3000  * COIN: return 2;
        case 5000 * COIN: return 3;
    }

    return 0;
}

unsigned CMasternode::Level(const CTxIn& vin)
{
    CAmount vin_val;

    if(!IsDepositCoins(vin, vin_val))
        return LevelValue::UNSPECIFIED;

    return Level(vin_val);
}

bool CMasternode::IsDepositCoins(CAmount vin_val)
{
    return Level(vin_val);
}

bool CMasternode::IsDepositCoins(const CTxIn& vin, CAmount& vin_val)
{
    CTransaction prevout_tx;
    uint256      hashBlock = 0;

    bool vin_valid =  GetTransaction(vin.prevout.hash, prevout_tx, hashBlock, true)
                   && (vin.prevout.n < prevout_tx.vout.size());

    if(!vin_valid)
        return false;

    CAmount vin_amount = prevout_tx.vout[vin.prevout.n].nValue;

    if(!IsDepositCoins(vin_amount))
        return false;

    vin_val = vin_amount;
    return true;
}

CMasternodeBroadcast::CMasternodeBroadcast()
{
    vin = CTxIn();
    addr = CService();
    pubKeyCollateralAddress = CPubKey();
    sig = std::vector<unsigned char>();
    activeState = MASTERNODE_ENABLED;
    sigTime = GetAdjustedTime();
    lastPing = CMasternodePing();
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    protocolVersion = PROTOCOL_VERSION;
    nLastDsq = 0;
}

CMasternodeBroadcast::CMasternodeBroadcast(CService newAddr, CTxIn newVin, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyMasternodeNew, int protocolVersionIn)
{
    vin = newVin;
    addr = newAddr;
    pubKeyCollateralAddress = pubKeyCollateralAddressNew;
    pubKeyMasternode = pubKeyMasternodeNew;
    sig = std::vector<unsigned char>();
    activeState = MASTERNODE_ENABLED;
    sigTime = GetAdjustedTime();
    lastPing = CMasternodePing();
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    protocolVersion = protocolVersionIn;
    nLastDsq = 0;
}

CMasternodeBroadcast::CMasternodeBroadcast(const CMasternode& mn)
{
    vin = mn.vin;
    addr = mn.addr;
    pubKeyCollateralAddress = mn.pubKeyCollateralAddress;
    pubKeyMasternode = mn.pubKeyMasternode;
    sig = mn.sig;
    activeState = mn.activeState;
    sigTime = mn.sigTime;
    lastPing = mn.lastPing;
    cacheInputAge = mn.cacheInputAge;
    cacheInputAgeBlock = mn.cacheInputAgeBlock;
    unitTest = mn.unitTest;
    allowFreeTx = mn.allowFreeTx;
    protocolVersion = mn.protocolVersion;
    nLastDsq = mn.nLastDsq;
}

bool CMasternodeBroadcast::Create(std::string strService, std::string strKeyMasternode, std::string strTxHash, std::string strOutputIndex, std::string& strErrorRet, CMasternodeBroadcast& mnbRet, bool fOffline)
{
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyMasternodeNew;
    CKey keyMasternodeNew;

    //need correct blocks to send ping
    if (!fOffline && !masternodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Masternode";
        LogPrintf("CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!obfuScationSigner.GetKeysFromSecret(strKeyMasternode, keyMasternodeNew, pubKeyMasternodeNew)) {
        strErrorRet = strprintf("Invalid masternode key %s", strKeyMasternode);
        LogPrintf("CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!pwalletMain->GetMasternodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex)) {
        strErrorRet = strprintf("Could not allocate txin %s:%s for masternode %s", strTxHash, strOutputIndex, strService);
        LogPrintf("CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    // The service needs the correct default port to work properly
    if(!CheckDefaultPort(strService, strErrorRet, "CMasternodeBroadcast::Create"))
        return false;

    return Create(txin, CService{strService}, keyCollateralAddressNew, pubKeyCollateralAddressNew, keyMasternodeNew, pubKeyMasternodeNew, strErrorRet, mnbRet);
}

bool CMasternodeBroadcast::Create(CTxIn txin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyMasternodeNew, CPubKey pubKeyMasternodeNew, std::string& strErrorRet, CMasternodeBroadcast& mnbRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex)
        return false;

    auto mnode = mnodeman.Find(service);

    if(mnode && mnode->vin != txin)
    {
        strErrorRet = strprintf("Duplicate Masternode address: %s", service.ToString());
        LogPrintf("CMasternodeBroadcast::Create -- ActiveMasternode::Register() -  %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    LogPrint("masternode", "CMasternodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeyMasternodeNew.GetID() = %s\n",
        CBitcoinAddress(pubKeyCollateralAddressNew.GetID()).ToString(),
        pubKeyMasternodeNew.GetID().ToString()
    );

    CMasternodePing mnp(txin);
    if (!mnp.Sign(keyMasternodeNew, pubKeyMasternodeNew)) {
        strErrorRet = strprintf("Failed to sign ping, masternode=%s", txin.prevout.hash.ToString());
        LogPrintf("CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    mnbRet = CMasternodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyMasternodeNew, PROTOCOL_VERSION);

    if (!mnbRet.IsValidNetAddr()) {
        strErrorRet = strprintf("Invalid IP address %s, masternode=%s", mnbRet.addr.ToStringIP (), txin.prevout.hash.ToString());
        LogPrintf("CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    mnbRet.lastPing = mnp;
    if (!mnbRet.Sign(keyCollateralAddressNew)) {
        strErrorRet = strprintf("Failed to sign broadcast, masternode=%s", txin.prevout.hash.ToString());
        LogPrintf("CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    return true;
}

bool CMasternodeBroadcast::CheckDefaultPort(std::string strService, std::string& strErrorRet, std::string strContext)
{
    return CheckDefaultPort(CService(strService), strErrorRet, strContext);
}

bool CMasternodeBroadcast::CheckDefaultPort(const CService& service, std::string& strErrorRet, std::string strContext)
{
    int nDefaultPort = Params().GetDefaultPort();

    if (service.GetPort() != nDefaultPort) {
        strErrorRet = strprintf("Invalid port %u for masternode %s, only %d is supported on %s-net.",
                                        service.GetPort(), service.ToString(), nDefaultPort, Params().NetworkIDString());
        LogPrint("masternode", "%s - %s\n", strContext, strErrorRet);
        return false;
    }

    return true;
}

bool CMasternodeBroadcast::CheckAndUpdate(int& nDos)
{
    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("mnb - Signature rejected, too far into the future %s\n", vin.prevout.hash.ToString());
        nDos = 1;
        return false;
    }

    std::string vchPubKey(pubKeyCollateralAddress.begin(), pubKeyCollateralAddress.end());
    std::string vchPubKey2(pubKeyMasternode.begin(), pubKeyMasternode.end());
    std::string strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(protocolVersion);

    if (protocolVersion < masternodePayments.GetMinMasternodePaymentsProto()) {
        LogPrintf("mnb - ignoring outdated Masternode %s protocol version %d\n", vin.prevout.hash.ToString(), protocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    if (pubkeyScript.size() != 25) {
        LogPrintf("mnb - pubkey the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeyMasternode.GetID());

    if (pubkeyScript2.size() != 25) {
        LogPrintf("mnb - pubkey2 the wrong size\n");
        nDos = 100;
        return false;
    }

    if (!vin.scriptSig.empty()) {
        LogPrintf("mnb - Ignore Not Empty ScriptSig %s\n", vin.prevout.hash.ToString());
        return false;
    }

    std::string errorMessage = "";

    if (!obfuScationSigner.VerifyMessage(pubKeyCollateralAddress, sig, strMessage, errorMessage)) {
        LogPrintf("mnb - Got bad Masternode address signature\n");
        nDos = 100;
        return false;
    }

    if(!CheckDefaultPort(addr, errorMessage, "CMasternodeBroadcast::CheckAndUpdate"))
        return false;

    //search existing Masternode list, this is where we update existing Masternodes with new mnb broadcasts
    CMasternode* pmn = mnodeman.Find(vin);

    // no such masternode, nothing to update
    if (!pmn)
        return true;

    // this broadcast older than we have, it's bad.
    if (pmn->sigTime > sigTime) {
        LogPrintf("mnb - Bad sigTime %d for Masternode %s (existing broadcast is at %d)\n",
        sigTime, vin.prevout.hash.ToString(), pmn->sigTime);
        return false;
    }
    // masternode is not enabled yet/already, nothing to update
    if (!pmn->IsEnabled())
        return true;

    // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
    //   after that they just need to match
    if (pmn->pubKeyCollateralAddress == pubKeyCollateralAddress && !pmn->IsBroadcastedWithin(MASTERNODE_MIN_MNB_SECONDS)) {
        //take the newest entry
        LogPrint("masternode","mnb - Got updated entry for %s\n", vin.prevout.hash.ToString());
        if (pmn->UpdateFromNewBroadcast((*this))) {
            pmn->Check();
            if (pmn->IsEnabled()) Relay();
        }
        masternodeSync.AddedMasternodeList(GetHash());
    }

    return true;
}

bool CMasternodeBroadcast::CheckInputsAndAdd(int& nDoS)
{
    // we are a masternode with the same vin (i.e. already activated) and this mnb is ours (matches our Masternode privkey)
    // so nothing to do here for us
    if (fMasterNode && vin.prevout == activeMasternode.vin.prevout && pubKeyMasternode == activeMasternode.pubKeyMasternode)
        return true;

    // search existing Masternode list
    CMasternode* pmn = mnodeman.Find(vin);

    if(pmn) {
        // nothing to do here if we already know about this masternode and it's enabled
        if (pmn->IsEnabled())
            return true;
        // if it's not enabled, remove old MN first and continue
        else
            mnodeman.Remove(pmn->vin);
    }

/*
    CMutableTransaction tx = CMutableTransaction();
    CTxOut vout = CTxOut(9999.99 * COIN, obfuScationPool.collateralPubKey);
    tx.vin.push_back(vin);
    tx.vout.push_back(vout);
*/

    CMutableTransaction tx;

    CValidationState state = CMasternodeMan::GetInputCheckingTx(vin, tx);

    if(!state.IsValid()) {
        state.IsInvalid(nDoS);
        return false;
    }

    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) {
            // not mnb fault, let it to be checked again later
            mnodeman.mapSeenMasternodeBroadcast.erase(GetHash());
            masternodeSync.mapSeenSyncMNB.erase(GetHash());
            return false;
        }

        if (!AcceptableInputs(mempool, state, CTransaction(tx), false, nullptr)) {
            //set nDos
            state.IsInvalid(nDoS);
            return false;
        }
    }

    LogPrint("masternode", "mnb - Accepted Masternode entry\n");

    if (GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS) {
        LogPrintf("mnb - Input must have at least %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS);
        // maybe we miss few blocks, let this mnb to be checked again later
        mnodeman.mapSeenMasternodeBroadcast.erase(GetHash());
        masternodeSync.mapSeenSyncMNB.erase(GetHash());
        return false;
    }

    // verify that sig time is legit in past
    // should be at least not earlier than block when 1000 XDNA tx got MASTERNODE_MIN_CONFIRMATIONS
    uint256 hashBlock = 0;
    CTransaction tx2;
    GetTransaction(vin.prevout.hash, tx2, hashBlock, true);
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi != mapBlockIndex.end() && mi->second) {
        CBlockIndex* pMNIndex = mi->second;                                                        // block for 1000 XDNA tx -> 1 confirmation
        CBlockIndex* pConfIndex = chainActive[pMNIndex->nHeight + MASTERNODE_MIN_CONFIRMATIONS - 1]; // block where tx got MASTERNODE_MIN_CONFIRMATIONS
        if (pConfIndex->GetBlockTime() > sigTime) {
            LogPrintf("mnb - Bad sigTime %d for Masternode %s (%i conf block is at %d)\n",
                sigTime, vin.prevout.hash.ToString(), MASTERNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime()
            );

            return false;
        }
    }

    LogPrintf("mnb - Got NEW Masternode entry - %s - %lli \n", vin.prevout.hash.ToString(), sigTime);
    CMasternode mn(*this);
    mnodeman.Add(mn);

    // if it matches our Masternode privkey, then we've been remotely activated
    if(pubKeyMasternode == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION) {
        activeMasternode.EnableHotColdMasterNode(vin, addr);
    }

    bool isLocal = addr.IsRFC1918() || addr.IsLocal();

    if(Params().NetworkID() == CBaseChainParams::REGTEST)
        isLocal = false;

    if(!isLocal)
        Relay();

    return true;
}

void CMasternodeBroadcast::Relay()
{
    CInv inv(MSG_MASTERNODE_ANNOUNCE, GetHash());
    RelayInv(inv);
}

bool CMasternodeBroadcast::Sign(CKey& keyCollateralAddress)
{
    std::string errorMessage;

    std::string vchPubKey(pubKeyCollateralAddress.begin(), pubKeyCollateralAddress.end());
    std::string vchPubKey2(pubKeyMasternode.begin(), pubKeyMasternode.end());

    sigTime = GetAdjustedTime();

    std::string strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(protocolVersion);

    if (!obfuScationSigner.SignMessage(strMessage, errorMessage, sig, keyCollateralAddress)) {
        LogPrintf("CMasternodeBroadcast::Sign() - Error: %s\n", errorMessage);
        return false;
    }

    if (!obfuScationSigner.VerifyMessage(pubKeyCollateralAddress, sig, strMessage, errorMessage)) {
        LogPrintf("CMasternodeBroadcast::Sign() - Error: %s\n", errorMessage);
        return false;
    }

    return true;
}

CMasternodePing::CMasternodePing()
{
    vin = CTxIn();
    blockHash = uint256(0);
    sigTime = 0;
    vchSig = std::vector<unsigned char>();
}

CMasternodePing::CMasternodePing(CTxIn& newVin)
{
    vin = newVin;
    blockHash = chainActive[chainActive.Height() - 12]->GetBlockHash();
    sigTime = GetAdjustedTime();
    vchSig = std::vector<unsigned char>();
}


bool CMasternodePing::Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode)
{
    std::string errorMessage;
    std::string strMasterNodeSignMessage;

    sigTime = GetAdjustedTime();
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);

    if (!obfuScationSigner.SignMessage(strMessage, errorMessage, vchSig, keyMasternode)) {
        LogPrintf("CMasternodePing::Sign() - Error: %s\n", errorMessage);
        return false;
    }

    if (!obfuScationSigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, errorMessage)) {
        LogPrintf("CMasternodePing::Sign() - Error: %s\n", errorMessage);
        return false;
    }

    return true;
}

bool CMasternodePing::CheckAndUpdate(int& nDos, bool fRequireEnabled)
{
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CMasternodePing::CheckAndUpdate - Signature rejected, too far into the future %s\n", vin.prevout.hash.ToString());
        nDos = 1;
        return false;
    }

    if (sigTime <= GetAdjustedTime() - 60 * 60) {
        LogPrintf("CMasternodePing::CheckAndUpdate - Signature rejected, too far into the past %s - %d %d \n", vin.prevout.hash.ToString(), sigTime, GetAdjustedTime());
        nDos = 1;
        return false;
    }

    LogPrint("masternode","CMasternodePing::CheckAndUpdate - New Ping - %s - %lli\n", blockHash.ToString(), sigTime);

    // see if we have this Masternode
    CMasternode* pmn = mnodeman.Find(vin);
    if (pmn != NULL && pmn->protocolVersion >= masternodePayments.GetMinMasternodePaymentsProto()) {
        if (fRequireEnabled && !pmn->IsEnabled()) return false;

        // LogPrintf("mnping - Found corresponding mn for vin: %s\n", vin.ToString());
        // update only if there is no known ping for this masternode or
        // last ping was more then MASTERNODE_MIN_MNP_SECONDS-60 ago comparing to this one
        if (!pmn->IsPingedWithin(MASTERNODE_MIN_MNP_SECONDS - 60, sigTime)) {
            std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);

            std::string errorMessage = "";
            if (!obfuScationSigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage)) {
                LogPrintf("CMasternodePing::CheckAndUpdate - Got bad Masternode address signature %s\n", vin.prevout.hash.ToString());
                nDos = 33;
                return false;
            }

            BlockMap::iterator mi = mapBlockIndex.find(blockHash);
            if (mi != mapBlockIndex.end() && (*mi).second) {
                if ((*mi).second->nHeight < chainActive.Height() - 24) {
                    LogPrintf("CMasternodePing::CheckAndUpdate - Masternode %s block hash %s is too old\n", vin.prevout.hash.ToString(), blockHash.ToString());
                    // Do nothing here (no Masternode update, no mnping relay)
                    // Let this node to be visible but fail to accept mnping

                    return false;
                }
            } else {
                if (fDebug) LogPrintf("CMasternodePing::CheckAndUpdate - Masternode %s block hash %s is unknown\n", vin.prevout.hash.ToString(), blockHash.ToString());
                // maybe we stuck so we shouldn't ban this node, just fail to accept it
                // TODO: or should we also request this block?

                return false;
            }

            pmn->lastPing = *this;

            //mnodeman.mapSeenMasternodeBroadcast.lastPing is probably outdated, so we'll update it
            CMasternodeBroadcast mnb(*pmn);
            uint256 hash = mnb.GetHash();
            if (mnodeman.mapSeenMasternodeBroadcast.count(hash)) {
                mnodeman.mapSeenMasternodeBroadcast[hash].lastPing = *this;
            }

            pmn->Check(true);
            if (!pmn->IsEnabled()) return false;

            LogPrint("masternode", "CMasternodePing::CheckAndUpdate - Masternode ping accepted, vin: %s\n", vin.prevout.hash.ToString());

            Relay();
            return true;
        }
        LogPrint("masternode", "CMasternodePing::CheckAndUpdate - Masternode ping arrived too early, vin: %s\n", vin.prevout.hash.ToString());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }
    LogPrint("masternode", "CMasternodePing::CheckAndUpdate - Couldn't find compatible Masternode entry, vin: %s\n", vin.prevout.hash.ToString());

    return false;
}

void CMasternodePing::Relay()
{
    CInv inv(MSG_MASTERNODE_PING, GetHash());
    RelayInv(inv);
}
