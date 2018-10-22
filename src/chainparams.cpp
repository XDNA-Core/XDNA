// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The XDNA Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "random.h"
#include "util.h"
#include "utilstrencodings.h"

#include <mutex>
#include <assert.h>
#include <limits>

#include <boost/assign/list_of.hpp>

using namespace std;
using namespace boost::assign;

struct SeedSpec6 {
    uint8_t addr[16];
    uint16_t port;
};

#include "chainparamsseeds.h"

/**
 * Main network
 */

//! Convert the pnSeeds6 array into usable address objects.
static void convertSeed6(std::vector<CAddress>& vSeedsOut, const SeedSpec6* data, unsigned int count)
{
    // It'll only connect to one or two seed nodes because once it connects,
    // it'll get a pile of addresses with newer timestamps.
    // Seed nodes are given a random 'last seen time' of between one and two
    // weeks ago.
    const int64_t nOneWeek = 7 * 24 * 60 * 60;
    for (unsigned int i = 0; i < count; i++) {
        struct in6_addr ip;
        memcpy(&ip, data[i].addr, sizeof(ip));
        CAddress addr(CService(ip, data[i].port));
        addr.nTime = GetTime() - GetRand(nOneWeek) - nOneWeek;
        vSeedsOut.push_back(addr);
    }
}

//   What makes a good checkpoint block?
// + Is surrounded by blocks with reasonable timestamps
//   (no blocks before with a timestamp after, none after with
//    timestamp before)
// + Contains no strange transactions
static Checkpoints::MapCheckpoints mapCheckpoints =
    boost::assign::map_list_of(0, uint256("000003b39d72ad4da1eb2ef2d044032dd95750cc25c435ecad2a236dd22b99fe"))
                              (50, uint256("000000224104db4572f767923cafd543f36b9a4d1eee117c4dc3e1961ca6371b"))
                              (60200, uint256("00000000000031ba24b923e1966fbe17ae0cdff1efa9d1efc556e2a3cafb5c55"))
                              (134000, uint256("0000000000c08ddeef3b04f5ead225a8d5f4765756d32407b74da5ca9c708b80"))
                              (165000, uint256("00000000010f679ac3889908996bed4716f33de4608eb48594fd36b65f03831b"));

static const Checkpoints::CCheckpointData data = {
    &mapCheckpoints,
    1540098739, // * UNIX timestamp of last checkpoint block
    234944,      // * total number of transactions between genesis and last checkpoint
                //   (the tx=... number in the SetBestChain debug.log lines)
    2000        // * estimated number of transactions per day after checkpoint
};

static Checkpoints::MapCheckpoints mapCheckpointsTestnet =
    boost::assign::map_list_of(0, uint256("000006b020d0db323b363c4d762b6931cff1855fd8a85a4455f416a91e9424f1"));
static const Checkpoints::CCheckpointData dataTestnet = {
    &mapCheckpointsTestnet,
    1529667000,
    0,
    250};

static Checkpoints::MapCheckpoints mapCheckpointsRegtest =
    boost::assign::map_list_of(0, uint256("300552a9db8b2921c3c07e5bbf8694df5099db579742e243daeaf5008b1e74de"));
static const Checkpoints::CCheckpointData dataRegtest = {
    &mapCheckpointsRegtest,
    1529668200,
    0,
    100};

const CChainParams::SubsidySwitchPoints& CChainParams::GetSubsidySwitchPoints(uint32_t nTime, int nHeight) const
{
    if(nTime <= nHEXHashTimestamp)
       return subsidySwitchPoints;
    else if(nTime <= nF2Timestamp)
       return subsidySwitchPoints_HEXHash;
    else if(nHeight < static_cast<int>(subsidyScheduleStart_F2))
        return subsidySwitchPoints_HEXHash;

    auto decrease_interval = std::min(subsidyDecreaseCount_F2, (nHeight - subsidyScheduleStart_F2) / subsidyDecreaseInterval_F2);

    return subsidySwitchPointsSchedule_F2.find(decrease_interval)->second;
}

CAmount CChainParams::SubsidyValue(SubsidySwitchPoints::key_type level, uint32_t nTime, int nHeight) const
{
    const auto& switch_points = GetSubsidySwitchPoints(nTime, nHeight);

    SubsidySwitchPoints::const_iterator point = switch_points.upper_bound(level);

    if(point != switch_points.begin())
        point = std::prev(point);

    return point->second;
}

void CChainParams::initSubsidySwitchPointsSchedule()
{
    subsidySwitchPointsSchedule_F2[0u] = subsidySwitchPoints_F2_0;

    for(auto i = 1u; i <= subsidyDecreaseCount_F2; ++i)
    {
       subsidySwitchPointsSchedule_F2[i] = subsidySwitchPointsSchedule_F2[i - 1];

       for(auto& sp : subsidySwitchPointsSchedule_F2[i])
       {
           auto prev_value = sp.second;

           sp.second *= 10000u - subsidyDecreaseValue_F2;
           sp.second /= 10000u;
           sp.second += COIN / 10u - 1u;
           sp.second /= COIN / 10u;
           sp.second *= COIN / 10u;

           if(sp.second == prev_value && sp.second > COIN / 10u)
             sp.second -= COIN / 10u;
       }
    }
}

class CMainParams : public CChainParams
{
public:
    CMainParams()
    {
        networkID = CBaseChainParams::MAIN;
        strNetworkID = "main";
        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 4-byte int at any alignment.
         */
        pchMessageStart[0] = 0x90;
        pchMessageStart[1] = 0xc3;
        pchMessageStart[2] = 0xfe;
        pchMessageStart[3] = 0xe8;
        vAlertPubKey = ParseHex("04A2B684CBABE97BA08A35EA388B06A6B03E13DFBA974466880AF4CAE1C5B606A751BF7C5CBDE5AB90722CF5B1EC1AADA6D24D607870B6D6B5D684082655404C8D");
        vXDNADevKey = ParseHex("022168bed33339fdc945077c9056a42ceacda589b9d2c7733a72e75e7ee29ef586"); // DevPubKey for fees
        vXDNAFundKey = ParseHex("03b7a94485bb264d12fbffdddff462060e51a27e55f256b62648c25c1fb8bb565c"); // FundPubKey for fees
        nDevFee = 1; // DevFee %
        nFundFee = 2; //FundFee %
        nDefaultPort = 1945;
        bnProofOfWorkLimit = ~uint256(0) >> 20;
        bnStartWork = ~uint256(0) >> 24;

        subsidySwitchPoints = {
            {0         ,   4 * COIN},
            {2   * 1e12,   5 * COIN},
            {3   * 1e12,   7 * COIN},
            {5   * 1e12,   9 * COIN},
            {8   * 1e12,  11 * COIN},
            {13  * 1e12,  15 * COIN},
            {21  * 1e12,  20 * COIN},
            {34  * 1e12,  27 * COIN},
            {55  * 1e12,  39 * COIN},
            {89  * 1e12,  57 * COIN},
            {144 * 1e12,  85 * COIN},
            {233 * 1e12, 131 * COIN},
            {377 * 1e12, 204 * COIN},
            {610 * 1e12, 321 * COIN},
            {987 * 1e12, 511 * COIN},
        };
        assert(subsidySwitchPoints.size());

        subsidySwitchPoints_HEXHash = {
            {0         ,   4 * COIN},
            {20   * 1e9,   5 * COIN},
            {30   * 1e9,   7 * COIN},
            {50   * 1e9,  10 * COIN},
            {80   * 1e9,  14 * COIN},
            {130  * 1e9,  19 * COIN},
            {210  * 1e9,  25 * COIN},
            {340  * 1e9,  32 * COIN},
            {550  * 1e9,  40 * COIN},
            {890  * 1e9,  49 * COIN},
            {1440 * 1e9,  59 * COIN},
            {2330 * 1e9,  70 * COIN},
            {3770 * 1e9,  82 * COIN},
            {6100 * 1e9,  95 * COIN},
            {9870 * 1e9, 109 * COIN},
        };
        assert(subsidySwitchPoints_HEXHash.size());

        subsidySwitchPoints_F2_0 = {
            {0         ,   38  * (COIN/10)},
            {20   * 1e9,   47  * (COIN/10)},
            {30   * 1e9,   66  * (COIN/10)},
            {50   * 1e9,   94  * (COIN/10)},
            {80   * 1e9,  131  * (COIN/10)},
            {130  * 1e9,  177  * (COIN/10)},
            {210  * 1e9,  233  * (COIN/10)},
            {340  * 1e9,  298  * (COIN/10)},
            {550  * 1e9,  373  * (COIN/10)},
            {890  * 1e9,  456  * (COIN/10)},
            {1440 * 1e9,  550  * (COIN/10)},
            {2330 * 1e9,  652  * (COIN/10)},
            {3770 * 1e9,  764  * (COIN/10)},
            {6100 * 1e9,  885  * (COIN/10)},
            {9870 * 1e9,  1015 * (COIN/10)},
        };
        assert(subsidySwitchPoints_F2_0.size());

        subsidyScheduleStart_F2    = 177000; // block#XXXXXX ~= nF2Timestamp + 1 day
        subsidyDecreaseInterval_F2 = 43200;  // 43200 bloks ~= 30 days
        subsidyDecreaseCount_F2    = 23;     // 23
        subsidyDecreaseValue_F2    = 694;    // 694 = 6,94% * 100

        initSubsidySwitchPointsSchedule();

        nMaxReorganizationDepth = 100;
        nEnforceBlockUpgradeMajority = 750;
        nRejectBlockOutdatedMajority = 950;
        nToCheckBlockUpgradeMajority = 1000;
        nMinerThreads = 0;
        nTargetSpacing = 1 * 60;  // XDNA: 1 minute
        nAntiInstamineTime = 720; // 720 blocks with 1 reward for instamine prevention
        nMaturity = 60;
        nMasternodeCountDrift = 3;
        nMaxMoneyOut = 367452000 * COIN;

        nStartMasternodePaymentsBlock = 7001;

        /** Height or Time Based Activations **/
        nLastPOWBlock = 1440000;
        nModifierUpdateBlock = std::numeric_limits<decltype(nModifierUpdateBlock)>::max();

        /**
         * Build the genesis block. Note that the output of the genesis coinbase cannot
         * be spent as it did not originally exist in the database.
         *
         * CBlock(hash=00000ffd590b14, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=e0028e, nTime=1390095618, nBits=1e0ffff0, nNonce=28917698, vtx=1)
         *   CTransaction(hash=e0028e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
         *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d01044c5957697265642030392f4a616e2f3230313420546865204772616e64204578706572696d656e7420476f6573204c6976653a204f76657273746f636b2e636f6d204973204e6f7720416363657074696e6720426974636f696e73)
         *     CTxOut(nValue=50.00000000, scriptPubKey=0xA9037BAC7050C479B121CF)
         *   vMerkleTree: e0028e
         */
        const char* pszTimestamp = "RT.com Iran may exit nuclear deal in coming weeks – Deputy FM 22 Jun, 2018 08:49";
        CMutableTransaction txNew;
        txNew.vin.resize(1);
        txNew.vout.resize(1);
        txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
        txNew.vout[0].nValue = 50 * COIN;
        txNew.vout[0].scriptPubKey = CScript() << ParseHex("044a001040da79684a0544c2254eb6c896fae95a9ea7b51d889475eb57ab2051f1a5858cac61ae400e90ea08015263ad40c65d36f0edf19e996972e7d2cbd13c15") << OP_CHECKSIG;
        genesis.vtx.push_back(txNew);
        genesis.hashPrevBlock = 0;
        genesis.hashMerkleRoot = genesis.BuildMerkleTree();
        genesis.nVersion = 1;
        genesis.nTime = 1529665200;
        genesis.nBits = 0x1e0ffff0;
        genesis.nNonce = 24657;

        hashGenesisBlock = genesis.GetKeccakHash();

        assert(hashGenesisBlock == uint256("000003b39d72ad4da1eb2ef2d044032dd95750cc25c435ecad2a236dd22b99fe"));
        assert(genesis.hashMerkleRoot == uint256("89370975b13f97d8f9cfc373b0e9d5cc0e2e06b8dc283c76824e4df03ca2d60a"));

        vSeeds.push_back(CDNSSeedData("xdna.io", "seeds.seeder01.xdna.io"));     // Primary DNS Seeder

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 75);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 8);
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 212);
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x02)(0x2D)(0x25)(0x33).convert_to_container<std::vector<unsigned char> >();
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x02)(0x21)(0x31)(0x2B).convert_to_container<std::vector<unsigned char> >();
        // BIP44 coin type is from https://github.com/satoshilabs/slips/blob/master/slip-0044.md
        base58Prefixes[EXT_COIN_TYPE] = boost::assign::list_of(0x80)(0x00)(0x07)(0x99).convert_to_container<std::vector<unsigned char> >();

        convertSeed6(vFixedSeeds, pnSeed6_main, ARRAYLEN(pnSeed6_main));

        fRequireRPCPassword = true;
        fMiningRequiresPeers = true;
        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;
        fSkipProofOfWorkCheck = false;
        fTestnetToBeDeprecatedFieldRPC = false;
        fHeadersFirstSyncingActive = false;

        nPoolMaxTransactions = 3;
        strSporkKey = "04520C1E6A46596DD9CA9A1A69B96D630410CBA2A1047FC462ADAA5D3BE451CC43B2E30C64A03513F31B3DB9450A3FC2F742DCB4AD99450575219549890392F465";
        strObfuscationPoolDummyAddress = "X87q2gC9j6nNrnzCsg4aY6bHMLsT9nUhEw";
        nStartMasternodePayments = 1403728576; //Wed, 25 Jun 2014 20:36:16 GMT

        nHEXHashTimestamp = 1533567600; // 6  August  2018, 15:00:00 GMT+00:00
        nF2Timestamp      = 1540728000; // 28 October 2018, 12:00:00 GMT+00:00
    }

    const Checkpoints::CCheckpointData& Checkpoints() const
    {
        return data;
    }
};
static CMainParams mainParams;

/**
 * Testnet (v3)
 */
class CTestNetParams : public CMainParams
{
public:
    CTestNetParams()
    {
        networkID = CBaseChainParams::TESTNET;
        strNetworkID = "test";
        pchMessageStart[0] = 0x47;
        pchMessageStart[1] = 0x77;
        pchMessageStart[2] = 0x66;
        pchMessageStart[3] = 0xbb;

        bnProofOfWorkLimit = ~uint256(0) >> 1;
        bnStartWork = bnProofOfWorkLimit;

        subsidySwitchPoints = {
           {0        ,   4 * COIN},
           {2   * 1e7,   5 * COIN},
           {3   * 1e7,   7 * COIN},
           {5   * 1e7,   9 * COIN},
           {8   * 1e7,  11 * COIN},
           {13  * 1e7,  15 * COIN},
           {21  * 1e7,  20 * COIN},
           {34  * 1e7,  27 * COIN},
           {55  * 1e7,  39 * COIN},
           {89  * 1e7,  57 * COIN},
           {144 * 1e7,  85 * COIN},
           {233 * 1e7, 131 * COIN},
           {377 * 1e7, 204 * COIN},
           {610 * 1e7, 321 * COIN},
           {987 * 1e7, 511 * COIN},
        };
        assert(subsidySwitchPoints.size());

        vAlertPubKey = ParseHex("04459DC949A9E2C2E1FA87ED9EE93F8D26CD52F95853EE24BCD4B07D4B7D79458E81F0425D81E52B797ED304A836667A1D2D422CD10F485B06CCBE906E1081FBAC");
        nDefaultPort = 11945;
        nEnforceBlockUpgradeMajority = 51;
        nRejectBlockOutdatedMajority = 75;
        nToCheckBlockUpgradeMajority = 100;
        nMinerThreads = 0;
        nTargetSpacing = 1 * 60;  // XDNA: 1 minute
        nLastPOWBlock = std::numeric_limits<decltype(nLastPOWBlock)>::max();
        nMaturity = 15;
        nMasternodeCountDrift = 4;
        nModifierUpdateBlock = std::numeric_limits<decltype(nModifierUpdateBlock)>::max();
        nMaxMoneyOut = 1000000000 * COIN;


        //! Modify the testnet genesis block so the timestamp is valid for a later start.
        genesis.nTime = 1529667000;
        genesis.nNonce = 290796;

        hashGenesisBlock = genesis.GetKeccakHash();

        assert(hashGenesisBlock == uint256("000006b020d0db323b363c4d762b6931cff1855fd8a85a4455f416a91e9424f1"));

        vFixedSeeds.clear();
        vSeeds.clear();
        vSeeds.push_back(CDNSSeedData("xdna.io", "seed01.xdna.io"));     // Primary DNS Seeder

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 137); // Testnet XDNA addresses start with 'x'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 19);  // Testnet XDNA script addresses start with '8' or '9'
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 239);     // Testnet private keys start with '9' or 'c' (Bitcoin defaults)
        // Testnet XDNA BIP32 pubkeys start with 'DRKV'
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x3a)(0x80)(0x61)(0xa0).convert_to_container<std::vector<unsigned char> >();
        // Testnet XDNA BIP32 prvkeys start with 'DRKP'
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x3a)(0x80)(0x58)(0x37).convert_to_container<std::vector<unsigned char> >();
        // Testnet XDNA BIP44 coin type is '1' (All coin's testnet default)
        base58Prefixes[EXT_COIN_TYPE] = boost::assign::list_of(0x80)(0x00)(0x00)(0x01).convert_to_container<std::vector<unsigned char> >();

        convertSeed6(vFixedSeeds, pnSeed6_test, ARRAYLEN(pnSeed6_test));

        fRequireRPCPassword = true;
        fMiningRequiresPeers = false;
        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        fMineBlocksOnDemand = false;
        fTestnetToBeDeprecatedFieldRPC = true;

        nPoolMaxTransactions = 2;
        strSporkKey = "0421838CC1407E7B8C0C5F2379DF7EBD395181949CFA55124939B4980D5054A7926F88E3059921A50F0F81C5195E882D9A414EA0835BB89C9BB061511B9F132B31";
        strObfuscationPoolDummyAddress = "y57cqfGRkekRyDRNeJiLtYVEbvhXrNbmox";
        nStartMasternodePayments = 1420837558; //Fri, 09 Jan 2015 21:05:58 GMT
    }
    const Checkpoints::CCheckpointData& Checkpoints() const
    {
        return dataTestnet;
    }
};
static CTestNetParams testNetParams;

/**
 * Regression test
 */
class CRegTestParams : public CTestNetParams
{
public:
    CRegTestParams()
    {
        networkID = CBaseChainParams::REGTEST;
        strNetworkID = "regtest";
        pchMessageStart[0] = 0xa1;
        pchMessageStart[1] = 0xcf;
        pchMessageStart[2] = 0x7e;
        pchMessageStart[3] = 0xac;

        bnStartWork = ~uint256(0) >> 20;

        subsidySwitchPoints = {
           {0        ,   4 * COIN},
           {2   * 1e7,   5 * COIN},
           {3   * 1e7,   7 * COIN},
           {5   * 1e7,   9 * COIN},
           {8   * 1e7,  11 * COIN},
           {13  * 1e7,  15 * COIN},
           {21  * 1e7,  20 * COIN},
           {34  * 1e7,  27 * COIN},
           {55  * 1e7,  39 * COIN},
           {89  * 1e7,  57 * COIN},
           {144 * 1e7,  85 * COIN},
           {233 * 1e7, 131 * COIN},
           {377 * 1e7, 204 * COIN},
           {610 * 1e7, 321 * COIN},
           {987 * 1e7, 511 * COIN},
        };
        assert(subsidySwitchPoints.size());

        nEnforceBlockUpgradeMajority = 750;
        nRejectBlockOutdatedMajority = 950;
        nToCheckBlockUpgradeMajority = 1000;
        nMinerThreads = 1;
        nTargetSpacing = 1 * 60;        // XDNA: 1 minute
        bnProofOfWorkLimit = ~uint256(0) >> 1;
        genesis.nTime = 1529668200;
        genesis.nBits = 0x207fffff;
        genesis.nNonce = 1;

        hashGenesisBlock = genesis.GetKeccakHash();
        nDefaultPort = 51476;

        assert(hashGenesisBlock == uint256("300552a9db8b2921c3c07e5bbf8694df5099db579742e243daeaf5008b1e74de"));

        vFixedSeeds.clear(); //! Testnet mode doesn't have any fixed seeds.
        vSeeds.clear();      //! Testnet mode doesn't have any DNS seeds.

        fRequireRPCPassword = false;
        fMiningRequiresPeers = false;
        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        fMineBlocksOnDemand = true;
        fTestnetToBeDeprecatedFieldRPC = false;
    }
    const Checkpoints::CCheckpointData& Checkpoints() const
    {
        return dataRegtest;
    }
};
static CRegTestParams regTestParams;

/**
 * Unit test
 */
class CUnitTestParams : public CMainParams, public CModifiableParams
{
public:
    CUnitTestParams()
    {
        networkID = CBaseChainParams::UNITTEST;
        strNetworkID = "unittest";
        nDefaultPort = 51478;
        vFixedSeeds.clear(); //! Unit test mode doesn't have any fixed seeds.
        vSeeds.clear();      //! Unit test mode doesn't have any DNS seeds.

        fRequireRPCPassword = false;
        fMiningRequiresPeers = false;
        fDefaultConsistencyChecks = true;
        fMineBlocksOnDemand = true;

        subsidySwitchPoints = {
            {0         ,   1 * COIN},
            {2   * 1e5,   2 * COIN},
            {3   * 1e5,   3 * COIN},
            {5   * 1e5,   5 * COIN},
            {8   * 1e5,   8 * COIN},
            {13  * 1e5,  13 * COIN},
            {21  * 1e5,  21 * COIN},
            {34  * 1e5,  34 * COIN},
            {55  * 1e5,  55 * COIN},
            {89  * 1e5,  89 * COIN},
            {144 * 1e5, 144 * COIN},
            {233 * 1e5, 233 * COIN},
            {377 * 1e5, 377 * COIN},
            {610 * 1e5, 610 * COIN},
            {987 * 1e5, 987 * COIN},
        };
        assert(subsidySwitchPoints.size());

    }

    const Checkpoints::CCheckpointData& Checkpoints() const
    {
        // UnitTest share the same checkpoints as MAIN
        return data;
    }

    //! Published setters to allow changing values in unit test cases
    virtual void setEnforceBlockUpgradeMajority(int anEnforceBlockUpgradeMajority) { nEnforceBlockUpgradeMajority = anEnforceBlockUpgradeMajority; }
    virtual void setRejectBlockOutdatedMajority(int anRejectBlockOutdatedMajority) { nRejectBlockOutdatedMajority = anRejectBlockOutdatedMajority; }
    virtual void setToCheckBlockUpgradeMajority(int anToCheckBlockUpgradeMajority) { nToCheckBlockUpgradeMajority = anToCheckBlockUpgradeMajority; }
    virtual void setDefaultConsistencyChecks(bool afDefaultConsistencyChecks) { fDefaultConsistencyChecks = afDefaultConsistencyChecks; }
    virtual void setSkipProofOfWorkCheck(bool afSkipProofOfWorkCheck) { fSkipProofOfWorkCheck = afSkipProofOfWorkCheck; }
};
static CUnitTestParams unitTestParams;


static CChainParams* pCurrentParams = 0;

CModifiableParams* ModifiableParams()
{
    assert(pCurrentParams);
    assert(pCurrentParams == &unitTestParams);
    return (CModifiableParams*)&unitTestParams;
}

const CChainParams& Params()
{
    assert(pCurrentParams);
    return *pCurrentParams;
}

CChainParams& Params(CBaseChainParams::Network network)
{
    switch (network) {
    case CBaseChainParams::MAIN:
        return mainParams;
    case CBaseChainParams::TESTNET:
        return testNetParams;
    case CBaseChainParams::REGTEST:
        return regTestParams;
    case CBaseChainParams::UNITTEST:
        return unitTestParams;
    default:
        assert(false && "Unimplemented network");
        return mainParams;
    }
}

void SelectParams(CBaseChainParams::Network network)
{
    SelectBaseParams(network);
    pCurrentParams = &Params(network);
}

bool SelectParamsFromCommandLine()
{
    CBaseChainParams::Network network = NetworkIdFromCommandLine();
    if (network == CBaseChainParams::MAX_NETWORK_TYPES)
        return false;

    SelectParams(network);
    return true;
}
