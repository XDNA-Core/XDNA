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

class CMasternodePayee
{
public:
    CScript scriptPubKey;
    unsigned mnlevel;
    int nVotes;

    CMasternodePayee()
    {
        mnlevel = CMasternode::LevelValue::UNSPECIFIED;
        nVotes = 0;
    }

    CMasternodePayee(unsigned mnlevelIn, const CScript payee, int nVotesIn)
    {
        scriptPubKey = payee;
        mnlevel = mnlevelIn;
        nVotes = nVotesIn;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(scriptPubKey);
        READWRITE(mnlevel);
        READWRITE(nVotes);
    }
};

// Keep track of votes for payees from masternodes
class CMasternodeBlockPayees
{
public:

    int nBlockHeight;
    std::vector<CMasternodePayee> vecPayments;

    CMasternodeBlockPayees(int nBlockHeightIn = 0)
        : nBlockHeight{nBlockHeightIn}
    {
    }

    void AddPayee(unsigned mnlevel, CScript payeeIn, int nIncrement)
    {
        LOCK(cs_vecPayments);

        auto payee = std::find_if(vecPayments.begin(), vecPayments.end(), [&payeeIn](const CMasternodePayee& p){
            return p.scriptPubKey == payeeIn;
        });

        if(payee == vecPayments.end())
            vecPayments.emplace_back(mnlevel, payeeIn, nIncrement);
        else
            payee->nVotes += nIncrement;
    }

    bool GetPayee(unsigned mnlevel, CScript& payee) const
    {
        LOCK(cs_vecPayments);

        auto payment = vecPayments.cend();

        for(auto p = vecPayments.cbegin(), e = vecPayments.cend(); p != e; ++p) {

            if(p->mnlevel != mnlevel)
                continue;

            if(payment == vecPayments.cend() || p->nVotes > payment->nVotes)
                payment = p;
        };

        if(payment == vecPayments.cend())
            return false;

        payee = payment->scriptPubKey;

        return true;
    }

    bool HasPayeeWithVotes(CScript payee, int nVotesReq)
    {
        LOCK(cs_vecPayments);

        auto payment = std::find_if(vecPayments.cbegin(), vecPayments.cend(), [&](const CMasternodePayee& p){
            return p.nVotes >= nVotesReq && p.scriptPubKey == payee;
        });

        return payment != vecPayments.cend();
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
    std::vector<unsigned char> vchSig;
    unsigned payeeLevel;

    CMasternodePaymentWinner()
    {
        nBlockHeight = 0;
        vinMasternode = CTxIn();
        payeeLevel = CMasternode::LevelValue::UNSPECIFIED;
    }

    CMasternodePaymentWinner(CTxIn vinIn)
    {
        nBlockHeight = 0;
        vinMasternode = vinIn;
        payeeLevel = CMasternode::LevelValue::UNSPECIFIED;
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << payee;
        ss << nBlockHeight;
        ss << vinMasternode.prevout;

        return ss.GetHash();
    }

    bool Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode);
    bool IsValid(CNode* pnode, std::string& strError);
    bool SignatureValid();
    void Relay();

    void AddPayee(CScript payeeIn, unsigned payeeLevelIn)
    {
        payee = payeeIn;
        payeeLevel = payeeLevelIn;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(vinMasternode);
        READWRITE(nBlockHeight);
        READWRITE(payee);
        READWRITE(vchSig);
    }

    std::string ToString()
    {
        std::string ret;
        ret += vinMasternode.ToString();
        ret += ", " + boost::lexical_cast<std::string>(nBlockHeight);
        ret += ", " + boost::lexical_cast<std::string>(payeeLevel) + ":" + payee.ToString();
        ret += ", " + boost::lexical_cast<std::string>((int)vchSig.size());
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

    int nLastBlockHeight;

public:
    std::map<uint256, CMasternodePaymentWinner> mapMasternodePayeeVotes;
    std::map<int, CMasternodeBlockPayees> mapMasternodeBlocks;
    std::map<uint256, int> mapMasternodesLastVote; // ((outMasternode.hash + outMasternode.n) << 4) + mnlevel, nBlockHeight

    CMasternodePayments()
    {
        nLastBlockHeight = 0;
    }

    void Clear()
    {
        LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);
        mapMasternodeBlocks.clear();
        mapMasternodePayeeVotes.clear();
        mapMasternodesLastVote.clear();
    }

    bool AddWinningMasternode(CMasternodePaymentWinner& winner);
    bool ProcessBlock(int nBlockHeight);

    void Sync(CNode* node, int nCountNeeded);
    void CleanPaymentList();

    bool GetBlockPayee(int nBlockHeight, unsigned mnlevel, CScript& payee);
    bool IsTransactionValid(const CTransaction& txNew, int nBlockHeight, uint32_t nTime);
    bool IsScheduled(CMasternode& mn, int nSameLevelMNCount, int nNotBlockHeight) const;
    bool CanVote(const COutPoint& outMasternode, int nBlockHeight, unsigned mnlevel);

    int GetMinMasternodePaymentsProto();
    void ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    std::string GetRequiredPaymentsString(int nBlockHeight);
    CAmount FillBlockPayee(CMutableTransaction& txNew, int64_t block_value, bool fProofOfStake);
    std::string ToString() const;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(mapMasternodePayeeVotes);
        READWRITE(mapMasternodeBlocks);
    }
};


#endif
