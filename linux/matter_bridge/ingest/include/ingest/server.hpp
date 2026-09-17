// UDP CoAP server loop -- the ONLY file in this program that touches
// sockets. Everything it does with a received datagram (parse, validate,
// apply, decide a response code, ACK) is delegated to
// ingest::coap_parse()/ingest::handle_request()/ingest::coap_build_ack(),
// so this stays swappable for a real libcoap3 server later without
// touching registry.hpp/session.hpp at all (see minicoap.hpp's own top
// comment for why this program uses a minimal internal parser instead of
// libcoap3 today).
#pragma once

#include <atomic>
#include <cstdint>

#include "ingest/registry.hpp"

namespace ingest {

// Binds a UDP/IPv6 socket on `port` and serves forever, applying every
// well-formed request to `registry`. If `should_stop` is non-null, the loop
// checks it (with a short, bounded poll interval) and returns once it
// becomes true -- used by the loopback test to shut the server down
// cleanly; the production entry point (main.cpp) passes nullptr and runs
// forever. Returns immediately (after logging to stderr) if the socket
// cannot be created or bound.
void run_server(uint16_t port, Registry &registry, std::atomic<bool> *should_stop = nullptr);

} // namespace ingest
