#include <rex/system/join_code.h>
#include <rex/system/relay_wire.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace jc = rex::system::join_code;
namespace wire = rex::system::relay_wire;
namespace mqtt = rex::system::relay_wire::mqtt;

int main() {
  const std::string code = jc::Plain(0x5DB8D822u);
  const std::string other = jc::Plain(0x5DB8D823u);


  const std::string topic = wire::Topic(code);
  assert(topic == wire::Topic(code));
  assert(topic != wire::Topic(other));
  assert(topic.rfind("bo2zmnx/1/", 0) == 0 && topic.size() == 10 + 16);
  assert(topic.find(code) == std::string::npos);
  assert(wire::HostTopic(code) == topic + "/host" && wire::JoinTopic(code) == topic + "/join");


  std::vector<uint8_t> data(40), copy;
  for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 7);
  copy = data;
  wire::Keystream(code, 42, data.data(), data.size());
  assert(data != copy);
  std::vector<uint8_t> other_nonce = copy;
  wire::Keystream(code, 43, other_nonce.data(), other_nonce.size());
  assert(other_nonce != data);
  wire::Keystream(code, 42, data.data(), data.size());
  assert(data == copy);


  wire::Message m;
  m.role = wire::kHost;
  m.session = 0x0123456789ABCDEFull;
  m.public_ip = 0x5DB8D822u;
  m.count = 3;
  for (int i = 0; i < 3; ++i) {
    m.endpoints[size_t(i)] = {uint16_t(31000 + i), 0x5DB8D822u, uint16_t(i == 1 ? 65534 : 31000 + i), i != 2};
  }
  const auto bytes = wire::Encode(m, code, 99);
  wire::Message back;
  assert(wire::Decode(bytes.data(), bytes.size(), code, back));
  assert(back.role == m.role && back.session == m.session && back.public_ip == m.public_ip && back.count == 3);
  for (int i = 0; i < 3; ++i) {
    const auto& a = m.endpoints[size_t(i)];
    const auto& b = back.endpoints[size_t(i)];
    assert(a.local_port == b.local_port && a.ip == b.ip && a.port == b.port && a.from_stun == b.from_stun);
  }

  const uint8_t ip_bytes[4] = {0x5D, 0xB8, 0xD8, 0x22};
  for (size_t i = 0; i + 4 <= bytes.size(); ++i) assert(std::memcmp(bytes.data() + i, ip_bytes, 4) != 0);
  assert(!wire::Decode(bytes.data(), bytes.size(), other, back));
  std::mt19937 rng(7);
  int flips_caught = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    auto damaged = bytes;
    damaged[i] ^= uint8_t(1 + rng() % 255);
    flips_caught += !wire::Decode(damaged.data(), damaged.size(), code, back);
  }
  assert(flips_caught >= int(bytes.size()) - 8);
  assert(!wire::Decode(bytes.data(), bytes.size() - 1, code, back));


  std::vector<uint8_t> stream;
  const auto connect = mqtt::Connect("bzn0123456789abcdef", 30, wire::HostTopic(code));
  const auto subscribe = mqtt::Subscribe(1, wire::JoinTopic(code));
  std::vector<uint8_t> big(300, 0xAB);
  const auto publish = mqtt::Publish(wire::HostTopic(code), big.data(), big.size(), true);
  for (const auto* p : {&connect, &subscribe, &publish}) stream.insert(stream.end(), p->begin(), p->end());
  size_t at = 0;
  int packets = 0;
  mqtt::Parsed parsed;
  while (at < stream.size()) {

    const long used = mqtt::Parse(stream.data() + at, stream.size() - at, parsed);
    assert(used > 0);
    assert(mqtt::Parse(stream.data() + at, size_t(used) - 1, parsed) == 0);
    mqtt::Parse(stream.data() + at, stream.size() - at, parsed);
    if (packets == 0) {
      assert(parsed.type == mqtt::kConnect);
      assert(parsed.body[2] == 'M' && parsed.body[6] == 4 && parsed.body[7] == (0x02 | 0x04 | 0x20));
    } else if (packets == 1) {
      assert(parsed.type == mqtt::kSubscribe && parsed.flags == 2);
    } else {
      std::string_view t;
      const uint8_t* payload = nullptr;
      size_t size = 0;
      assert(mqtt::ReadPublish(parsed, t, payload, size));
      assert(t == wire::HostTopic(code) && size == big.size() && payload[0] == 0xAB && (parsed.flags & 1));
    }
    at += size_t(used);
    ++packets;
  }
  assert(packets == 3);
  const uint8_t bad[] = {0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
  assert(mqtt::Parse(bad, sizeof(bad), parsed) == -1);

  std::printf("relay wire: topic for %s is %s; %zu-byte endpoint message round-trips; %d of %zu damaged copies "
              "refused; MQTT packets parse back\n",
              jc::Make(0x5DB8D822u).c_str(), topic.c_str(), bytes.size(), flips_caught, bytes.size());
  return 0;
}
