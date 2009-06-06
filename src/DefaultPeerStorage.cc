/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "DefaultPeerStorage.h"

#include <algorithm>

#include "LogFactory.h"
#include "Logger.h"
#include "message.h"
#include "a2time.h"
#include "Peer.h"
#include "BtContext.h"
#include "BtRuntime.h"
#include "BtSeederStateChoke.h"
#include "BtLeecherStateChoke.h"
#include "PieceStorage.h"

namespace aria2 {

static const int MAX_PEER_LIST_SIZE = 1024;

DefaultPeerStorage::DefaultPeerStorage(const BtContextHandle& btContext,
				       const Option* option):
  btContext(btContext),
  option(option),
  logger(LogFactory::getInstance()),
  removedPeerSessionDownloadLength(0),
  removedPeerSessionUploadLength(0),
  _seederStateChoke(new BtSeederStateChoke()),
  _leecherStateChoke(new BtLeecherStateChoke()),
  _lastTransferStatMapUpdated(0)
{}

DefaultPeerStorage::~DefaultPeerStorage()
{
  delete _seederStateChoke;
  delete _leecherStateChoke;
}

class FindIdenticalPeer {
private:
  PeerHandle _peer;
public:
  FindIdenticalPeer(const PeerHandle& peer):_peer(peer) {}

  bool operator()(const PeerHandle& peer) const {
    return (_peer == peer) ||
      ((_peer->ipaddr == peer->ipaddr) && (_peer->port == peer->port));
  }
};

bool DefaultPeerStorage::isPeerAlreadyAdded(const PeerHandle& peer)
{
  return std::find_if(peers.begin(), peers.end(), FindIdenticalPeer(peer)) != peers.end();
}

static size_t calculateMaxPeerListSize(const SharedHandle<BtRuntime>& btRuntime)
{
  if(btRuntime.isNull()) {
    return MAX_PEER_LIST_SIZE;
  }
  return btRuntime->getMaxPeers() == 0 ?
    MAX_PEER_LIST_SIZE :
    btRuntime->getMaxPeers()+(btRuntime->getMaxPeers() >> 2);
}

bool DefaultPeerStorage::addPeer(const PeerHandle& peer) {
  if(isPeerAlreadyAdded(peer)) {
    logger->debug("Adding %s:%u is rejected because it has been already added.", peer->ipaddr.c_str(), peer->port);
    return false;
  }
  size_t maxPeerListSize = calculateMaxPeerListSize(_btRuntime);
  if(peers.size() >= maxPeerListSize) {
    deleteUnusedPeer(peers.size()-maxPeerListSize+1);
  }
  peers.push_front(peer);
  return true;
}

void DefaultPeerStorage::addPeer(const Peers& peers) {
  for(Peers::const_iterator itr = peers.begin();
      itr != peers.end(); ++itr) {
    const PeerHandle& peer = *itr;
    if(addPeer(peer)) {
      logger->debug(MSG_ADDING_PEER,
		    peer->ipaddr.c_str(), peer->port);
    }
  }  
}

const Peers& DefaultPeerStorage::getPeers() {
  return peers;
}

class FindFinePeer {
public:
  bool operator()(const PeerHandle& peer) const {
    return peer->unused() && peer->isGood();
  }
};

PeerHandle DefaultPeerStorage::getUnusedPeer() {
  Peers::const_iterator itr = std::find_if(peers.begin(), peers.end(),
					   FindFinePeer());
  if(itr == peers.end()) {
    return SharedHandle<Peer>();
  } else {
    return *itr;
  }
}

class FindPeer {
private:
  std::string ipaddr;
  uint16_t port;
public:
  FindPeer(const std::string& ipaddr, uint16_t port):ipaddr(ipaddr), port(port) {}

  bool operator()(const PeerHandle& peer) const {
    return ipaddr == peer->ipaddr && port == peer->port;
  }
};

PeerHandle DefaultPeerStorage::getPeer(const std::string& ipaddr,
				       uint16_t port) const {
  Peers::const_iterator itr = std::find_if(peers.begin(), peers.end(),
					   FindPeer(ipaddr, port));
  if(itr == peers.end()) {
    return SharedHandle<Peer>();
  } else {
    return *itr;
  }
}

size_t DefaultPeerStorage::countPeer() const {
  return peers.size();
}

bool DefaultPeerStorage::isPeerAvailable() {
  return !getUnusedPeer().isNull();
}

class CollectActivePeer {
private:
  std::deque<SharedHandle<Peer> >& _activePeers;
public:
  CollectActivePeer(std::deque<SharedHandle<Peer> >& activePeers):
    _activePeers(activePeers) {}

  void operator()(const SharedHandle<Peer>& peer)
  {
    if(peer->isActive()) {
      _activePeers.push_back(peer);
    }
  }
};

void DefaultPeerStorage::getActivePeers(std::deque<SharedHandle<Peer> >& activePeers)
{
  std::for_each(peers.begin(), peers.end(), CollectActivePeer(activePeers));
}

static TransferStat calculateStatFor(const SharedHandle<Peer>& peer)
{
  struct timeval now;
  gettimeofday(&now, 0);
  TransferStat s;
  s.downloadSpeed = peer->calculateDownloadSpeed(now);
  s.uploadSpeed = peer->calculateUploadSpeed(now);
  s.sessionDownloadLength = peer->getSessionDownloadLength();
  s.sessionUploadLength = peer->getSessionUploadLength();
  return s;
}

TransferStat DefaultPeerStorage::calculateStat()
{
  TransferStat stat;
  if(_lastTransferStatMapUpdated.elapsedInMillis(250)) {
    logger->debug("Updating TransferStat of PeerStorage");
    _lastTransferStatMapUpdated.reset();
    _peerTransferStatMap.clear();
    std::deque<SharedHandle<Peer> > activePeers;
    getActivePeers(activePeers);
    struct timeval now;
    gettimeofday(&now, 0);
    for(std::deque<SharedHandle<Peer> >::const_iterator i = activePeers.begin();
	i != activePeers.end(); ++i) {
      TransferStat s;
      s.downloadSpeed = (*i)->calculateDownloadSpeed(now);
      s.uploadSpeed = (*i)->calculateUploadSpeed(now);
      s.sessionDownloadLength = (*i)->getSessionDownloadLength();
      s.sessionUploadLength = (*i)->getSessionUploadLength();

      _peerTransferStatMap[(*i)->getID()] = calculateStatFor(*i);
      stat += s;
    }
    _cachedTransferStat = stat;
  } else {
    stat = _cachedTransferStat;
  }
  stat.sessionDownloadLength += removedPeerSessionDownloadLength;
  stat.sessionUploadLength += removedPeerSessionUploadLength;
  stat.setAllTimeUploadLength(_btRuntime->getUploadLengthAtStartup()+
			      stat.getSessionUploadLength());
  return stat;
}

void DefaultPeerStorage::updateTransferStatFor(const SharedHandle<Peer>& peer)
{
  logger->debug("Updating TransferStat for peer %s", peer->getID().c_str());
  _cachedTransferStat -= _peerTransferStatMap[peer->getID()];
  TransferStat s = calculateStatFor(peer);
  _cachedTransferStat += s;
  _peerTransferStatMap[peer->getID()] = s;
}

TransferStat DefaultPeerStorage::getTransferStatFor
(const SharedHandle<Peer>& peer)
{
  return _peerTransferStatMap[peer->getID()];
}

void DefaultPeerStorage::deleteUnusedPeer(size_t delSize) {
  Peers temp;
  for(Peers::reverse_iterator itr = peers.rbegin();
      itr != peers.rend(); ++itr) {
    const PeerHandle& p = *itr;
    if(p->unused() && delSize > 0) {
      onErasingPeer(p);
      delSize--;
    } else {
      temp.push_front(p);
    }
  }
  peers = temp;
}

void DefaultPeerStorage::onErasingPeer(const SharedHandle<Peer>& peer) {}

void DefaultPeerStorage::onReturningPeer(const SharedHandle<Peer>& peer)
{
  if(peer->isActive()) {
    TransferStat removedStat(calculateStatFor(peer));
    removedPeerSessionDownloadLength += removedStat.getSessionDownloadLength();
    removedPeerSessionUploadLength += removedStat.getSessionUploadLength();
    _cachedTransferStat -= removedStat;

    // Execute choking algorithm if unchoked and interested peer is
    // disconnected.
    if(!peer->amChoking() && peer->peerInterested()) {
      executeChoke();
    }
  }
}

void DefaultPeerStorage::returnPeer(const PeerHandle& peer)
{
  Peers::iterator itr = std::find(peers.begin(), peers.end(), peer);
  if(itr == peers.end()) {
    logger->debug("Cannot find peer %s:%u in PeerStorage.", peer->ipaddr.c_str(), peer->port);
  } else {
    peers.erase(itr);

    onReturningPeer(peer);
    onErasingPeer(peer);
  }
}

bool DefaultPeerStorage::chokeRoundIntervalElapsed()
{
  const time_t CHOKE_ROUND_INTERVAL = 10;
  if(_pieceStorage->downloadFinished()) {
    return _seederStateChoke->getLastRound().elapsed(CHOKE_ROUND_INTERVAL);
  } else {
    return _leecherStateChoke->getLastRound().elapsed(CHOKE_ROUND_INTERVAL);
  }
}

void DefaultPeerStorage::executeChoke()
{
  std::deque<SharedHandle<Peer> > activePeers;
  getActivePeers(activePeers);
  if(_pieceStorage->downloadFinished()) {
    return _seederStateChoke->executeChoke(activePeers);
  } else {
    return _leecherStateChoke->executeChoke(activePeers);
  }
}

void DefaultPeerStorage::setPieceStorage(const SharedHandle<PieceStorage>& ps)
{
  _pieceStorage = ps;
}

void DefaultPeerStorage::setBtRuntime(const SharedHandle<BtRuntime>& btRuntime)
{
  _btRuntime = btRuntime;
}

} // namespace aria2
