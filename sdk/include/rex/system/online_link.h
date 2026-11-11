#pragma once
// Internet play on top of System Link. A title's System Link session runs unchanged over the internet once each
// console is reachable at the address its XNADDR carries. This module gets it there, trying three ways in turn:
//   - UPnP (IGD): finds the router by SSDP, reads its device description over HTTP, and asks its WAN*Connection
//     service to forward the System Link ports (UDP, the title's ports moved by net_system_port_offset, and the
//     port's own side channel above them) to this console. The leases are renewed while online and removed when
//     online play ends or the program exits.
//   - STUN (RFC 5389 Binding Request): the public IPv4 address the internet sees, checked against the router's own
//     idea of it (GetExternalIPAddress). A difference means another NAT between the router and the internet.
//   - Hole punching (online_relay.h), once the address is known: the consoles of one join code swap the public
//     endpoints of their ports through public MQTT brokers and send each other punch datagrams, which opens NATs that
//     ignore UPnP or cannot be configured at all (a phone's carrier NAT).
//   - When all that fails, the player forwards UDP 31000-31002 on the router by hand (the menu's help line says so).
// While online, system_link::TitleAddress is the public address: XNADDRs, the session's host address and every
// conversion between XNADDRs and addresses use it (xam_net.cpp, xsocket.cpp).
//
// Everything runs on one worker thread with bounded waits; the calls below only post work and read the result.

#include <cstdint>
#include <string>

#include <rex/system/online_relay.h>

namespace rex::system::online {

enum class Role { kNone, kHost, kJoin };
enum class State { kIdle, kPreparing, kReady, kFailed };
enum class Failure {
  kNone,
  kNoNetwork,        // no LAN address
  kNoPublicAddress,  // neither STUN nor the router told this console's public address
  kSameNetwork,      // the code is this console's own public address
};

struct Report {
  State state = State::kIdle;
  Role role = Role::kNone;
  Failure failure = Failure::kNone;
  uint32_t lan_ip = 0;            // all addresses in host byte order, 0 when unknown
  uint32_t public_ip = 0;         // the address other consoles reach this one at
  uint32_t target_ip = 0;         // joining: the host's public address, from the code
  uint32_t router_ip = 0;         // the UPnP router that answered
  uint32_t router_external_ip = 0;
  bool router_found = false;      // a UPnP router with a WAN connection service answered
  int ports_mapped = 0;           // of port_count
  int port_count = 0;
  uint16_t first_port = 0;        // the forwarded ports, first_port .. first_port + port_count - 1
  int upnp_error = 0;             // the router's last UPnP error code (718: the port is mapped to another device)
  uint32_t conflict_ip = 0;       // with 718: the device that has the port
  bool stun_ok = false;
  uint16_t stun_local_port = 0;   // the STUN socket's port, and the port the internet saw: a difference means the
  uint16_t stun_mapped_port = 0;  // NAT rewrites ports (fine with the router's forwarding in place)
  bool double_nat = false;        // the router's external address is private, or differs from STUN's
  relay::Status relay;            // hole punching, once ready (filled in by Current)
  uint64_t version = 0;           // changes with every new report
};

// Starts the preparation for hosting, or for joining the host at target_ip (host byte order), in the background.
// Online mode is off until it is ready.
void Start(Role role, uint32_t target_ip);
// Leaves online mode at once; the router's forwarding is removed in the background.
void Stop();
// At exit: removes the forwarding (briefly) and ends the worker thread.
void Shutdown();

Report Current();
uint64_t Version();  // Current().version, without the lock

// For the socket layer. The public address while online mode is on, 0 otherwise.
uint32_t PublicAddress();
// Joining by code: the host's public address while online mode is on, 0 otherwise.
uint32_t Target();

// The ports the router forwards: the moved title ports (from guest port 1000) and the side channel.
uint16_t FirstPort();
int PortCount();

}  // namespace rex::system::online
