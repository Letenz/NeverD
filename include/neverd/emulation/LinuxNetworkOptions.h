//===- LinuxNetworkOptions.h - Explicit offline stream inputs ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded offline network contracts and their semantic checks.

#ifndef NEVERD_EMULATION_LINUXNETWORKOPTIONS_H
#define NEVERD_EMULATION_LINUXNETWORKOPTIONS_H

#include <cstdint>
#include <vector>

namespace neverd::emulation {
/// One ordered TCP connection attempt. Addresses are Linux sockaddr_in (16
/// bytes) or sockaddr_in6 (28 bytes), with network-order port/address bytes.
/// These are virtual peers; the model never opens a host socket.
struct LinuxStreamConnection {
  std::vector<uint8_t> Peer;
  uint32_t ConnectErrno = 0;
  /// The complete outgoing stream expected from the guest. A mismatch or
  /// exhausted contract stops before consuming any bytes from that call.
  std::vector<uint8_t> ExpectedSend;
  std::vector<uint8_t> Receive;
  /// Explicit peer EOF after Receive. False leaves an unknown waiting boundary.
  bool ReceiveEOF = false;
  /// Explicit per-call transfer ceilings permit short I/O without host timing.
  uint32_t MaxSend = 65536, MaxReceive = 65536;
};
struct LinuxNetworkOptions {
  /// Attempts are consumed only after the exact peer and guest memory pass
  /// validation. Descriptor aliases share the selected connection and cursors.
  std::vector<LinuxStreamConnection> Connections;
};
} // namespace neverd::emulation
#endif
