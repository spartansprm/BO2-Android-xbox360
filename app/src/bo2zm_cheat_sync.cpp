#include "bo2zm_cheat_sync.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/online_link.h>
#include <rex/system/xsocket.h>

#include "bo2zm_cheats.h"
#include "bo2zm_mode.h"
#include "bo2zm_preset.h"

REXCVAR_DEFINE_BOOL(bo2zm_cheat_sync, true, "BO2",
                    "System Link: send this console's player cheats and cheat commands to the host of a match it "
                    "joined, and run those of players who joined this console's match");
REXCVAR_DEFINE_BOOL(bo2zm_cheat_sync_trace, false, "BO2",
                    "System Link: log every cheat sync message sent and received (the rare events are always logged)");

namespace {

namespace link = rex::system::system_link;
namespace detail = bo2zm::cheats::detail;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x425A4353;
constexpr uint8_t kVersion = 2;
enum : uint8_t { kOptions = 1, kAnswer = 2, kCommand = 3, kResult = 4, kCatalog = 5, kWorld = 6, kBlasts = 7 };
constexpr size_t kHeader = 12;
constexpr size_t kRevisions = 8;
constexpr int kMaxPlayers = 4;
constexpr size_t kOptionsEntry = 8 + detail::kSyncedCount;
constexpr size_t kAnswerEntry = 9;
constexpr size_t kMaxDatagram = 1400;
constexpr size_t kCatalogPart = 1200;
constexpr int kMaxCatalogParts = 16;
constexpr int kMaxBlasts = 8;
constexpr size_t kMaxArgument = 64;
constexpr uint8_t kNoClient = 255;
constexpr uint16_t kGamePort = 1000;
constexpr uint16_t kSidePort = 1002;
constexpr auto kPeerFresh = std::chrono::milliseconds(5000);
constexpr std::chrono::milliseconds kResends[] = {std::chrono::milliseconds(1000), std::chrono::milliseconds(3000)};
constexpr int kLast = int(std::size(kResends));
constexpr int kHostPollEvery = 4;
constexpr int kJoinPollEvery = 2;


constexpr std::string_view kCommands[] = {
    "", "bo2zm_perk", "bo2zm_remove_perks", "bo2zm_give_weapon", "bo2zm_pap_weapon", "bo2zm_max_points",
    "bo2zm_powerup", "bo2zm_kill_zombies", "bo2zm_doors", "bo2zm_power", "bo2zm_debris", "bo2zm_goto_round",
    "bo2zm_zm_nospawn", "bo2zm_hold", "bo2zm_release"};

bool Trace() { return REXCVAR_GET(bo2zm_cheat_sync_trace); }

std::string Ip(uint32_t ip) {
  return fmt::format("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

void Put16(uint8_t* at, uint16_t v) {
  at[0] = uint8_t(v >> 8);
  at[1] = uint8_t(v);
}
void Put32(uint8_t* at, uint32_t v) {
  for (int i = 0; i < 4; ++i) at[i] = uint8_t(v >> (24 - 8 * i));
}
void Put64(uint8_t* at, uint64_t v) {
  for (int i = 0; i < 8; ++i) at[i] = uint8_t(v >> (56 - 8 * i));
}
uint16_t Get16(const uint8_t* at) { return uint16_t(at[0] << 8 | at[1]); }
uint32_t Get32(const uint8_t* at) {
  return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | at[3];
}
uint64_t Get64(const uint8_t* at) { return uint64_t(Get32(at)) << 32 | Get32(at + 4); }


struct Message {
  std::array<uint8_t, kMaxDatagram> bytes{};
  size_t size = kHeader;
  Message(uint8_t type, uint8_t count, uint8_t aux, uint32_t sequence) {
    Put32(bytes.data(), kMagic);
    bytes[4] = kVersion;
    bytes[5] = type;
    bytes[6] = count;
    bytes[7] = aux;
    Put32(bytes.data() + 8, sequence);
  }
  bool Room(size_t more) const { return size + more <= bytes.size(); }
  void U8(uint8_t v) { bytes[size++] = v; }
  void U16(uint16_t v) { Put16(bytes.data() + size, v); size += 2; }
  void U32(uint32_t v) { Put32(bytes.data() + size, v); size += 4; }
  void U64(uint64_t v) { Put64(bytes.data() + size, v); size += 8; }
  void Text(std::string_view text) { std::memcpy(bytes.data() + size, text.data(), text.size()); size += text.size(); }
};


bool PlainText(std::string_view text, size_t most) {
  return text.size() <= most &&
         text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_. ,()'-") ==
             std::string_view::npos;
}


uint64_t ControllerXuid(int controller) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* profile = kernel_state && controller >= 0 && controller < detail::kLocalPlayers
                      ? kernel_state->user_profile(uint32_t(controller))
                      : nullptr;
  return profile ? profile->xuid() : 0;
}

bool OwnXuid(uint64_t xuid) {
  for (int controller = 0; controller < detail::kLocalPlayers; ++controller) {
    if (ControllerXuid(controller) == xuid) return true;
  }
  return false;
}

std::string OptionsText(const int* values) {
  std::string text;
  for (int i = 0; i < detail::kSyncedCount; ++i) {
    const auto option = detail::kSyncedOptions[i];
    if (values[i] == 0) continue;
    if (!text.empty()) text += ' ';
    text += fmt::format("{}={}", detail::OptionName(option), values[i]);
  }
  return text.empty() ? "all off" : text;
}


int64_t Socket() {
  static int64_t socket = -1;
  static bool tried = false;
  if (!tried) {
    tried = true;
    bool bound = false;
    const uint16_t port = link::HostPort(kSidePort);
    socket = link::OpenSideSocket(port, &bound);
    REXLOG_INFO("[cheat sync] side socket on port {}: {}", port,
                socket < 0 ? "not available" : bound ? "open" : "open, sending only (the port is taken)");
  }
  return socket;
}

int Send(uint32_t ip, uint16_t port, const Message& message) {
  const int64_t socket = Socket();
  return socket < 0 ? -1 : link::SideSend(socket, ip, port, message.bytes.data(), uint32_t(message.size));
}


struct Received {
  uint8_t type = 0, count = 0, aux = 0;
  uint32_t sequence = 0;
  const uint8_t* body = nullptr;
  size_t body_size = 0;
  uint32_t ip = 0;
  uint16_t port = 0;
};

bool Receive(std::array<uint8_t, 2048>& buffer, Received& out) {
  const int64_t socket = Socket();
  if (socket < 0) return false;
  for (;;) {
    const int got = link::SideReceive(socket, buffer.data(), uint32_t(buffer.size()), &out.ip, &out.port);
    if (got < 0) return false;
    const size_t size = size_t(got);
    if (size < kHeader || Get32(buffer.data()) != kMagic) {
      if (Trace()) REXLOG_INFO("[cheat sync] ignored a {}-byte datagram from {}:{} (not ours)", size, Ip(out.ip), out.port);
      continue;
    }
    if (buffer[4] != kVersion) {
      static int logged = 0;
      if (logged++ < 3) {
        REXLOG_WARN("[cheat sync] {} sends version {} messages (this build reads {}); ignored", Ip(out.ip), buffer[4],
                    kVersion);
      }
      continue;
    }
    out.type = buffer[5];
    out.count = buffer[6];
    out.aux = buffer[7];
    out.sequence = Get32(buffer.data() + 8);
    out.body = buffer.data() + kHeader;
    out.body_size = size - kHeader;
    return true;
  }
}


struct Command {
  uint32_t sequence = 0;
  uint64_t xuid = 0;
  uint8_t id = 0;
  bool pap = false;
  std::string argument;
  Clock::time_point first_at{}, sent_at{};
  int resends = 0;
};

struct Joiner {
  bool active = false;
  uint32_t sequence = 0;
  int count = 0;
  std::array<uint64_t, kMaxPlayers> xuids{};
  std::array<std::array<int, detail::kSyncedCount>, kMaxPlayers> values{};
  std::array<int, kMaxPlayers> clients{};
  bool answered = false;
  Clock::time_point first_at{};
  Clock::time_point sent_at{};
  int resends = 0;
  bool sent_broadcast = false;
  uint32_t host_ip = 0;
  uint32_t catalog_revision = 0, world_revision = 0;
  uint32_t catalog_building = 0;
  std::vector<std::string> catalog_parts;
  std::vector<bool> catalog_have;
  uint32_t command_sequence = 0;
  std::vector<Command> commands;
};
Joiner g_join;
std::mutex g_forward_mutex;
std::vector<Command> g_forwarded;


uint32_t HostAddress() {
  if (g_join.host_ip) return g_join.host_ip;
  uint32_t ip = 0;
  int64_t age = 0;
  return link::LastPeer(kGamePort, &ip, &age) && age < kPeerFresh.count() ? ip : 0;
}


uint32_t Destination(bool broadcast, bool& sent_broadcast) {
  uint32_t to = broadcast ? 0 : HostAddress();
  if (!to) to = rex::system::online::Target();
  sent_broadcast = to == 0;
  if (!to) {
    uint32_t ip = 0, mask = 0;
    if (!link::LanAddress(&ip, &mask) || !mask) {
      REXLOG_WARN("[cheat sync] no LAN address: nothing is sent");
      return 0;
    }
    to = ip | ~mask;
  }
  return to;
}

void SendOptions(const char* why, bool broadcast = false) {
  if (g_join.count == 0) return;
  Message message(kOptions, uint8_t(g_join.count), uint8_t(detail::kSyncedCount), g_join.sequence);
  message.U32(g_join.catalog_revision);
  message.U32(g_join.world_revision);
  for (int p = 0; p < g_join.count; ++p) {
    message.U64(g_join.xuids[p]);
    for (int i = 0; i < detail::kSyncedCount; ++i) message.U8(uint8_t(g_join.values[p][i]));
  }
  const uint32_t to = Destination(broadcast, g_join.sent_broadcast);
  if (!to) return;
  const uint16_t port = link::HostPort(kSidePort);
  const int sent = Send(to, port, message);
  g_join.sent_at = Clock::now();
  for (int p = 0; p < g_join.count; ++p) {
    REXLOG_INFO("[cheat sync] sent options ({}, message {}) to {}:{}{}: player {:016X} {}{}", why, g_join.sequence,
                Ip(to), port, g_join.sent_broadcast ? " (the LAN's broadcast)" : "", g_join.xuids[p],
                OptionsText(g_join.values[p].data()), sent < 0 ? " -- the send failed" : "");
  }
}


void SendNew(const char* why) {
  ++g_join.sequence;
  g_join.answered = false;
  g_join.resends = 0;
  g_join.clients.fill(-1);
  g_join.first_at = Clock::now();
  SendOptions(why);
}

void SendCommand(Command& command, const char* why, bool broadcast = false) {
  Message message(kCommand, 1, command.id, command.sequence);
  message.U64(command.xuid);
  message.U8(command.pap ? 1 : 0);
  message.U8(uint8_t(command.argument.size()));
  message.Text(command.argument);
  bool sent_broadcast = false;
  const uint32_t to = Destination(broadcast, sent_broadcast);
  if (!to) return;
  const uint16_t port = link::HostPort(kSidePort);
  const int sent = Send(to, port, message);
  command.sent_at = Clock::now();
  REXLOG_INFO("[cheat sync] sent command {} ({}, number {}) to {}:{}{}: player {:016X} {} {}{}", command.id, why,
              command.sequence, Ip(to), port, sent_broadcast ? " (the LAN's broadcast)" : "", command.xuid,
              kCommands[command.id], command.argument, sent < 0 ? " -- the send failed" : "");
}

void JoinerReceived(PPCContext& ctx, uint8_t* base, const Received& in) {
  if (in.type == kAnswer) {
    const int count = in.count;
    if (count < 1 || count > kMaxPlayers || in.body_size != kRevisions + count * kAnswerEntry) {
      REXLOG_WARN("[cheat sync] ignored a malformed answer from {}", Ip(in.ip));
      return;
    }
    if (in.sequence != g_join.sequence) {
      if (Trace()) REXLOG_INFO("[cheat sync] answer from {} to an older message ({}); ignored", Ip(in.ip), in.sequence);
      return;
    }
    const uint32_t catalog = Get32(in.body), world = Get32(in.body + 4);
    for (int a = 0; a < count; ++a) {
      const uint8_t* entry = in.body + kRevisions + a * kAnswerEntry;
      const uint64_t xuid = Get64(entry);
      for (int p = 0; p < g_join.count; ++p) {
        if (g_join.xuids[p] == xuid) g_join.clients[p] = entry[8] == kNoClient ? -1 : entry[8];
      }
    }
    bool all = true;
    for (int p = 0; p < g_join.count; ++p) all &= g_join.clients[p] >= 0;
    const bool current = catalog == g_join.catalog_revision && world == g_join.world_revision;
    if (all && !g_join.answered) {
      for (int p = 0; p < g_join.count; ++p) {
        REXLOG_INFO("[cheat sync] the host applies options message {} to player {:016X} (its server client {})",
                    g_join.sequence, g_join.xuids[p], g_join.clients[p]);
      }
    }
    if (Trace() && (!all || !current)) {
      REXLOG_INFO("[cheat sync] the host at {}: {} catalog {} (here {}), world {} (here {})", Ip(in.ip),
                  all ? "has every player," : "does not have every player yet,", catalog, g_join.catalog_revision,
                  world, g_join.world_revision);
    }
    g_join.answered = all && current;
    return;
  }
  if (in.type == kResult) {
    if (in.body_size < 9 || in.body_size != 9 + size_t(in.body[8]) || in.aux > 1) {
      REXLOG_WARN("[cheat sync] ignored a malformed result from {}", Ip(in.ip));
      return;
    }
    const std::string_view text(reinterpret_cast<const char*>(in.body + 9), in.body[8]);
    const auto it = std::find_if(g_join.commands.begin(), g_join.commands.end(),
                                 [&](const Command& command) { return command.sequence == in.sequence; });
    if (it == g_join.commands.end()) return;
    if (!PlainText(text, 255)) return;
    REXLOG_INFO("[cheat sync] the host {} {} {} for player {:016X}{}{}", in.aux == 0 ? "ran" : "refused",
                kCommands[it->id], it->argument, it->xuid, text.empty() ? "" : ": ", text);
    g_join.commands.erase(it);
    return;
  }
  if (in.type == kCatalog) {
    const int part = in.count, parts = in.aux;
    if (parts < 1 || parts > kMaxCatalogParts || part >= parts || in.body_size > kCatalogPart) {
      REXLOG_WARN("[cheat sync] ignored a malformed catalog part from {}", Ip(in.ip));
      return;
    }
    if (in.sequence == g_join.catalog_revision) return;
    if (in.sequence != g_join.catalog_building || int(g_join.catalog_parts.size()) != parts) {
      g_join.catalog_building = in.sequence;
      g_join.catalog_parts.assign(parts, std::string());
      g_join.catalog_have.assign(parts, false);
    }
    g_join.catalog_parts[part].assign(reinterpret_cast<const char*>(in.body), in.body_size);
    g_join.catalog_have[part] = true;
    if (std::all_of(g_join.catalog_have.begin(), g_join.catalog_have.end(), [](bool have) { return have; })) {
      std::string text;
      for (const auto& piece : g_join.catalog_parts) text += piece;
      bo2zm::cheats::ApplyCatalog(ctx, base, text);
      g_join.catalog_revision = in.sequence;
      g_join.catalog_parts.clear();
      g_join.catalog_have.clear();
      REXLOG_INFO("[cheat sync] the host's catalog {} received ({} bytes)", in.sequence, text.size());
    }
    return;
  }
  if (in.type == kWorld) {
    if (in.body_size < 2 || in.body_size != 2 + size_t(Get16(in.body)) || in.body_size > 512) {
      REXLOG_WARN("[cheat sync] ignored malformed world settings from {}", Ip(in.ip));
      return;
    }
    if (in.sequence == g_join.world_revision) return;
    const std::string_view text(reinterpret_cast<const char*>(in.body + 2), Get16(in.body));
    if (text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.=;") !=
        std::string_view::npos) {
      REXLOG_WARN("[cheat sync] the host's world settings have unexpected characters; ignored");
      return;
    }
    bo2zm::cheats::ApplyWorldState(text);
    g_join.world_revision = in.sequence;
    REXLOG_INFO("[cheat sync] the host's world settings {}: {}", in.sequence, text.empty() ? "the game's own" : text);
    return;
  }
  if (in.type == kBlasts) {
    const int count = in.count;
    if (count < 1 || count > kMaxBlasts || in.body_size != 8 + size_t(count) * 12) return;
    const uint64_t xuid = Get64(in.body);
    bool ours = false;
    for (int p = 0; p < g_join.count; ++p) ours |= g_join.xuids[p] == xuid;
    if (!ours) return;
    for (int h = 0; h < count; ++h) {
      float at[3];
      for (int i = 0; i < 3; ++i) {
        const uint32_t bits = Get32(in.body + 8 + h * 12 + i * 4);
        std::memcpy(&at[i], &bits, 4);
      }
      if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2])) return;
      bo2zm::cheats::PlayRemoteBlast(at);
    }
    if (Trace()) REXLOG_INFO("[cheat sync] {} explosive hit(s) from the host to draw", count);
    return;
  }
  if (Trace()) REXLOG_INFO("[cheat sync] ignored a type {} message from {}", in.type, Ip(in.ip));
}

void JoinerPoll(PPCContext& ctx, uint8_t* base) {
  std::array<uint8_t, 2048> buffer{};
  Received in;
  while (Receive(buffer, in)) {
    if (in.type == kOptions || in.type == kCommand) continue;
    if (g_join.host_ip != in.ip) {
      REXLOG_INFO("[cheat sync] the host is {}", Ip(in.ip));
      g_join.host_ip = in.ip;
    }
    JoinerReceived(ctx, base, in);
  }
}

void Join(PPCContext& ctx, uint8_t* base) {
  static int frames = 0;
  if (++frames >= kJoinPollEvery) {
    frames = 0;
    JoinerPoll(ctx, base);
  }
  bo2zm::cheats::SyncedPlayer players[kMaxPlayers];
  const int count = bo2zm::cheats::LocalSyncedPlayers(players, kMaxPlayers);
  Joiner now = g_join;
  now.count = 0;
  for (int p = 0; p < count; ++p) {
    const uint64_t xuid = ControllerXuid(players[p].controller);
    if (!xuid) continue;
    now.xuids[now.count] = xuid;
    for (int i = 0; i < detail::kSyncedCount; ++i) now.values[now.count][i] = players[p].values[i];
    ++now.count;
  }
  bool same = now.count == g_join.count;
  for (int p = 0; same && p < now.count; ++p) {
    same = now.xuids[p] == g_join.xuids[p] && now.values[p] == g_join.values[p];
  }
  if (!g_join.active) {

    const uint32_t start = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        Clock::now().time_since_epoch()).count());
    g_join = {};
    g_join.active = true;
    g_join.count = now.count;
    g_join.xuids = now.xuids;
    g_join.values = now.values;
    g_join.sequence = start;
    g_join.command_sequence = start;
    REXLOG_INFO("[cheat sync] in a match this console joined: sending its {} player(s)' cheats to the host",
                g_join.count);
    SendNew("match start");
  } else if (!same) {
    g_join.count = now.count;
    g_join.xuids = now.xuids;
    g_join.values = now.values;
    SendNew("options changed");
  } else if (!g_join.answered) {
    const auto now_time = Clock::now();
    if (g_join.resends < kLast) {
      if (g_join.sent_broadcast && g_join.resends == 0 && HostAddress()) {
        SendOptions("the host is known now");
      } else if (now_time - g_join.first_at >= kResends[g_join.resends]) {
        ++g_join.resends;

        SendOptions("resend", g_join.resends == kLast);
      }
    } else if (g_join.resends == kLast && now_time - g_join.sent_at >= std::chrono::seconds(1)) {
      ++g_join.resends;
      REXLOG_WARN("[cheat sync] the host did not answer options message {} for every player; it is sent again on the "
                  "next change", g_join.sequence);
    }
  }


  std::vector<Command> forwarded;
  {
    std::lock_guard lock(g_forward_mutex);
    forwarded.swap(g_forwarded);
  }
  for (auto& command : forwarded) {
    if (g_join.commands.size() >= 8) {
      REXLOG_WARN("[cheat sync] too many commands waiting for the host; {} dropped", kCommands[command.id]);
      continue;
    }
    command.sequence = ++g_join.command_sequence;
    command.first_at = Clock::now();
    SendCommand(command, "picked");
    g_join.commands.push_back(std::move(command));
  }
  const auto now_time = Clock::now();
  for (auto it = g_join.commands.begin(); it != g_join.commands.end();) {
    if (it->resends < kLast) {
      if (now_time - it->first_at >= kResends[it->resends]) {
        ++it->resends;
        SendCommand(*it, "resend", it->resends == kLast);
      }
      ++it;
    } else if (now_time - it->sent_at >= std::chrono::seconds(1)) {
      REXLOG_WARN("[cheat sync] the host did not answer {} {}; it was not run", kCommands[it->id], it->argument);
      it = g_join.commands.erase(it);
    } else {
      ++it;
    }
  }
}


struct Peer {
  uint32_t ip = 0;
  uint16_t port = 0;
  std::array<uint64_t, kMaxPlayers> xuids{};
  int count = 0;
  uint32_t catalog_sent = 0, world_sent = 0;
};
std::array<Peer, 8> g_peers;
struct Done {
  uint32_t ip = 0;
  uint64_t xuid = 0;
  uint32_t sequence = 0;
  uint8_t status = 0;
  std::string why;
};
std::array<Done, 16> g_done;
size_t g_done_next = 0;
uint32_t g_world_revision = 1;
std::string g_world_text;
Clock::time_point g_world_again{};
uint32_t g_catalog_pushed = 0;
Clock::time_point g_catalog_again{};

Peer* PeerAt(uint32_t ip, uint16_t port, bool add) {
  for (auto& peer : g_peers) {
    if (peer.ip == ip && peer.port == port) return &peer;
  }
  if (!add) return nullptr;
  for (auto& peer : g_peers) {
    if (!peer.ip) {
      peer = {};
      peer.ip = ip;
      peer.port = port;
      return &peer;
    }
  }
  g_peers[0] = {};
  g_peers[0].ip = ip;
  g_peers[0].port = port;
  return &g_peers[0];
}

void SendCatalog(Peer& peer, const std::string& text, uint32_t revision) {
  const int parts = int((text.size() + kCatalogPart - 1) / kCatalogPart);
  if (parts < 1 || parts > kMaxCatalogParts) {
    REXLOG_WARN("[cheat sync] the catalog ({} bytes) is too big to send", text.size());
    return;
  }
  for (int part = 0; part < parts; ++part) {
    Message message(kCatalog, uint8_t(part), uint8_t(parts), revision);
    message.Text(std::string_view(text).substr(part * kCatalogPart, kCatalogPart));
    Send(peer.ip, peer.port, message);
  }
  peer.catalog_sent = revision;
  REXLOG_INFO("[cheat sync] sent the catalog {} ({} bytes in {} part(s)) to {}", revision, text.size(), parts,
              Ip(peer.ip));
}

void SendWorld(Peer& peer) {
  Message message(kWorld, 0, 0, g_world_revision);
  message.U16(uint16_t(g_world_text.size()));
  message.Text(g_world_text);
  Send(peer.ip, peer.port, message);
  peer.world_sent = g_world_revision;
  REXLOG_INFO("[cheat sync] sent the world settings {} to {}: {}", g_world_revision, Ip(peer.ip),
              g_world_text.empty() ? "the game's own" : g_world_text);
}

void HostOptions(const uint8_t* base, const Received& in) {
  const int count = in.count;
  if (count < 1 || count > kMaxPlayers || in.aux != detail::kSyncedCount ||
      in.body_size != kRevisions + count * kOptionsEntry) {
    REXLOG_WARN("[cheat sync] ignored a malformed options message from {} ({} players, {} options)", Ip(in.ip), count,
                in.aux);
    return;
  }
  Message answer(kAnswer, 0, 0, in.sequence);
  std::string catalog;
  uint32_t catalog_revision = 0;
  bo2zm::cheats::CatalogText(catalog, catalog_revision);
  answer.U32(catalog_revision);
  answer.U32(g_world_revision);
  Peer* peer = PeerAt(in.ip, in.port, true);
  int answered = 0;
  for (int p = 0; p < count; ++p) {
    const uint8_t* entry = in.body + kRevisions + p * kOptionsEntry;
    const uint64_t xuid = Get64(entry);
    int values[detail::kSyncedCount];
    bool valid = xuid != 0;
    for (int i = 0; i < detail::kSyncedCount; ++i) {
      values[i] = entry[8 + i];
      valid &= values[i] <= detail::OptionMaximum(detail::kSyncedOptions[i]);
    }
    if (!valid) {
      REXLOG_WARN("[cheat sync] ignored player {:016X} from {}: a value out of range", xuid, Ip(in.ip));
      continue;
    }
    if (OwnXuid(xuid)) {
      if (Trace()) REXLOG_INFO("[cheat sync] {:016X} from {} is one of this console's players; ignored", xuid, Ip(in.ip));
      continue;
    }
    int client = -1;
    bool changed = false;
    if (!bo2zm::cheats::SetRemotePlayer(base, xuid, in.sequence, values, &client, &changed)) {
      if (Trace()) REXLOG_INFO("[cheat sync] options {} for {:016X} from {} not taken", in.sequence, xuid, Ip(in.ip));
      continue;
    }
    if (changed || Trace()) {
      REXLOG_INFO("[cheat sync] received options {} from {}: player {:016X} {} -> {}", in.sequence, Ip(in.ip), xuid,
                  OptionsText(values),
                  client >= 0 ? fmt::format("applied to server client {}", client)
                              : std::string("kept until that player is among the server's"));
    }
    answer.U64(xuid);
    answer.U8(client >= 0 ? uint8_t(client) : kNoClient);
    if (answered < kMaxPlayers) peer->xuids[answered] = xuid;
    ++answered;
  }
  if (!answered) return;
  peer->count = answered;
  answer.bytes[6] = uint8_t(answered);
  const int sent = Send(in.ip, in.port, answer);
  if (Trace() || sent < 0) {
    REXLOG_INFO("[cheat sync] answered options {} to {}:{} for {} player(s){}", in.sequence, Ip(in.ip), in.port,
                answered, sent < 0 ? " -- the send failed" : "");
  }

  if (!catalog.empty() && Get32(in.body) != catalog_revision) SendCatalog(*peer, catalog, catalog_revision);
  if (Get32(in.body + 4) != g_world_revision) SendWorld(*peer);
}

void HostCommand(const uint8_t* base, const Received& in) {
  if (in.count != 1 || in.aux < 1 || in.aux >= std::size(kCommands) || in.body_size < 10 ||
      in.body_size != 10 + size_t(in.body[9]) || in.body[9] > kMaxArgument || in.body[8] > 1) {
    REXLOG_WARN("[cheat sync] ignored a malformed command from {}", Ip(in.ip));
    return;
  }
  const uint64_t xuid = Get64(in.body);
  const bool pap = in.body[8] == 1;
  const std::string_view argument(reinterpret_cast<const char*>(in.body + 10), in.body[9]);
  const std::string_view name = kCommands[in.aux];
  uint8_t status = 1;
  std::string why;
  const auto previous = std::find_if(g_done.begin(), g_done.end(), [&](const Done& done) {
    return done.ip == in.ip && done.xuid == xuid && done.sequence == in.sequence;
  });
  if (previous != g_done.end()) {
    status = previous->status;
    why = previous->why;
  } else {
    if (!PlainText(argument, kMaxArgument)) {
      why = "the argument has unexpected characters";
    } else if (OwnXuid(xuid)) {
      why = "that is one of the host's own players";
    } else {
      status = uint8_t(bo2zm::cheats::RunRemoteCommand(base, xuid, name, argument, pap, why) == 0 ? 0 : 1);
    }
    g_done[g_done_next++ % g_done.size()] = {in.ip, xuid, in.sequence, status, why};
    REXLOG_INFO("[cheat sync] received command {} from {}: player {:016X} {} {} -> {}{}{}", in.sequence, Ip(in.ip),
                xuid, name, argument, status == 0 ? "run" : "refused", why.empty() ? "" : ": ", why);
  }
  PeerAt(in.ip, in.port, true);
  Message result(kResult, 0, status, in.sequence);
  result.U64(xuid);
  const std::string text = why.substr(0, 200);
  result.U8(uint8_t(text.size()));
  result.Text(text);
  Send(in.ip, in.port, result);
}

void HostPush() {

  const std::string world = bo2zm::cheats::WorldStateText();
  const auto now = Clock::now();
  bool push_world = false;
  if (world != g_world_text) {
    g_world_text = world;
    ++g_world_revision;
    push_world = true;
    g_world_again = now + std::chrono::seconds(1);
  } else if (g_world_again != Clock::time_point{} && now >= g_world_again) {
    g_world_again = {};
    for (auto& peer : g_peers) peer.world_sent = 0;
    push_world = true;
  }
  std::string catalog;
  uint32_t catalog_revision = 0;
  const bool has_catalog = bo2zm::cheats::CatalogText(catalog, catalog_revision);
  bool push_catalog = false;
  if (has_catalog && catalog_revision != g_catalog_pushed) {
    g_catalog_pushed = catalog_revision;
    push_catalog = true;
    g_catalog_again = now + std::chrono::seconds(1);
  } else if (has_catalog && g_catalog_again != Clock::time_point{} && now >= g_catalog_again) {
    g_catalog_again = {};
    for (auto& peer : g_peers) peer.catalog_sent = 0;
    push_catalog = true;
  }
  for (auto& peer : g_peers) {
    if (!peer.ip) continue;
    if (push_world && peer.world_sent != g_world_revision) SendWorld(peer);
    if (push_catalog && peer.catalog_sent != catalog_revision) SendCatalog(peer, catalog, catalog_revision);
  }


  bo2zm::cheats::RemoteBlastHit hits[16];
  const int count = bo2zm::cheats::TakeRemoteBlasts(hits, 16);
  for (int h = 0; h < count;) {
    const uint64_t xuid = hits[h].xuid;
    Peer* to = nullptr;
    for (auto& peer : g_peers) {
      for (int p = 0; peer.ip && p < peer.count; ++p) {
        if (peer.xuids[p] == xuid) to = &peer;
      }
    }
    int n = 0;
    Message message(kBlasts, 0, 0, 0);
    message.U64(xuid);
    for (; h < count && hits[h].xuid == xuid && n < kMaxBlasts; ++h, ++n) {
      for (const float v : hits[h].at) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        message.U32(bits);
      }
    }
    message.bytes[6] = uint8_t(n);
    if (to) Send(to->ip, to->port, message);
  }
}

void Host(const uint8_t* base) {
  static int frames = 0;
  static bool polling = false;
  uint32_t peer = 0;
  int64_t age = 0;
  const bool talking = link::LastPeer(kGamePort, &peer, &age) && age < kPeerFresh.count();
  if (!talking) {
    if (polling) {
      polling = false;
      g_peers = {};
    }
    return;
  }
  if (++frames < kHostPollEvery) return;
  frames = 0;
  if (Socket() < 0) return;
  std::array<uint8_t, 2048> buffer{};
  Received in;
  if (!polling) {

    polling = true;
    int dropped = 0;
    while (Receive(buffer, in)) ++dropped;
    REXLOG_INFO("[cheat sync] hosting a match with another console ({}): listening for its players' cheats{}", Ip(peer),
                dropped ? fmt::format(" ({} old message(s) dropped)", dropped) : std::string());
    return;
  }
  while (Receive(buffer, in)) {
    if (in.type == kOptions) HostOptions(base, in);
    else if (in.type == kCommand) HostCommand(base, in);

  }
  HostPush();
}

}

bool bo2zm::cheat_sync::Forward(int controller, std::string_view name, std::string_view argument, bool pack_a_punch) {
  const auto it = std::find(std::begin(kCommands) + 1, std::end(kCommands), name);
  if (it == std::end(kCommands) || !PlainText(argument, kMaxArgument)) {
    REXLOG_WARN("[cheat sync] {} {}: not a command the host takes", name, argument);
    return false;
  }
  const uint64_t xuid = ControllerXuid(controller);
  if (!xuid) return false;
  Command command;
  command.xuid = xuid;
  command.id = uint8_t(it - std::begin(kCommands));
  command.pap = pack_a_punch;
  command.argument = std::string(argument);
  std::lock_guard lock(g_forward_mutex);
  if (g_forwarded.size() >= 8) return false;
  g_forwarded.push_back(std::move(command));
  return true;
}

void bo2zm::cheat_sync::TraceFrame(bool map_loading, bool local_actor) {
  if (!Trace()) return;
  static Clock::time_point next{};
  const auto now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(10);
  uint32_t peer = 0;
  int64_t age = -1;
  const bool heard = link::LastPeer(kGamePort, &peer, &age);
  REXLOG_INFO("[cheat sync] frame: map loading {}, a local player on this server {}, the client in a match {}, "
              "game port last heard {} ({} ms ago)", map_loading, local_actor, bo2zm::preset::ClientInGame(),
              heard ? Ip(peer) : std::string("never"), age);
}

void bo2zm::cheat_sync::EveryFrame(PPCContext& ctx, uint8_t* base) {
  if (!base || !REXCVAR_GET(bo2zm_cheat_sync) || !bo2zm::mode::IsZombies()) return;
  static int state = 0;
  const bool hosting = bo2zm::cheats::LocalServer(base);
  const bool joined = !hosting && bo2zm::cheats::JoinedRemoteGame();
  const int now = hosting ? 1 : joined ? 2 : 0;
  if (now != state) {
    state = now;
    if (Trace() || now == 2) {
      REXLOG_INFO("[cheat sync] this console {}", now == 1 ? "runs the server" : now == 2 ? "joined another's match"
                                                                                  : "is in no match");
    }
  }
  if (hosting) Host(base);
  if (joined) {
    Join(ctx, base);
  } else if (g_join.active) {
    REXLOG_INFO("[cheat sync] out of the joined match: the host's world settings go back to the game's");
    bo2zm::cheats::ApplyWorldState({});
    g_join = {};
    std::lock_guard lock(g_forward_mutex);
    g_forwarded.clear();
  }
}
