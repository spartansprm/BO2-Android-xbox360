#pragma once
// Internet play's second way in: UDP hole punching, with public MQTT brokers as the go-between.
//
// Many home routers accept a UPnP forwarding and then never forward, and a phone's carrier NAT cannot be configured at
// all; a datagram from a console the other side has never sent to is dropped. Both NATs let a datagram in, though,
// from an address and port this side has just sent to. So each console learns where the internet sees each of its
// three UDP ports, the two consoles swap those endpoints, and each sends a few small "punch" datagrams to the other's
// endpoints: the punches open both NATs for the game's own traffic.
//
//   1. Endpoints. A STUN binding request goes out from each of the ports the System Link game uses: the title's two
//      sockets (the moved ports 31000 and 31001, bound by the game) and the side socket (31002). Each port has its own
//      NAT mapping, so each is asked from its own socket. The answers arrive on those sockets; the socket layer hands
//      them here (Classify) and never to the game.
//   2. Swap. The relay is any public MQTT 3.1.1 broker (net_online_brokers; all that answer are used at once). The
//      topics are made from the join code (relay_wire.h), so only the consoles of one code meet there. The host posts
//      its endpoints as a retained message (a joiner that comes later still gets them) and reads the joiners' topic;
//      a joiner reads the host's topic and posts its own endpoints every 2 s until it hears the host directly.
//   3. Punch. Whenever a console learns a peer's endpoints, it sends a punch datagram from each of its ports to the

//      the sender as a console on the internet (so replies go to the port it really uses). The joiner's searches
//      already knock on the host's door again and again; once the host's punches have opened its NAT, the next knock
//      gets through.
//
// What it cannot beat: a NAT that maps each destination to a different port (a "symmetric" NAT, which some carriers
// run) makes the endpoints STUN saw useless to the peer. The log says so when the two STUN servers see different ports.
//
// Everything runs on one thread of its own with short waits; a broker that is down or slow never holds up the game.

#include <array>
#include <cstdint>
#include <string>

namespace rex::system::online {

// What a datagram that reached one of the ports above is (the socket layer asks for every datagram it receives on
// them). kStun and kPunch are the relay's own and are never handed to the game.
enum class Datagram { kTitle, kStun, kPunch };
Datagram Classify(uint16_t local_port, uint32_t ip, uint16_t port, const uint8_t* data, int size);

namespace relay {

// One configured broker, in net_online_brokers order.
struct BrokerStatus {
  enum State { kChecking, kWorking, kDown };
  std::string name;     // host:port
  State state = kChecking;
  bool posted = false;  // this console's endpoints are on it
  int found = 0;        // consoles found through it
  bool operator==(const BrokerStatus&) const = default;
};

struct Status {
  bool active = false;
  std::array<BrokerStatus, 8> broker_list{};  // the first `brokers`
  int brokers = 0;          // configured
  int brokers_up = 0;       // connected now
  int brokers_failed = 0;   // gave no connection on their last attempt
  int ports = 0;            // local ports with a known public endpoint (from STUN)
  int port_count = 0;
  bool symmetric = false;   // two STUN servers saw different ports for the same local port
  int peers = 0;            // consoles found through the relay
  int peers_heard = 0;      // of them, heard directly on a title socket since
};

// Starts the relay for hosting (code_ip is this console's public address) or joining the host at code_ip. Replaces a
// running session.
void Start(bool host, uint32_t code_ip, uint32_t public_ip);
// Ends the session in the background: a host's retained message is cleared, the brokers are left.
void Stop();
// At exit.
void Shutdown();

Status Current();
uint64_t Version();  // changes with every change of Current()

}  // namespace relay
}  // namespace rex::system::online
