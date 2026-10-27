//===- LinuxNetwork.h - Offline network admission ----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded offline network contracts and their semantic checks.

#ifndef NEVERD_EMULATION_LINUXNETWORK_H
#define NEVERD_EMULATION_LINUXNETWORK_H
#include "neverd/emulation/LinuxNetworkOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

namespace neverd::emulation::linux_model {
/// Returns AF_INET/AF_INET6 only for a complete canonical Linux endpoint.
uint16_t networkAddressFamily(llvm::ArrayRef<uint8_t> Address);
bool hasTimedReceives(const LinuxNetworkOptions &Options);
llvm::Error validateNetworkOptions(const LinuxNetworkOptions &Options);
} // namespace neverd::emulation::linux_model
#endif
