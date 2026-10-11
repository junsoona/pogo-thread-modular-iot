// UDP CoAP server loop -- the ONLY file in this program that touches
// sockets. Everything it does with a received datagram (parse, validate,
// apply, decide a response code, ACK) is delegated to
// ingest::coap_parse()/ingest::handle_request()/ingest::coap_build_ack(),
// so this stays swappable for a real libcoap3 server later without
// touching registry.hpp/session.hpp at all (see minicoap.hpp's own top
// comment for why this program uses a minimal internal parser instead of
// libcoap3 today).
//
// SECURITY NOTE (this follow-up's correction pass, item 2): CoAP's CRC in
// bridge_export_proto and the base CoAP framing here are integrity checks
// against ACCIDENTAL corruption, NOT authentication -- neither proves who
// sent a datagram, and neither is any protection against a spoofed source
// address or a malicious sender. This program is designed to be reachable
// ONLY from inside the Thread network's own security boundary (a Thread
// mesh's own link-layer/network-layer encryption and the Linux OTBR's own
// border-router role are what actually restrict who can originate traffic
// that ever reaches this process at all) -- restricting which LOCAL
// network interface this program listens on (via ServerConfig::interface_name,
// e.g. "wpan0") is a defense-in-depth measure against a process on the SAME
// Linux host (or reachable via some other local interface) impersonating a
// Power Base, not a substitute for that Thread-side boundary. A future
// revision may add a Power Base UUID allowlist (see ServerConfig's own doc
// comment) for a further layer once an operator has a specific set of
// Power Base UUIDs to pin -- deliberately NOT auto-implemented as
// trust-on-first-use in this pass.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "ingest/registry.hpp"
#include "ingest/session.hpp"

namespace ingest {

struct ServerConfig {
    uint16_t port = 5684;
    // The Power Base management resource (see session.hpp). Empty: the
    // resource answers 4.04 and this server behaves exactly as before.
    // Called on the receive thread, one request at a time.
    ManagementHandler management;
    // "Where is the hub now?" (docs/power-base-management.md, section 12).
    // A Power Base that lost its hub asks on `locate_port` of whatever
    // address it has a hint for; it knows that port without being told
    // (PB_MGMT_LOCATE_PORT, the default ingest port). When this server's own
    // `port` IS that port, nothing more is opened: the ingest socket answers
    // LOCATE like every other management message, through `management`.
    // When it is another port, a second socket is bound on `locate_port` of
    // the same interface, and it answers exactly one thing: a Confirmable
    // POST to the management resource, handed to `locate`, whose reply is
    // sent unless it is `silent`. Everything else that arrives there is
    // dropped without an answer and never reaches the Registry.
    // `locate` empty or `locate_port` 0: no second socket. A second socket
    // that cannot be bound is logged and the server runs without it.
    ManagementHandler locate;
    uint16_t locate_port = 0;
    // REQUIRED (must be non-empty and must resolve via if_nametoindex()) --
    // the ONLY local network interface this server accepts datagrams from;
    // every other interface's traffic is dropped before it ever reaches
    // ingest::handle_request() (see server.cpp's IPV6_RECVPKTINFO check).
    // There is deliberately no "any interface" default: a caller must name
    // the Thread interface (or, for tests, an explicit loopback name)
    // rather than accidentally falling back to accepting from everywhere.
    std::string interface_name;
    // Reserved for a future Power Base UUID allowlist (this follow-up's own
    // explicit "설정 구조를 분리하되 ... TOFU를 자동 구현하지 않는다" --
    // deliberately left unimplemented/unused here; the field does not even
    // exist yet, this comment is the placeholder for where it will go so a
    // later change is additive, not a redesign).
};

// Resolves config.interface_name, binds a UDP/IPv6 socket on config.port,
// and serves forever, applying every request FROM THAT INTERFACE ONLY to
// `registry` -- a datagram received on any other interface is dropped
// before ingest::handle_request() ever sees it (see this header's own
// SECURITY NOTE). ONLY a Confirmable POST can ever reach
// ingest::handle_request() at all (correction pass) -- a Non-confirmable
// (or Acknowledgement/Reset) datagram, or a Confirmable non-POST method, is
// either dropped outright (Non-confirmable/Ack/Reset: no state change, no
// response) or answered with a 4.xx error (Confirmable non-POST: 4.05,
// Confirmable with an unrecognized critical option: 4.02) without ever
// calling handle_request() -- see server.cpp's own recv loop for the exact
// order these checks run in. If `should_stop` is non-null, the loop checks
// it (with a short, bounded poll interval) and returns once it becomes
// true -- used by the loopback test to shut the server down cleanly; the
// production entry point (main.cpp) passes nullptr and runs forever.
//
// Returns false immediately (after logging a clear reason to stderr) on ANY
// startup failure: the named interface does not exist, the socket cannot be
// created or bound, or the IPV6_RECVPKTINFO socket option cannot be set --
// this program refuses to silently fall back to listening on every
// interface. Returns true after a clean should_stop-triggered shutdown.
bool run_server(const ServerConfig &config, Registry &registry, std::atomic<bool> *should_stop = nullptr);

} // namespace ingest
