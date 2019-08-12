// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The XDNA Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "clientmodel.h"

#include "guiconstants.h"
#include "peertablemodel.h"

#include "alert.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "clientversion.h"
#include "main.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "net.h"
#include "ui_interface.h"
#include "util.h"

#include <stdint.h>
#include <map>

#include <QDateTime>
#include <QDebug>
#include <QTimer>

static const int64_t nClientStartupTime = GetTime();

struct statElement {
  uint32_t blockTime; // block time
  CAmount txInValue; // pos input value
  std::vector<std::pair<std::string, CAmount>> mnPayee; // masternode payees
};
static int blockOldest = 0;
static int blockLast = 0;
static std::vector<std::pair<int, statElement>> statSourceData;

CCriticalSection cs_stat;
map<std::string, CAmount> masternodeRewards;
CAmount posMin, posMax, posMedian;
int block24hCount;
CAmount lockedCoin;

ClientModel::ClientModel(OptionsModel* optionsModel, QObject* parent) : QObject(parent),
                                                                        optionsModel(optionsModel),
                                                                        peerTableModel(0),
                                                                        cachedNumBlocks(0),
                                                                        cachedMasternodeCountString(""),
                                                                        cachedReindexing(0), cachedImporting(0),
                                                                        numBlocksAtStartup(-1), pollTimer(0)
{
    peerTableModel = new PeerTableModel(this);
    pollTimer = new QTimer(this);
    connect(pollTimer, SIGNAL(timeout()), this, SLOT(updateTimer()));
    pollTimer->start(MODEL_UPDATE_DELAY);

    pollMnTimer = new QTimer(this);
    connect(pollMnTimer, SIGNAL(timeout()), this, SLOT(updateMnTimer()));
    // no need to update as frequent as data for balances/txes/blocks
    pollMnTimer->start(MODEL_UPDATE_DELAY * 4);

    poll24hStatsTimer = new QTimer(this);
    connect(poll24hStatsTimer, SIGNAL(timeout()), this, SLOT(update24hStatsTimer()));
    poll24hStatsTimer->start(MODEL_UPDATE_DELAY * 10);

    subscribeToCoreSignals();
}

ClientModel::~ClientModel()
{
    unsubscribeFromCoreSignals();
}

bool sortStat(const pair<int,statElement> &a, const pair<int,statElement> &b)
{
    return (a.second.txInValue < b.second.txInValue);
}

void ClientModel::update24hStatsTimer()
{
  // Get required lock upfront. This avoids the GUI from getting stuck on
  // periodical polls if the core is holding the locks for a longer time -
  // for example, during a wallet rescan.
  TRY_LOCK(cs_main, lockMain);
  if (!lockMain) return;

  TRY_LOCK(cs_stat, lockStat);
  if (!lockStat) return;

  if (masternodeSync.IsBlockchainSynced() && !IsInitialBlockDownload()) {
    qDebug() << __FUNCTION__ << ": Process stats...";
    const int64_t syncStartTime = GetTime();

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[chainActive.Tip()->GetBlockHash()];

    CTxDestination Dest;
    CBitcoinAddress Address;

    int currentBlock = pblockindex->nHeight;
    // read block from last to last scaned
    while (pblockindex->nHeight > blockLast) {
        if (ReadBlockFromDisk(block, pblockindex)) {
            if (block.IsProofOfStake()) {
                // decode transactions
                const CTransaction& tx = block.vtx[1];
                if (tx.IsCoinStake()) {
                    // decode txIn
                    CTransaction txIn;
                    uint256 hashBlock;
                    if (GetTransaction(tx.vin[0].prevout.hash, txIn, hashBlock, true)) {
                        CAmount valuePoS = txIn.vout[tx.vin[0].prevout.n].nValue; // vin Value
                        ExtractDestination(txIn.vout[tx.vin[0].prevout.n].scriptPubKey, Dest);
                        Address.Set(Dest);
                        std::string addressPoS = Address.ToString(); // vin Address

                        statElement blockStat;
                        blockStat.blockTime = block.nTime;
                        blockStat.txInValue = valuePoS;
                        blockStat.mnPayee.clear();

                        // decode txOut
                        CAmount sumPoS = 0;
                        for (unsigned int i = 0; i < tx.vout.size(); i++) {
                            CTxOut txOut = tx.vout[i];
                            ExtractDestination(txOut.scriptPubKey, Dest);
                            Address.Set(Dest);
                            std::string addressOut = Address.ToString(); // vout Address
                            if (addressPoS == addressOut && valuePoS > sumPoS) {
                                // skip pos output
                                sumPoS += txOut.nValue;
                            } else {
                                // store vout payee and value
                                blockStat.mnPayee.push_back( make_pair(addressOut, txOut.nValue) );
                                // and update node rewards
                                masternodeRewards[addressOut] += txOut.nValue;
                            }
                        }
                        // store block stat
                        statSourceData.push_back( make_pair(pblockindex->nHeight, blockStat) );
                        // stop if blocktime over 24h past
                        if ( (block.nTime + 24*60*60) < syncStartTime ) {
                            blockOldest = pblockindex->nHeight;
                            break;
                        }
                    }
                }
            }
        }
        // select next (previous) block
        pblockindex = pblockindex->pprev;
    }

    // clear over 24h block data
    std::vector<pair<std::string, CAmount>> tMN;
    std::string tAddress;
    CAmount tValue;
    if (statSourceData.size() > 0) {
        for (auto it = statSourceData.rbegin(); it != statSourceData.rend(); ++it) {
            if ( (it->second.blockTime + 24*60*60) < syncStartTime) {
                tMN = it->second.mnPayee;
                for (auto im = tMN.begin(); im != tMN.end(); ++im) {
                    tAddress = im->first;
                    tValue = im->second;
                    masternodeRewards[tAddress] -= tValue;
                }
                // remove element
                *it = statSourceData.back();
                statSourceData.pop_back();
            }
        }
    }

    // recalc stats data if new block found
    if (currentBlock > blockLast && statSourceData.size() > 0) {
      // sorting vector and get stats values
      sort(statSourceData.begin(), statSourceData.end(), sortStat);

      if (statSourceData.size() > 100) {
        CAmount posAverage = 0;
        for (auto it = statSourceData.begin(); it != statSourceData.begin() + 100; ++it)
              posAverage += it->second.txInValue;
        posMin = posAverage / 100;
        for (auto it = statSourceData.rbegin(); it != statSourceData.rbegin() + 100; ++it)
              posAverage += it->second.txInValue;
        posMax = posAverage / 100;
      } else {
        posMin = statSourceData.front().second.txInValue;
        posMax = statSourceData.back().second.txInValue;
      }

      if (statSourceData.size() % 2) {
        posMedian = (statSourceData[int(statSourceData.size()/2)].second.txInValue + statSourceData[int(statSourceData.size()/2)-1].second.txInValue) / 2;
      } else {
        posMedian = statSourceData[int(statSourceData.size()/2)-1].second.txInValue;
      }
      block24hCount = statSourceData.size();
    }

    blockLast = currentBlock;

    if (poll24hStatsTimer->interval() < 30000)
        poll24hStatsTimer->setInterval(30000);

    qDebug() << __FUNCTION__ << ": Stats ready...";
  }

  // sending signal
  //emit stats24hUpdated();
}

int ClientModel::getNumConnections(unsigned int flags) const
{
    LOCK(cs_vNodes);
    qDebug() << __FUNCTION__ << ": LOCK(cs_vNodes)";
    if (flags == CONNECTIONS_ALL) // Shortcut if we want total
        return vNodes.size();

    int nNum = 0;
    BOOST_FOREACH (CNode* pnode, vNodes)
        if (flags & (pnode->fInbound ? CONNECTIONS_IN : CONNECTIONS_OUT))
            nNum++;

    return nNum;
}

QString ClientModel::getMasternodeCountString() const
{
    int ipv4 = 0, ipv6 = 0, onion = 0;
    mnodeman.CountNetworks(ActiveProtocol(), ipv4, ipv6, onion);
    int nUnknown = mnodeman.size() - ipv4 - ipv6 - onion;
    if (nUnknown < 0) nUnknown = 0;
    return tr("Total: %1 (IPv4: %2 / IPv6: %3 / Onion: %4 / Unknown: %5)")
        .arg(QString::number((int)mnodeman.size()))
        .arg(QString::number((int)ipv4))
        .arg(QString::number((int)ipv6))
        .arg(QString::number((int)onion))
        .arg(QString::number((int)nUnknown));
}

int ClientModel::getNumBlocks() const
{
    LOCK(cs_main);
    return chainActive.Height();
}

int ClientModel::getNumBlocksAtStartup()
{
    if (numBlocksAtStartup == -1) numBlocksAtStartup = getNumBlocks();
    return numBlocksAtStartup;
}

quint64 ClientModel::getTotalBytesRecv() const
{
    return CNode::GetTotalBytesRecv();
}

quint64 ClientModel::getTotalBytesSent() const
{
    return CNode::GetTotalBytesSent();
}

QDateTime ClientModel::getLastBlockDate() const
{
    LOCK(cs_main);
    qDebug() << __FUNCTION__ << ": LOCK(cs_main)";
    if (chainActive.Tip())
        return QDateTime::fromTime_t(chainActive.Tip()->GetBlockTime());
    else
        return QDateTime::fromTime_t(Params().GenesisBlock().GetBlockTime()); // Genesis block's time of current network
}

double ClientModel::getVerificationProgress() const
{
    LOCK(cs_main);
    return Checkpoints::GuessVerificationProgress(chainActive.Tip());
}

void ClientModel::updateTimer()
{
    // Some quantities (such as number of blocks) change so fast that we don't want to be notified for each change.
    // Periodically check and update with a timer.
    int newNumBlocks = getNumBlocks();

    static int prevAttempt = -1;
    static int prevAssets = -1;

    // check for changed number of blocks we have, number of blocks peers claim to have, reindexing state and importing state
    if (cachedNumBlocks != newNumBlocks ||
        cachedReindexing != fReindex || cachedImporting != fImporting ||
        masternodeSync.RequestedMasternodeAttempt != prevAttempt || masternodeSync.RequestedMasternodeAssets != prevAssets) {
        cachedNumBlocks = newNumBlocks;
        cachedReindexing = fReindex;
        cachedImporting = fImporting;
        prevAttempt = masternodeSync.RequestedMasternodeAttempt;
        prevAssets = masternodeSync.RequestedMasternodeAssets;

        emit numBlocksChanged(newNumBlocks);
    }

    emit bytesChanged(getTotalBytesRecv(), getTotalBytesSent());
}

void ClientModel::updateMnTimer()
{
    QString newMasternodeCountString = getMasternodeCountString();

    if (cachedMasternodeCountString != newMasternodeCountString) {
        cachedMasternodeCountString = newMasternodeCountString;

        emit strMasternodesChanged(cachedMasternodeCountString);
    }
}

void ClientModel::updateNumConnections(int numConnections)
{
    emit numConnectionsChanged(numConnections);
}

void ClientModel::updateAlert(const QString& hash, int status)
{
    // Show error message notification for new alert
    if (status == CT_NEW) {
        uint256 hash_256;
        hash_256.SetHex(hash.toStdString());
        CAlert alert = CAlert::getAlertByHash(hash_256);
        if (!alert.IsNull()) {
            emit message(tr("Network Alert"), QString::fromStdString(alert.strStatusBar), CClientUIInterface::ICON_ERROR);
        }
    }

    emit alertsChanged(getStatusBarWarnings());
}

bool ClientModel::inInitialBlockDownload() const
{
    return IsInitialBlockDownload();
}

enum BlockSource ClientModel::getBlockSource() const
{
    if (fReindex)
        return BLOCK_SOURCE_REINDEX;
    else if (fImporting)
        return BLOCK_SOURCE_DISK;
    else if (getNumConnections() > 0)
        return BLOCK_SOURCE_NETWORK;

    return BLOCK_SOURCE_NONE;
}

QString ClientModel::getStatusBarWarnings() const
{
    return QString::fromStdString(GetWarnings("statusbar"));
}

OptionsModel* ClientModel::getOptionsModel()
{
    return optionsModel;
}

PeerTableModel* ClientModel::getPeerTableModel()
{
    return peerTableModel;
}

QString ClientModel::formatFullVersion() const
{
    return QString::fromStdString(FormatFullVersion());
}

QString ClientModel::formatBuildDate() const
{
    return QString::fromStdString(CLIENT_DATE);
}

bool ClientModel::isReleaseVersion() const
{
    return CLIENT_VERSION_IS_RELEASE;
}

QString ClientModel::clientName() const
{
    return QString::fromStdString(CLIENT_NAME);
}

QString ClientModel::formatClientStartupTime() const
{
    return QDateTime::fromTime_t(nClientStartupTime).toString();
}

// Handlers for core signals
static void ShowProgress(ClientModel* clientmodel, const std::string& title, int nProgress)
{
    // emits signal "showProgress"
    QMetaObject::invokeMethod(clientmodel, "showProgress", Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString(title)),
        Q_ARG(int, nProgress));
}

static void NotifyNumConnectionsChanged(ClientModel* clientmodel, int newNumConnections)
{
    // Too noisy: qDebug() << "NotifyNumConnectionsChanged : " + QString::number(newNumConnections);
    QMetaObject::invokeMethod(clientmodel, "updateNumConnections", Qt::QueuedConnection,
        Q_ARG(int, newNumConnections));
}

static void NotifyAlertChanged(ClientModel* clientmodel, const uint256& hash, ChangeType status)
{
    qDebug() << "NotifyAlertChanged : " + QString::fromStdString(hash.GetHex()) + " status=" + QString::number(status);
    QMetaObject::invokeMethod(clientmodel, "updateAlert", Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString(hash.GetHex())),
        Q_ARG(int, status));
}

void ClientModel::subscribeToCoreSignals()
{
    // Connect signals to client
    uiInterface.ShowProgress.connect(boost::bind(ShowProgress, this, _1, _2));
    uiInterface.NotifyNumConnectionsChanged.connect(boost::bind(NotifyNumConnectionsChanged, this, _1));
    uiInterface.NotifyAlertChanged.connect(boost::bind(NotifyAlertChanged, this, _1, _2));
}

void ClientModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    uiInterface.ShowProgress.disconnect(boost::bind(ShowProgress, this, _1, _2));
    uiInterface.NotifyNumConnectionsChanged.disconnect(boost::bind(NotifyNumConnectionsChanged, this, _1));
    uiInterface.NotifyAlertChanged.disconnect(boost::bind(NotifyAlertChanged, this, _1, _2));
}
