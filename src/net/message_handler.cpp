#include “net/message_handler.h”
#include “crypto/keccak256.h”

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace net {

// =============================================================================
// CONSTRUCTOR
// =============================================================================
MessageHandler::MessageHandler(Config                       cfg,
Blockchain&                  chain,
Mempool&                     mempool,
std::shared_ptr<PeerManager> peerMgr)
: cfg_(std::move(cfg))
, chain_(chain)
, mempool_(mempool)
, peerMgr_(std::move(peerMgr))
{}

// =============================================================================
// CALLBACK SETTERS
// =============================================================================
void MessageHandler::setSendFn     (SendFn fn)      noexcept { std::lock_guard<std::mutex> l(sendMu_);      sendFn_      = std::move(fn); }
void MessageHandler::setBroadcastFn(BroadcastFn fn) noexcept { std::lock_guard<std::mutex> l(broadcastMu_); broadcastFn_ = std::move(fn); }
void MessageHandler::setLogFn      (LogFn fn)       noexcept { std::lock_guard<std::mutex> l(logMu_);       logFn_       = std::move(fn); }
void MessageHandler::onBlock       (BlockFn fn)     noexcept { std::lock_guard<std::mutex> l(blockMu_);     blockFn_     = std::move(fn); }
void MessageHandler::onTransaction (TxFn fn)        noexcept { std::lock_guard<std::mutex> l(txMu_);        txFn_        = std::move(fn); }

// =============================================================================
// STRUCTURED LOGGER
// =============================================================================
void MessageHandler::slog(int level, const std::string& msg) const noexcept {
std::lock_guard<std::mutex> lk(logMu_);
if (logFn_) {
try { logFn_(level, “[MessageHandler] “ + msg); }
catch (const std::exception& e) {
std::cerr << “[MessageHandler] logFn_ threw: “
<< e.what() << “\n”;
} catch (…) {
std::cerr << “[MessageHandler] logFn_ threw unknown\n”;
}
return;
}
if (level >= 1)
std::cerr << “[MessageHandler] “ << msg << “\n”;
}

// =============================================================================
// SAFE CALLBACK INVOKERS
// =============================================================================
void MessageHandler::invokeSend(const std::string& peerId,
std::vector<uint8_t> frame) const noexcept {
SendFn fn;
{ std::lock_guard<std::mutex> lk(sendMu_); fn = sendFn_; }
if (!fn) return;
try { fn(peerId, std::move(frame)); }
catch (const std::exception& e) {
slog(2, “sendFn_ threw for peer “ + peerId + “: “ + e.what());
} catch (…) {
slog(2, “sendFn_ threw unknown for peer “ + peerId);
}
}

void MessageHandler::invokeBroadcast(MsgType type,
std::vector<uint8_t> payload,
const std::string& excludeId) const noexcept {
BroadcastFn fn;
{ std::lock_guard<std::mutex> lk(broadcastMu_); fn = broadcastFn_; }
if (!fn) return;
try { fn(type, std::move(payload), excludeId); }
catch (const std::exception& e) {
slog(2, std::string(“broadcastFn_ threw: “) + e.what());
} catch (…) {
slog(2, “broadcastFn_ threw unknown”);
}
}

void MessageHandler::invokeBlock(const Block& b,
const std::string& peerId) const noexcept {
BlockFn fn;
{ std::lock_guard<std::mutex> lk(blockMu_); fn = blockFn_; }
if (!fn) return;
try { fn(b, peerId); }
catch (const std::exception& e) {
slog(2, “blockFn_ threw for peer “ + peerId + “: “ + e.what());
} catch (…) {
slog(2, “blockFn_ threw unknown for peer “ + peerId);
}
}

void MessageHandler::invokeTx(const Transaction& tx,
const std::string& peerId) const noexcept {
TxFn fn;
{ std::lock_guard<std::mutex> lk(txMu_); fn = txFn_; }
if (!fn) return;
try { fn(tx, peerId); }
catch (const std::exception& e) {
slog(2, “txFn_ threw for peer “ + peerId + “: “ + e.what());
} catch (…) {
slog(2, “txFn_ threw unknown for peer “ + peerId);
}
}

// =============================================================================
// WIRE SERIALIZATION HELPERS
// =============================================================================
void MessageHandler::writeUint32BE(std::vector<uint8_t>& buf,
uint32_t v) noexcept {
buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
buf.push_back(static_cast<uint8_t>((v >>  8) & 0xFF));
buf.push_back(static_cast<uint8_t>( v        & 0xFF));
}

void MessageHandler::writeUint64BE(std::vector<uint8_t>& buf,
uint64_t v) noexcept {
for (int i = 7; i >= 0; i–)
buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

uint32_t MessageHandler::readUint32BE(const uint8_t* p) noexcept {
return (static_cast<uint32_t>(p[0]) << 24) |
(static_cast<uint32_t>(p[1]) << 16) |
(static_cast<uint32_t>(p[2]) <<  8) |
static_cast<uint32_t>(p[3]);
}

uint64_t MessageHandler::readUint64BE(const uint8_t* p) noexcept {
uint64_t v = 0;
for (int i = 0; i < 8; i++)
v = (v << 8) | static_cast<uint64_t>(p[i]);
return v;
}

void MessageHandler::writeString(std::vector<uint8_t>& buf,
const std::string& s) noexcept {
writeUint32BE(buf, static_cast<uint32_t>(s.size()));
buf.insert(buf.end(), s.begin(), s.end());
}

std::string MessageHandler::readString(const uint8_t* p,
size_t& offset,
size_t max) noexcept {
if (offset + 4 > max) { offset = max; return {}; }
uint32_t len = readUint32BE(p + offset);
offset += 4;
if (len > max - offset) { offset = max; return {}; }
if (len > MAX_PAYLOAD_SIZE) { offset = max; return {}; }
std::string s(reinterpret_cast<const char*>(p + offset), len);
offset += len;
return s;
}

// =============================================================================
// FRAME ENCODE
// =============================================================================
std::vector<uint8_t> MessageHandler::encodeFrame(
MsgType                     type,
const std::vector<uint8_t>& payload,
const std::string&          magic) noexcept
{
try {
if (payload.size() > MAX_PAYLOAD_SIZE) return {};
std::vector<uint8_t> frame;
frame.reserve(FRAME_HEADER_SIZE + payload.size());
for (size_t i = 0; i < 4; i++)
frame.push_back(i < magic.size()
? static_cast<uint8_t>(magic[i]) : 0);
frame.push_back(static_cast<uint8_t>(type));
uint32_t payLen = static_cast<uint32_t>(payload.size());
frame.push_back(static_cast<uint8_t>((payLen >> 24) & 0xFF));
frame.push_back(static_cast<uint8_t>((payLen >> 16) & 0xFF));
frame.push_back(static_cast<uint8_t>((payLen >>  8) & 0xFF));
frame.push_back(static_cast<uint8_t>( payLen        & 0xFF));
frame.insert(frame.end(), payload.begin(), payload.end());
return frame;
} catch (…) { return {}; }
}

// =============================================================================
// FRAME DECODE
// =============================================================================
FrameDecodeResult MessageHandler::decodeFrame(
const std::vector<uint8_t>& raw,
const std::string&          magic,
WireFrame&                  out) noexcept
{
FrameDecodeResult r;

```
if (raw.size() < FRAME_HEADER_SIZE) {
    r.error = FrameDecodeError::TooShort;
    return r;
}

for (size_t i = 0; i < 4; i++) {
    uint8_t expected = i < magic.size()
                       ? static_cast<uint8_t>(magic[i]) : 0;
    if (raw[i] != expected) {
        r.error = FrameDecodeError::BadMagic;
        return r;
    }
}

auto type = static_cast<MsgType>(raw[4]);
if (type == MsgType::Unknown) {
    r.error = FrameDecodeError::BadMsgType;
    return r;
}

uint32_t payLen = readUint32BE(raw.data() + 5);

if (payLen > MAX_PAYLOAD_SIZE) {
    r.error = FrameDecodeError::OversizedFrame;
    return r;
}
if (raw.size() < FRAME_HEADER_SIZE + payLen) {
    r.error = FrameDecodeError::PayloadTrunc;
    return r;
}

try {
    out.type = type;
    out.payload.assign(raw.begin() + FRAME_HEADER_SIZE,
                       raw.begin() + FRAME_HEADER_SIZE + payLen);
    r.ok = true;
} catch (const std::bad_alloc&) {
    r.error = FrameDecodeError::AllocFailed;
} catch (...) {
    r.error = FrameDecodeError::Unknown;
}
return r;
```

}

// =============================================================================
// HANDLE RAW
// =============================================================================
void MessageHandler::handleRaw(const std::string&          peerId,
const std::vector<uint8_t>& raw)
{
if (!peerMgr_->checkRateLimit(peerId, raw.size())) {
slog(1, “rate limit exceeded from peer “ + peerId);
peerMgr_->penalizePeer(peerId, 5.0);
return;
}

```
WireFrame frame;
auto result = decodeFrame(raw, cfg_.networkMagic, frame);
if (!result.ok) {
    slog(1, "decode failure from peer " + peerId
            + ": " + result.errorName());
    peerMgr_->recordDecodeError(peerId);
    peerMgr_->penalizePeer(peerId, 3.0);
    return;
}

if (frame.payload.size() > cfg_.maxPayloadBytes) {
    slog(1, "payload too large from peer " + peerId
            + " size=" + std::to_string(frame.payload.size()));
    peerMgr_->penalizePeer(peerId, 10.0);
    return;
}

std::string msgId(reinterpret_cast<const char*>(raw.data()),
                  std::min(raw.size(), size_t(32)));
if (!peerMgr_->markSeen(msgId)) {
    slog(2, "replay detected from peer " + peerId);
    return;
}

peerMgr_->rewardPeer(peerId, 0.1);

try {
    switch (frame.type) {
        case MsgType::Handshake:
            handleHandshake(peerId, frame.payload);    break;
        case MsgType::HandshakeAck:
            handleHandshakeAck(peerId, frame.payload); break;
        case MsgType::Ping:
            handlePing(peerId);                        break;
        case MsgType::Pong:
            handlePong(peerId);                        break;
        case MsgType::GetPeers:
            handleGetPeers(peerId);                    break;
        case MsgType::Peers:
            handlePeers(peerId, frame.payload);        break;
        case MsgType::Block:
            handleBlock(peerId, frame.payload);        break;
        case MsgType::Transaction:
            handleTransaction(peerId, frame.payload);  break;
        case MsgType::TxBatch:
            handleTxBatch(peerId, frame.payload);      break;
        case MsgType::GetHeaders:
            handleGetHeaders(peerId, frame.payload);   break;
        case MsgType::Headers:
            handleHeaders(peerId, frame.payload);      break;
        case MsgType::Reject:
            handleReject(peerId, frame.payload);       break;
        default:
            slog(1, "unknown MsgType from peer " + peerId);
            peerMgr_->penalizePeer(peerId, 2.0);
    }
} catch (const std::exception& e) {
    slog(2, "dispatch exception from peer " + peerId
            + ": " + e.what());
    peerMgr_->penalizePeer(peerId, 5.0);
} catch (...) {
    slog(2, "dispatch unknown exception from peer " + peerId);
    peerMgr_->penalizePeer(peerId, 5.0);
}
```

}

// =============================================================================
// HANDLERS
// =============================================================================
void MessageHandler::handleHandshake(const std::string& peerId,
const std::vector<uint8_t>& payload)
{
if (payload.size() < 8) {
slog(1, “malformed Handshake from “ + peerId);
peerMgr_->penalizePeer(peerId, 10.0);
return;
}
size_t off = 0;
uint32_t ver = readUint32BE(payload.data()); off += 4;
std::string magic  = readString(payload.data(), off, payload.size());
std::string nodeId = readString(payload.data(), off, payload.size());

```
if (off > payload.size()) {
    slog(1, "Handshake parse overflow from " + peerId);
    peerMgr_->penalizePeer(peerId, 10.0);
    return;
}

if (ver < cfg_.minPeerVersion || ver > cfg_.maxPeerVersion) {
    slog(1, "Handshake version mismatch from " + peerId
            + " ver=" + std::to_string(ver));
    auto reject = buildReject("incompatible protocol version");
    invokeSend(peerId, std::move(reject));
    peerMgr_->penalizePeer(peerId, 20.0);
    return;
}

if (magic != cfg_.networkMagic) {
    slog(1, "Handshake wrong network from " + peerId);
    auto reject = buildReject("wrong network");
    invokeSend(peerId, std::move(reject));
    peerMgr_->banPeer(peerId);
    return;
}

peerMgr_->rewardPeer(peerId, 5.0);
auto ack = buildHandshake();
invokeSend(peerId, std::move(ack));
slog(0, "Handshake done with " + peerId
        + " ver=" + std::to_string(ver));
```

}

void MessageHandler::handleHandshakeAck(const std::string& peerId,
const std::vector<uint8_t>&)
{
peerMgr_->rewardPeer(peerId, 2.0);
slog(0, “HandshakeAck from “ + peerId);
}

void MessageHandler::handlePing(const std::string& peerId) {
auto pong = buildPong();
invokeSend(peerId, std::move(pong));
}

void MessageHandler::handlePong(const std::string& peerId) {
peerMgr_->rewardPeer(peerId, 0.5);
slog(2, “Pong from “ + peerId);
}

void MessageHandler::handleGetPeers(const std::string& peerId) {
auto peers = peerMgr_->getActivePeers();
std::vector<uint8_t> payload;
writeUint32BE(payload, static_cast<uint32_t>(
std::min(peers.size(), size_t(200))));
for (size_t i = 0; i < peers.size() && i < 200; i++) {
writeString(payload, peers[i].address);
payload.push_back(static_cast<uint8_t>((peers[i].port >> 8) & 0xFF));
payload.push_back(static_cast<uint8_t>( peers[i].port       & 0xFF));
}
auto frame = encodeFrame(MsgType::Peers, payload, cfg_.networkMagic);
invokeSend(peerId, std::move(frame));
}

void MessageHandler::handlePeers(const std::string& peerId,
const std::vector<uint8_t>& payload)
{
if (payload.size() < 4) return;
size_t off = 0;
uint32_t count = readUint32BE(payload.data()); off += 4;
count = std::min(count, uint32_t(200));

```
for (uint32_t i = 0; i < count && off < payload.size(); i++) {
    std::string addr = readString(payload.data(), off, payload.size());
    if (off + 2 > payload.size()) break;
    uint16_t port = static_cast<uint16_t>(
        (static_cast<uint16_t>(payload[off]) << 8)
        | static_cast<uint16_t>(payload[off + 1]));
    off += 2;
    if (addr.empty() || port == 0) continue;
    slog(2, "peer hint " + addr + ":" + std::to_string(port)
            + " from " + peerId);
}
peerMgr_->rewardPeer(peerId, 0.5);
```

}

void MessageHandler::handleBlock(const std::string& peerId,
const std::vector<uint8_t>& payload)
{
Block block;
try {
std::string raw(payload.begin(), payload.end());
block.deserialize(raw);
} catch (const std::exception& e) {
slog(1, “block deserialize failed from “ + peerId
+ “: “ + e.what());
peerMgr_->penalizePeer(peerId, 10.0);
peerMgr_->recordDecodeError(peerId);
return;
}

```
if (block.hash.empty()) {
    slog(1, "empty block hash from " + peerId);
    peerMgr_->penalizePeer(peerId, 10.0);
    return;
}

if (!chain_.isOpen()) {
    slog(1, "blockchain not ready - dropping block from " + peerId);
    return;
}

slog(0, "received block hash=" + block.hash + " from " + peerId);
peerMgr_->rewardPeer(peerId, 2.0);
invokeBlock(block, peerId);

invokeBroadcast(MsgType::Block, payload, peerId);
```

}

void MessageHandler::handleTransaction(const std::string& peerId,
const std::vector<uint8_t>& payload)
{
Transaction tx;
try {
std::istringstream ss(std::string(payload.begin(), payload.end()));
std::string tok;
auto next = [&]() -> std::string {
std::getline(ss, tok, ‘|’); return tok;
};
tx.chainId              = std::stoull(next());
tx.nonce                = std::stoull(next());
tx.toAddress            = next();
tx.value                = std::stoull(next());
tx.gasLimit             = std::stoull(next());
tx.maxFeePerGas         = std::stoull(next());
tx.maxPriorityFeePerGas = std::stoull(next());
tx.txHash               = next();
} catch (const std::exception& e) {
slog(1, “tx deserialize failed from “ + peerId
+ “: “ + e.what());
peerMgr_->penalizePeer(peerId, 5.0);
peerMgr_->recordDecodeError(peerId);
return;
}

```
if (!tx.isValid()) {
    slog(1, "invalid tx from " + peerId + " hash=" + tx.txHash);
    peerMgr_->penalizePeer(peerId, 5.0);
    return;
}

uint64_t baseFee = chain_.baseFee();
if (!mempool_.addTransaction(tx, baseFee)) {
    slog(2, "tx rejected by mempool: " + tx.txHash);
    return;
}

peerMgr_->rewardPeer(peerId, 0.5);
invokeTx(tx, peerId);
invokeBroadcast(MsgType::Transaction, payload, peerId);
```

}

void MessageHandler::handleTxBatch(const std::string& peerId,
const std::vector<uint8_t>& payload)
{
if (payload.size() < 4) return;
uint32_t count = readUint32BE(payload.data());
count = std::min(count, uint32_t(1000));
size_t off = 4;

```
for (uint32_t i = 0; i < count && off + 4 <= payload.size(); i++) {
    uint32_t txLen = readUint32BE(payload.data() + off); off += 4;
    if (txLen > payload.size() - off) break;
    std::vector<uint8_t> txPayload(
        payload.begin() + static_cast<ptrdiff_t>(off),
        payload.begin() + static_cast<ptrdiff_t>(off + txLen));
    off += txLen;
    handleTransaction(peerId, txPayload);
}
```

}

void MessageHandler::handleGetHeaders(const std::string& peerId,
const std::vector<uint8_t>& payload)
{
if (payload.size() < 8) return;
uint64_t fromHeight  = readUint64BE(payload.data());
uint64_t chainHeight = chain_.height();

```
std::vector<uint8_t> response;
uint64_t count = 0;
if (fromHeight <= chainHeight)
    count = std::min(chainHeight - fromHeight + 1, uint64_t(2000));
writeUint64BE(response, count);
for (uint64_t h = fromHeight; h < fromHeight + count; h++)
    writeUint64BE(response, h);

auto frame = encodeFrame(MsgType::Headers, response, cfg_.networkMagic);
invokeSend(peerId, std::move(frame));
```

}

void MessageHandler::handleHeaders(const std::string& peerId,
const std::vector<uint8_t>& payload)
{
if (payload.size() < 8) return;
uint64_t count = readUint64BE(payload.data());
slog(2, “received “ + std::to_string(count)
+ “ headers from “ + peerId);
peerMgr_->rewardPeer(peerId, 1.0);
}

void MessageHandler::handleReject(const std::string& peerId,
const std::vector<uint8_t>& payload)
{
size_t off = 0;
std::string reason = readString(payload.data(), off, payload.size());
if (reason.empty()) reason = “(no reason)”;
slog(1, “REJECT from “ + peerId + “: “ + reason);
peerMgr_->penalizePeer(peerId, 1.0);
}

// =============================================================================
// OUTBOUND BUILDERS
// =============================================================================
std::vector<uint8_t> MessageHandler::buildHandshake() const {
std::vector<uint8_t> payload;
writeUint32BE(payload, cfg_.protocolVersion);
writeString(payload, cfg_.networkMagic);
writeString(payload, cfg_.nodeId);
return encodeFrame(MsgType::Handshake, payload, cfg_.networkMagic);
}

std::vector<uint8_t> MessageHandler::buildPing() const {
return encodeFrame(MsgType::Ping, {}, cfg_.networkMagic);
}

std::vector<uint8_t> MessageHandler::buildPong() const {
return encodeFrame(MsgType::Pong, {}, cfg_.networkMagic);
}

std::vector<uint8_t> MessageHandler::buildGetPeers() const {
return encodeFrame(MsgType::GetPeers, {}, cfg_.networkMagic);
}

std::vector<uint8_t> MessageHandler::buildBlock(const Block& b) const {
std::string s = b.serialize();
std::vector<uint8_t> payload(s.begin(), s.end());
return encodeFrame(MsgType::Block, payload, cfg_.networkMagic);
}

std::vector<uint8_t> MessageHandler::buildTx(const Transaction& tx) const {
std::ostringstream ss;
ss << tx.chainId << “|” << tx.nonce << “|”
<< tx.toAddress << “|” << tx.value << “|”
<< tx.gasLimit << “|” << tx.maxFeePerGas << “|”
<< tx.maxPriorityFeePerGas << “|” << tx.txHash;
std::string s = ss.str();
std::vector<uint8_t> payload(s.begin(), s.end());
return encodeFrame(MsgType::Transaction, payload, cfg_.networkMagic);
}

std::vector<uint8_t> MessageHandler::buildTxBatch(
const std::vector<Transaction>& txs) const
{
std::vector<uint8_t> payload;
writeUint32BE(payload, static_cast<uint32_t>(txs.size()));
for (const auto& tx : txs) {
auto txFrame = buildTx(tx);
writeUint32BE(payload, static_cast<uint32_t>(txFrame.size()));
payload.insert(payload.end(), txFrame.begin(), txFrame.end());
}
return encodeFrame(MsgType::TxBatch, payload, cfg_.networkMagic);
}

std::vector<uint8_t> MessageHandler::buildGetHeaders(
uint64_t fromHeight) const
{
std::vector<uint8_t> payload;
writeUint64BE(payload, fromHeight);
return encodeFrame(MsgType::GetHeaders, payload, cfg_.networkMagic);
}

std::vector<uint8_t> MessageHandler::buildReject(
const std::string& reason) const
{
std::vector<uint8_t> payload;
writeString(payload, reason);
return encodeFrame(MsgType::Reject, payload, cfg_.networkMagic);
}

} // namespace net
