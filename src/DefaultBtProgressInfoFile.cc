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
#include "DefaultBtProgressInfoFile.h"

#include <cerrno>
#include <cstring>
#include <fstream>

#include "BtContext.h"
#include "PieceStorage.h"
#include "Piece.h"
#ifdef ENABLE_BITTORRENT
#include "PeerStorage.h"
#include "BtRuntime.h"
#endif // ENABLE_BITTORRENT
#include "BitfieldMan.h"
#include "Option.h"
#include "TransferStat.h"
#include "LogFactory.h"
#include "Logger.h"
#include "prefs.h"
#include "DlAbortEx.h"
#include "message.h"
#include "File.h"
#include "Util.h"
#include "a2io.h"
#include "DownloadFailureException.h"
#include "StringFormat.h"

namespace aria2 {

const std::string DefaultBtProgressInfoFile::V0000("0000");
const std::string DefaultBtProgressInfoFile::V0001("0001");

static std::string createFilename(const SharedHandle<DownloadContext>& dctx)
{
  std::string t = dctx->getActualBasePath();
  t += ".aria2";
  return t;
}

DefaultBtProgressInfoFile::DefaultBtProgressInfoFile
(const DownloadContextHandle& dctx,
 const PieceStorageHandle& pieceStorage,
 const Option* option):
  _dctx(dctx),
  _pieceStorage(pieceStorage),
  _option(option),
  _logger(LogFactory::getInstance()),
  _filename(createFilename(_dctx))
{}

DefaultBtProgressInfoFile::~DefaultBtProgressInfoFile() {}

void DefaultBtProgressInfoFile::updateFilename()
{
  _filename = createFilename(_dctx);
}

bool DefaultBtProgressInfoFile::isTorrentDownload()
{
#ifdef ENABLE_BITTORRENT
  return !_btRuntime.isNull();
#else // !ENABLE_BITTORRENT
  return false;
#endif // !ENABLE_BITTORRENT
}

// Since version 0001, Integers are saved in binary form, network byte order.
void DefaultBtProgressInfoFile::save() {
  _logger->info(MSG_SAVING_SEGMENT_FILE, _filename.c_str());
  std::string filenameTemp = _filename+"__temp";
  std::ofstream o(filenameTemp.c_str(), std::ios::out|std::ios::binary);
  try {
    o.exceptions(std::ios::failbit);
    bool torrentDownload = isTorrentDownload();
    // file version: 16 bits
    // values: '1'
    char version[] = { 0x00, 0x01 };
    o.write(version, sizeof(version));
    // extension: 32 bits
    // If this is BitTorrent download, then 0x00000001
    // Otherwise, 0x00000000
    char extension[4];
    memset(extension, 0, sizeof(extension));
    if(torrentDownload) {
      extension[3] = 1;
    }
    o.write(reinterpret_cast<const char*>(&extension), sizeof(extension));
    if(torrentDownload) {
      // infoHashLength:
      // length: 32 bits
      BtContextHandle btContext(dynamic_pointer_cast<BtContext>(_dctx));
      uint32_t infoHashLengthNL = htonl(btContext->getInfoHashLength());
      o.write(reinterpret_cast<const char*>(&infoHashLengthNL),
	      sizeof(infoHashLengthNL));
      // infoHash:
      o.write(reinterpret_cast<const char*>(btContext->getInfoHash()),
	      btContext->getInfoHashLength());
    } else {
      // infoHashLength:
      // length: 32 bits
      uint32_t infoHashLength = 0;
      o.write(reinterpret_cast<const char*>(&infoHashLength),
	      sizeof(infoHashLength));
    }
    // pieceLength: 32 bits
    uint32_t pieceLengthNL = htonl(_dctx->getPieceLength());
    o.write(reinterpret_cast<const char*>(&pieceLengthNL),
	    sizeof(pieceLengthNL));
    // totalLength: 64 bits
    uint64_t totalLengthNL = hton64(_dctx->getTotalLength());
    o.write(reinterpret_cast<const char*>(&totalLengthNL),
	    sizeof(totalLengthNL));
    // uploadLength: 64 bits
    uint64_t uploadLengthNL = 0;
#ifdef ENABLE_BITTORRENT
    if(torrentDownload) {
      TransferStat stat = _peerStorage->calculateStat();
      uploadLengthNL = hton64(stat.getAllTimeUploadLength());
    }
#endif // ENABLE_BITTORRENT
    o.write(reinterpret_cast<const char*>(&uploadLengthNL),
	    sizeof(uploadLengthNL));
    // bitfieldLength: 32 bits
    uint32_t bitfieldLengthNL = htonl(_pieceStorage->getBitfieldLength());
    o.write(reinterpret_cast<const char*>(&bitfieldLengthNL),
	    sizeof(bitfieldLengthNL));
    // bitfield
    o.write(reinterpret_cast<const char*>(_pieceStorage->getBitfield()),
	    _pieceStorage->getBitfieldLength());
    // the number of in-flight piece: 32 bits
    // TODO implement this
    uint32_t numInFlightPieceNL = htonl(_pieceStorage->countInFlightPiece());
    o.write(reinterpret_cast<const char*>(&numInFlightPieceNL),
	    sizeof(numInFlightPieceNL));
    Pieces inFlightPieces;
    _pieceStorage->getInFlightPieces(inFlightPieces);
    for(Pieces::const_iterator itr = inFlightPieces.begin();
	itr != inFlightPieces.end(); ++itr) {
      uint32_t indexNL = htonl((*itr)->getIndex());
      o.write(reinterpret_cast<const char*>(&indexNL), sizeof(indexNL));
      uint32_t lengthNL = htonl((*itr)->getLength());
      o.write(reinterpret_cast<const char*>(&lengthNL), sizeof(lengthNL));
      uint32_t bitfieldLengthNL = htonl((*itr)->getBitfieldLength());
      o.write(reinterpret_cast<const char*>(&bitfieldLengthNL),
	      sizeof(bitfieldLengthNL));
      o.write(reinterpret_cast<const char*>((*itr)->getBitfield()),
	      (*itr)->getBitfieldLength());
    }

    o.close();
    _logger->info(MSG_SAVED_SEGMENT_FILE);
  } catch(std::ios::failure const& exception) {
    // TODO std::ios::failure doesn't give us the reasons of failure...
    throw DL_ABORT_EX(StringFormat(EX_SEGMENT_FILE_WRITE,
				 _filename.c_str(), strerror(errno)).str());
  }
  if(!File(filenameTemp).renameTo(_filename)) {
    throw DL_ABORT_EX(StringFormat(EX_SEGMENT_FILE_WRITE,
				 _filename.c_str(), strerror(errno)).str());
  }
}

// It is assumed that integers are saved as:
// 1) host byte order if version == 0000
// 2) network byte order if version == 0001
void DefaultBtProgressInfoFile::load() 
{
  _logger->info(MSG_LOADING_SEGMENT_FILE, _filename.c_str());
  std::ifstream in(_filename.c_str(), std::ios::in|std::ios::binary);
  unsigned char* savedInfoHash = 0;
  unsigned char* savedBitfield = 0;
  try {
    in.exceptions(std::ios::failbit);
    unsigned char versionBuf[2];
    in.read((char*)versionBuf, sizeof(versionBuf));
    std::string versionHex = Util::toHex(versionBuf, sizeof(versionBuf));
    int version;
    if(DefaultBtProgressInfoFile::V0000 == versionHex) {
      version = 0;
    } else if(DefaultBtProgressInfoFile::V0001 == versionHex) {
      version = 1;
    } else {
      throw DL_ABORT_EX
	(StringFormat("Unsupported ctrl file version: %s",
		      versionHex.c_str()).str());
    }
    unsigned char extension[4];
    in.read((char*)extension, sizeof(extension));

    bool infoHashCheckEnabled = false;
    if(extension[3]&1 && isTorrentDownload()) {
      infoHashCheckEnabled = true;
      _logger->debug("InfoHash checking enabled.");
    }

    uint32_t infoHashLength;
    in.read(reinterpret_cast<char*>(&infoHashLength), sizeof(infoHashLength));
    if(version >= 1) {
      infoHashLength = ntohl(infoHashLength);
    }
    if((infoHashLength < 0) ||
       ((infoHashLength == 0) && infoHashCheckEnabled)) {
      throw DL_ABORT_EX
	(StringFormat("Invalid info hash length: %d", infoHashLength).str());
    }
    if(infoHashLength > 0) {
      savedInfoHash = new unsigned char[infoHashLength];
      in.read(reinterpret_cast<char*>(savedInfoHash), infoHashLength);
      BtContextHandle btContext(dynamic_pointer_cast<BtContext>(_dctx));
      if(infoHashCheckEnabled &&
	 Util::toHex(savedInfoHash, infoHashLength) !=
	 btContext->getInfoHashAsString()) {
	throw DL_ABORT_EX
	  (StringFormat("info hash mismatch. expected: %s, actual: %s",
			btContext->getInfoHashAsString().c_str(),
			Util::toHex(savedInfoHash,
				    infoHashLength).c_str()).str());
      }
      delete [] savedInfoHash;
      savedInfoHash = 0;
    }

    uint32_t pieceLength;
    in.read(reinterpret_cast<char*>(&pieceLength), sizeof(pieceLength));
    if(version >= 1) {
      pieceLength = ntohl(pieceLength);
    }

    uint64_t totalLength;
    in.read(reinterpret_cast<char*>(&totalLength), sizeof(totalLength));
    if(version >= 1) {
      totalLength = ntoh64(totalLength);
    }
    if(totalLength != _dctx->getTotalLength()) {
      throw DL_ABORT_EX
	(StringFormat("total length mismatch. expected: %s, actual: %s",
		      Util::itos(_dctx->getTotalLength()).c_str(),
		      Util::itos(totalLength).c_str()).str());
    }
    uint64_t uploadLength;
    in.read(reinterpret_cast<char*>(&uploadLength), sizeof(uploadLength));
    if(version >= 1) {
      uploadLength = ntoh64(uploadLength);
    }
#ifdef ENABLE_BITTORRENT
    if(isTorrentDownload()) {
      _btRuntime->setUploadLengthAtStartup(uploadLength);
    }
#endif // ENABLE_BITTORRENT
    // TODO implement the conversion mechanism between different piece length.
    uint32_t bitfieldLength;
    in.read(reinterpret_cast<char*>(&bitfieldLength), sizeof(bitfieldLength));
    if(version >= 1) {
      bitfieldLength = ntohl(bitfieldLength);
    }
    uint32_t expectedBitfieldLength =
      ((totalLength+pieceLength-1)/pieceLength+7)/8;
    if(expectedBitfieldLength != bitfieldLength) {
      throw DL_ABORT_EX
	(StringFormat("bitfield length mismatch. expected: %d, actual: %d",
		      expectedBitfieldLength,
		      bitfieldLength).str());
    }

    savedBitfield = new unsigned char[bitfieldLength];
    in.read(reinterpret_cast<char*>(savedBitfield), bitfieldLength);

    if(pieceLength == _dctx->getPieceLength()) {
      _pieceStorage->setBitfield(savedBitfield, bitfieldLength);
      delete [] savedBitfield;
      savedBitfield = 0;

      uint32_t numInFlightPiece;
      in.read(reinterpret_cast<char*>(&numInFlightPiece),
	      sizeof(numInFlightPiece));
      if(version >= 1) {
	numInFlightPiece = ntohl(numInFlightPiece);
      }
      Pieces inFlightPieces;
      while(numInFlightPiece--) {
	uint32_t index;
	in.read(reinterpret_cast<char*>(&index), sizeof(index));
	if(version >= 1) {
	  index = ntohl(index);
	}
	if(!(index < _dctx->getNumPieces())) {
	  throw DL_ABORT_EX
	    (StringFormat("piece index out of range: %u", index).str());
	}
	uint32_t length;
	in.read(reinterpret_cast<char*>(&length), sizeof(length));
	if(version >= 1) {
	  length = ntohl(length);
	}
	if(!(length <=_dctx->getPieceLength())) {
	  throw DL_ABORT_EX
	    (StringFormat("piece length out of range: %u", length).str());
	}
	PieceHandle piece(new Piece(index, length));
	uint32_t bitfieldLength;
	in.read(reinterpret_cast<char*>(&bitfieldLength),
		sizeof(bitfieldLength));
	if(version >= 1) {
	  bitfieldLength = ntohl(bitfieldLength);
	}
	if(piece->getBitfieldLength() != bitfieldLength) {
	  throw DL_ABORT_EX
	    (StringFormat("piece bitfield length mismatch."
			  " expected: %u actual: %u",
			  piece->getBitfieldLength(), bitfieldLength).str());
	}
	savedBitfield = new unsigned char[bitfieldLength];
	in.read(reinterpret_cast<char*>(savedBitfield), bitfieldLength);
	piece->setBitfield(savedBitfield, bitfieldLength);

#ifdef ENABLE_MESSAGE_DIGEST

	piece->setHashAlgo(_dctx->getPieceHashAlgo());

#endif // ENABLE_MESSAGE_DIGEST

	delete [] savedBitfield;
	savedBitfield = 0;
	
	inFlightPieces.push_back(piece);
      }
      _pieceStorage->addInFlightPiece(inFlightPieces);
    } else {
      uint32_t numInFlightPiece;
      in.read(reinterpret_cast<char*>(&numInFlightPiece),
	      sizeof(numInFlightPiece));
      if(version >= 1) {
	numInFlightPiece = ntohl(numInFlightPiece);
      }
      BitfieldMan src(pieceLength, totalLength);
      src.setBitfield(savedBitfield, bitfieldLength);
      if((src.getCompletedLength() || numInFlightPiece) &&
	 !_option->getAsBool(PREF_ALLOW_PIECE_LENGTH_CHANGE)) {
	throw DOWNLOAD_FAILURE_EXCEPTION
	  ("WARNING: Detected a change in piece length. You can proceed with"
	   " --allow-piece-length-change=true, but you may lose some download"
	   " progress.");
      }
      BitfieldMan dest(_dctx->getPieceLength(), totalLength);
      Util::convertBitfield(&dest, &src);
      _pieceStorage->setBitfield(dest.getBitfield(), dest.getBitfieldLength());
      delete [] savedBitfield;
      savedBitfield = 0;
    }
    _logger->info(MSG_LOADED_SEGMENT_FILE);
  } catch(std::ios::failure const& exception) {
    delete [] savedBitfield;
    delete [] savedInfoHash;
    // TODO std::ios::failure doesn't give us the reasons of failure...
    throw DL_ABORT_EX(StringFormat(EX_SEGMENT_FILE_READ,
				 _filename.c_str(), strerror(errno)).str());
  } 
}

void DefaultBtProgressInfoFile::removeFile()
{
  if(exists()) {
    File f(_filename);
    f.remove();
  }
}

bool DefaultBtProgressInfoFile::exists()
{
  File f(_filename);
  if(f.isFile()) {
    _logger->info(MSG_SEGMENT_FILE_EXISTS, _filename.c_str());
    return true;
  } else {
    _logger->info(MSG_SEGMENT_FILE_DOES_NOT_EXIST, _filename.c_str());
    return false;
  }
}

#ifdef ENABLE_BITTORRENT
void DefaultBtProgressInfoFile::setPeerStorage
(const SharedHandle<PeerStorage>& peerStorage)
{
  _peerStorage = peerStorage;
}

void DefaultBtProgressInfoFile::setBtRuntime
(const SharedHandle<BtRuntime>& btRuntime)
{
  _btRuntime = btRuntime;
}
#endif // ENABLE_BITTORRENT

} // namespace aria2
