// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_PAYMENTS_H
#define MASTERNODE_PAYMENTS_H

#include "key.h"
#include "main.h"
#include "masternodeman.h"
#include "masternode.h"
#include <boost/lexical_cast.hpp>

using namespace std;

extern CCriticalSection cs_vecPayments;
extern CCriticalSection cs_mapMasternodeBlocks;
extern CCriticalSection cs_mapMasternodePayeeVotes;

class CMasternodePayments;
class CMasternodePaymentWinner;
class CMasternodeBlockPayees;

extern CMasternodePayments masternodePayments;

#define MNPAYMENTS_SIGNATURES_REQUIRED 6
#define MNPAYMENTS_SIGNATURES_TOTAL 10

void ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
bool IsBlockPayeeValid(const CBlock& block, int nBlockHeight);
std::string GetRequiredPaymentsString(int nBlockHeight);
bool IsBlockValueValid(const CBlock& block, CAmount nExpectedValue, CAmount nMinted);
void FillBlockPayee(CMutableTransaction& txNew, uint32_t nTime, CAmount nFees, bool fProofOfStake);

void DumpMasternodePayments();

/** Save Masternode Payment Data (mnpayments.dat)
 */
class CMasternodePaymentDB
{
private:
    boost::filesystem::path pathDB;
    std::string strMagicMessage;

public:
    enum ReadResult {
        Ok,
        FileError,
        HashReadError,
        IncorrectHash,
        IncorrectMagicMessage,
        IncorrectMagicNumber,
        IncorrectFormat
    };

    CMasternodePaymentDB();
    bool Write(const CMasternodePayments& objToSave);
    ReadResult Read(CMasternodePayments& objToLoad, bool fDryRun = false);
};

class CMasternodePayee
{
public:
    CScript scriptPubKey;
    unsigned mnlevel;
    CTxIn vin;
    int nVotes;

    CMasternodePayee()
    {
        scriptPubKey = CScript();
        mnlevel = CMasternode::LevelValue::UNSPECIFIED;
        vin = CTxIn();
        nVotes = 0;
    }

    CMasternodePayee(unsigned mnlevelIn, const CScript payee, CTxIn vinIn, int nVotesIn)
    {
        scriptPubKey = payee;
        mnlevel = mnlevelIn;
        vin = vinIn;
        nVotes = nVotesIn;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(scriptPubKey);
        READWRITE(mnlevel);
        READWRITE(nVotes);
        READWRITE(vin);
    }
};

// Keep track of votes for payees from masternodes
class CMasternodeBlockPayees
{
public:
    int nBlockHeight;
    std::vector<CMasternodePayee> vecPayments;

    CMasternodeBlockPayees()
    {
        nBlockHeight = 0;
        vecPayments.clear();
    }
    CMasternodeBlockPayees(int nBlockHeightIn)
    {
        nBlockHeight = nBlockHeightIn;
        vecPayments.clear();
    }

    void AddPayee(unsigned mnlevel, CScript payeeIn, CTxIn vinIn, int nIncrement)
    {
        LOCK(cs_vecPayments);

        for (CMasternodePayee& payee : vecPayments) {
            if (payee.scriptPubKey == payeeIn && payee.vin == vinIn) {
                payee.nVotes += nIncrement;
                return;
            }
        }

        CMasternodePayee c(mnlevel, payeeIn, vinIn, nIncrement);
        vecPayments.push_back(c);
    }

    bool GetPayee(unsigned mnlevel, CScript& payee) const
    {
        LOCK(cs_vecPayments);

        std::vector<CMasternodePayee>::const_iterator payment = vecPayments.cend();

        for (std::vector<CMasternodePayee>::const_iterator p = vecPayments.cbegin(), e = vecPayments.cend(); p != e; ++p) {

            if (p->mnlevel != mnlevel)
                continue;

            if (payment == vecPayments.cend() || p->nVotes > payment->nVotes)
                payment = p;
        }

        if (payment == vecPayments.cend())
            return false;

        payee = payment->scriptPubKey;

        return true;
    }

    bool HasPayeeWithVotes(CScript payee, CTxIn vin, int nVotesReq)
    {
        LOCK(cs_vecPayments);

        for (CMasternodePayee& p : vecPayments) {
            if (p.nVotes >= nVotesReq && p.scriptPubKey == payee && p.vin == vin) return true;
        }

        return false;
    }

    bool IsTransactionValid(const CTransaction& txNew, uint32_t nTime);
    std::string GetRequiredPaymentsString();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(nBlockHeight);
        READWRITE(vecPayments);
    }
};

// for storing the winning payments
class CMasternodePaymentWinner
{
public:
    CTxIn vinMasternode;

    int nBlockHeight;
    CScript payee;
    unsigned payeeLevel;
    CTxIn payeeVin;

    std::vector<unsigned char> vchSig;

    CMasternodePaymentWinner()
    {
        nBlockHeight = 0;
        vinMasternode = CTxIn();
        payee = CScript();
        payeeLevel = CMasternode::LevelValue::UNSPECIFIED;
        payeeVin = CTxIn();
    }

    CMasternodePaymentWinner(CTxIn vinIn)
    {
        nBlockHeight = 0;
        vinMasternode = vinIn;
        payee = CScript();
        payeeLevel = CMasternode::LevelValue::UNSPECIFIED;
        payeeVin = CTxIn();
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << payee;
        ss << nBlockHeight;
        ss << vinMasternode.prevout;
        ss << payeeLevel;
        ss << payeeVin;

        return ss.GetHash();
    }

    bool Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode);
    bool IsValid(CNode* pnode, std::string& strError);
    bool SignatureValid();
    void Relay();

    void AddPayee(CScript payeeIn, unsigned payeeLevelIn, CTxIn payeeVinIn)
    {
        payee = payeeIn;
        payeeLevel = payeeLevelIn;
        payeeVin = payeeVinIn;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(vinMasternode);
        READWRITE(nBlockHeight);
        READWRITE(payee);
        READWRITE(vchSig);
        READWRITE(payeeLevel);
        READWRITE(payeeVin);        
    }

    std::string ToString()
    {
        std::string ret = "";
        ret += vinMasternode.ToString();
        ret += ", " + std::to_string(nBlockHeight);
        ret += ", " + std::to_string(payeeLevel) + ":" + payee.ToString();
        ret += ", " + std::to_string((int)vchSig.size());
        return ret;
    }
};

//
// Masternode Payments Class
// Keeps track of who should get paid for which blocks
//

class CMasternodePayments
{
private:
    int nSyncedFromPeer;
    int nLastBlockHeight;

public:
    std::map<uint256, CMasternodePaymentWinner> mapMasternodePayeeVotes;
    std::map<int, CMasternodeBlockPayees> mapMasternodeBlocks;
    std::map<uint256, int> mapMasternodesLastVote; // ((outMasternode.hash + outMasternode.n) << 4) + mnlevel, nBlockHeight

    CMasternodePayments()
    {
        nSyncedFromPeer = 0;
        nLastBlockHeight = 0;
    }

    void Clear()
    {
        LOCK2(cs_mapMasternodeBlocks, cs_mapMasternodePayeeVotes);
        mapMasternodeBlocks.clear();
        mapMasternodePayeeVotes.clear();
        mapMasternodesLastVote.clear();
    }

    bool AddWinningMasternode(CMasternodePaymentWinner& winner);
    bool ProcessBlock(int nBlockHeight);

    void Sync(CNode* node, int nCountNeeded);
    void CleanPaymentList();
    int LastPayment(CMasternode& mn);

    bool GetBlockPayee(int nBlockHeight, unsigned mnlevel, CScript& payee);
    bool IsTransactionValid(const CTransaction& txNew, int nBlockHeight, uint32_t nTime);
    bool IsScheduled(CMasternode& mn, int nNotBlockHeight);
    bool CanVote(const COutPoint& outMasternode, int nBlockHeight, unsigned mnlevel)
    {
        LOCK(cs_mapMasternodePayeeVotes);

        uint256 key = ((outMasternode.hash + outMasternode.n) << 4) + mnlevel;

        if (mapMasternodesLastVote.count(key)) {
            if (mapMasternodesLastVote[key] == nBlockHeight) {
                return false;
            }
        }

        //record this masternode voted
        mapMasternodesLastVote[key] = nBlockHeight;
        return true;
    }
    int GetMinMasternodePaymentsProto();
    void ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    std::string GetRequiredPaymentsString(int nBlockHeight);
    CAmount FillBlockPayee(CMutableTransaction& txNew, uint32_t nTime, int64_t nFees, bool fProofOfStake);
    std::string ToString() const;
    int GetOldestBlock();
    int GetNewestBlock();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(mapMasternodePayeeVotes);
        READWRITE(mapMasternodeBlocks);
    }
};


#endif
