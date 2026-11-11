#pragma once
// The wire formats of internet play's matchmaking relay (online_relay.cpp): the topic a join code meets on, the
// endpoint message two consoles swap there, and the few MQTT 3.1.1 packets the client needs.
//
// The relay is any public MQTT broker. Players never meet a server of ours: the host and the joiners of one code
// subscribe to topics made from that code and post where the internet sees each of their UDP ports, and then send
// datagrams straight to each other (online_relay.h).
//
// Privacy: the topic is a hash of the code, and the message is XORed with a keystream made from the code. That only
// keeps a casual reader of a public broker from seeing addresses in the clear; anyone who knows or guesses a code can
// read its messages. Nothing in them is secret beyond the public addresses the consoles exchange anyway.
//
// Header-only and free of the runtime's other headers, so the codec test builds it on its own.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace rex::system::relay_wire {

// ---- Hashing ----------------------------------------------------------------------------------------------------

inline uint64_t Mix(uint64_t x) {  // splitmix64's finalizer
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// FNV-1a over the text, finished with Mix.
inline uint64_t Hash64(std::string_view text) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (const char c : text) {
    h ^= uint8_t(c);
    h *= 0x100000001B3ull;
  }
  return Mix(h);
}

inline std::string Hex64(uint64_t value) {
  static const char digits[] = "0123456789abcdef";
  std::string text(16, '0');
  for (int i = 15; i >= 0; --i, value >>= 4) text[size_t(i)] = digits[value & 15];
  return text;
}

// ---- Topics -----------------------------------------------------------------------------------------------------

constexpr std::string_view kTopicRoot = "bo2zmnx/1/";

// The base topic for a code (its plain form, join_code::Plain): "bo2zmnx/1/<16 hex digits>". The host posts on
// <base>/host (retained), the joiners on <base>/join.
inline std::string Topic(std::string_view plain_code) {
  return std::string(kTopicRoot) + Hex64(Hash64(std::string("bo2zmnx topic|") + std::string(plain_code)));
}
inline std::string HostTopic(std::string_view plain_code) { return Topic(plain_code) + "/host"; }
inline std::string JoinTopic(std::string_view plain_code) { return Topic(plain_code) + "/join"; }

// XORs data with the keystream for this code and message nonce (applying it twice restores the data).
inline void Keystream(std::string_view plain_code, uint64_t nonce, uint8_t* data, size_t size) {
  const uint64_t seed = Hash64(std::string("bo2zmnx key|") + std::string(plain_code)) ^ Mix(nonce);
  uint64_t word = 0;
  for (size_t i = 0; i < size; ++i) {
    if (i % 8 == 0) word = Mix(seed + uint64_t(i / 8) * 0xD1B54A32D192ED03ull);
    data[i] ^= uint8_t(word >> (8 * (i % 8)));
  }
}

// ---- The endpoint message -----------------------------------------------------------------------------------------

constexpr uint8_t kMagic = 'B';
constexpr uint8_t kWireVersion = 1;
constexpr int kMaxEndpoints = 4;
enum Role : uint8_t { kHost = 1, kJoiner = 2 };

struct Endpoint {
  uint16_t local_port = 0;  // this console's port (host byte order, as bound: 31000, 31001, 31002)
  uint32_t ip = 0;          // where the internet sees it
  uint16_t port = 0;
  bool from_stun = false;   // seen by a STUN server; otherwise assumed (public address, same port)
};

struct Message {
  uint8_t role = 0;
  uint64_t session = 0;   // a random number per hosting or joining
  uint32_t public_ip = 0;
  int count = 0;
  std::array<Endpoint, kMaxEndpoints> endpoints{};
};

namespace detail {
inline void Put16(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}
inline void Put32(std::vector<uint8_t>& out, uint32_t v) {
  Put16(out, v >> 16);
  Put16(out, v);
}
inline void Put64(std::vector<uint8_t>& out, uint64_t v) {
  Put32(out, uint32_t(v >> 32));
  Put32(out, uint32_t(v));
}
inline uint64_t Get(const uint8_t* p, int bytes) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; ++i) v = (v << 8) | p[i];
  return v;
}
inline uint32_t BodyCheck(std::string_view plain_code, const uint8_t* body, size_t size) {
  std::string text("bo2zmnx check|");
  text += plain_code;
  text.append(reinterpret_cast<const char*>(body), size);
  return uint32_t(Hash64(text));
}
}  // namespace detail

// [magic][version][nonce: 8, in the clear] then, through the keystream: role, session (8), public address (4), count,
// count x {local port (2), address (4), port (2), flags (1)}, and a 32-bit check of all that and the code.
inline std::vector<uint8_t> Encode(const Message& m, std::string_view plain_code, uint64_t nonce) {
  std::vector<uint8_t> out = {kMagic, kWireVersion};
  detail::Put64(out, nonce);
  const size_t body_at = out.size();
  out.push_back(m.role);
  detail::Put64(out, m.session);
  detail::Put32(out, m.public_ip);
  const int count = m.count < 0 ? 0 : (m.count > kMaxEndpoints ? kMaxEndpoints : m.count);
  out.push_back(uint8_t(count));
  for (int i = 0; i < count; ++i) {
    const Endpoint& e = m.endpoints[size_t(i)];
    detail::Put16(out, e.local_port);
    detail::Put32(out, e.ip);
    detail::Put16(out, e.port);
    out.push_back(e.from_stun ? 1 : 0);
  }
  detail::Put32(out, detail::BodyCheck(plain_code, out.data() + body_at, out.size() - body_at));
  Keystream(plain_code, nonce, out.data() + body_at, out.size() - body_at);
  return out;
}

// false for anything that is not a message of this version made with this code.
inline bool Decode(const uint8_t* data, size_t size, std::string_view plain_code, Message& m) {
  constexpr size_t kHead = 10, kFixed = 1 + 8 + 4 + 1, kEach = 9;
  if (size < kHead + kFixed + 4 || data[0] != kMagic || data[1] != kWireVersion || size > 256) return false;
  const uint64_t nonce = detail::Get(data + 2, 8);
  std::vector<uint8_t> body(data + kHead, data + size);
  Keystream(plain_code, nonce, body.data(), body.size());
  const size_t count = body[13];
  if (count > size_t(kMaxEndpoints) || body.size() != kFixed + count * kEach + 4) return false;
  const size_t checked = body.size() - 4;
  if (detail::BodyCheck(plain_code, body.data(), checked) != uint32_t(detail::Get(body.data() + checked, 4))) {
    return false;
  }
  m = {};
  m.role = body[0];
  m.session = detail::Get(body.data() + 1, 8);
  m.public_ip = uint32_t(detail::Get(body.data() + 9, 4));
  m.count = int(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* e = body.data() + kFixed + i * kEach;
    m.endpoints[i].local_port = uint16_t(detail::Get(e, 2));
    m.endpoints[i].ip = uint32_t(detail::Get(e + 2, 4));
    m.endpoints[i].port = uint16_t(detail::Get(e + 6, 2));
    m.endpoints[i].from_stun = (e[8] & 1) != 0;
  }
  return m.role == kHost || m.role == kJoiner;
}

// ---- MQTT 3.1.1 (QoS 0 only) ----------------------------------------------------------------------------------------

namespace mqtt {

enum Type : uint8_t {
  kConnect = 1, kConnack = 2, kPublish = 3, kSubscribe = 8, kSuback = 9, kPingreq = 12, kPingresp = 13, kDisconnect = 14
};

inline void PutString(std::vector<uint8_t>& out, std::string_view text) {
  detail::Put16(out, uint32_t(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}

// The fixed header (type and flags, the remaining length) before body.
inline std::vector<uint8_t> Packet(uint8_t first, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> out = {first};
  size_t length = body.size();
  do {
    uint8_t byte = uint8_t(length % 128);
    length /= 128;
    if (length) byte |= 0x80;
    out.push_back(byte);
  } while (length);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// CONNECT with a clean session. A non-empty will_topic adds a will: an empty retained message there, which the broker
// posts if the connection dies without a DISCONNECT (it clears a host's retained endpoints).
inline std::vector<uint8_t> Connect(std::string_view client_id, uint16_t keep_alive_s, std::string_view will_topic = {}) {
  std::vector<uint8_t> body;
  PutString(body, "MQTT");
  body.push_back(4);  // protocol level 3.1.1
  uint8_t flags = 0x02;  // clean session
  if (!will_topic.empty()) flags |= 0x04 | 0x20;  // will, will retain, will QoS 0
  body.push_back(flags);
  detail::Put16(body, keep_alive_s);
  PutString(body, client_id);
  if (!will_topic.empty()) {
    PutString(body, will_topic);
    PutString(body, "");
  }
  return Packet(kConnect << 4, body);
}

inline std::vector<uint8_t> Subscribe(uint16_t packet_id, std::string_view topic) {
  std::vector<uint8_t> body;
  detail::Put16(body, packet_id);
  PutString(body, topic);
  body.push_back(0);  // QoS 0
  return Packet(kSubscribe << 4 | 0x02, body);
}

inline std::vector<uint8_t> Publish(std::string_view topic, const uint8_t* payload, size_t size, bool retain) {
  std::vector<uint8_t> body;
  PutString(body, topic);
  body.insert(body.end(), payload, payload + size);
  return Packet(uint8_t(kPublish << 4 | (retain ? 1 : 0)), body);
}

inline std::vector<uint8_t> PingReq() { return {kPingreq << 4, 0}; }
inline std::vector<uint8_t> Disconnect() { return {kDisconnect << 4, 0}; }

struct Parsed {
  uint8_t type = 0, flags = 0;
  const uint8_t* body = nullptr;
  size_t body_size = 0;
};

// One whole packet at the start of data: the bytes it takes (> 0), 0 when more bytes are needed, -1 when malformed.
inline long Parse(const uint8_t* data, size_t size, Parsed& out, size_t max_packet = 64 * 1024) {
  if (size < 2) return 0;
  size_t length = 0, at = 1;
  for (int shift = 0;; shift += 7) {
    if (at >= size) return 0;
    if (shift > 21) return -1;
    const uint8_t byte = data[at++];
    length |= size_t(byte & 0x7F) << shift;
    if (!(byte & 0x80)) break;
  }
  if (length > max_packet) return -1;
  if (size - at < length) return 0;
  out.type = data[0] >> 4;
  out.flags = data[0] & 15;
  out.body = data + at;
  out.body_size = length;
  return long(at + length);
}

// A PUBLISH's topic and payload.
inline bool ReadPublish(const Parsed& p, std::string_view& topic, const uint8_t*& payload, size_t& payload_size) {
  if (p.type != kPublish || p.body_size < 2) return false;
  const size_t topic_size = size_t(p.body[0]) << 8 | p.body[1];
  size_t at = 2 + topic_size;
  if (at > p.body_size) return false;
  topic = std::string_view(reinterpret_cast<const char*>(p.body + 2), topic_size);
  if ((p.flags >> 1) & 3) at += 2;  // a packet identifier (QoS 1 or 2)
  if (at > p.body_size) return false;
  payload = p.body + at;
  payload_size = p.body_size - at;
  return true;
}

}  // namespace mqtt

}  // namespace rex::system::relay_wire
