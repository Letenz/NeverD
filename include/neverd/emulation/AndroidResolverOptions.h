//===- AndroidResolverOptions.h - Offline resolver inputs -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded offline network contracts and their semantic checks.

#ifndef NEVERD_EMULATION_ANDROIDRESOLVEROPTIONS_H
#define NEVERD_EMULATION_ANDROIDRESOLVEROPTIONS_H
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {
struct AndroidResolverAddress {
  uint32_t SocketType = 1, Protocol = 6;
  /// Linux sockaddr_in or sockaddr_in6, including the resolved service port.
  std::vector<uint8_t> Address;
  std::optional<std::string> CanonicalName;
};
/// One exact getaddrinfo observation. Null node/service differs from empty.
/// The complete ordered result is explicit: no host DNS, hosts file, service
/// database, interface configuration, address sorting or default domain.
struct AndroidResolverQuery {
  std::optional<std::string> Node, Service;
  uint32_t Flags = 0, Family = 0, SocketType = 0, Protocol = 0;
  uint32_t Error = 0, SystemErrno = 0;
  std::vector<AndroidResolverAddress> Addresses;
};
} // namespace neverd::emulation
#endif
