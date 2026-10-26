//===- X86AtomicExchange.h - Native memory XCHG contract ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A memory exchange is implicitly locked and accepts unaligned addresses.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86ATOMICEXCHANGE_H
#define NEVERD_BACKEND_LLVM_X86_X86ATOMICEXCHANGE_H

#include "neverd/Common.h"
#include "neverd/ir/NdOps.h"

#include <optional>
#include <string>
#include <string_view>

namespace neverd {

inline constexpr std::string_view kX86AtomicExchangeAsmMarker =
    "# neverd.x86.exchange ";

struct X86AtomicExchangePlan {
  Arch Architecture;
  unsigned Bytes;
  NdMemoryAddressSpace AddressSpace;

  unsigned addressBytes() const { return Architecture == Arch::X86 ? 4 : 8; }
  std::string mnemonic() const {
    return Bytes == 1   ? "xchgb"
           : Bytes == 2 ? "xchgw"
           : Bytes == 4 ? "xchgl"
                        : "xchgq";
  }
  std::string segment() const {
    return AddressSpace == NdMemoryAddressSpace::X86FS   ? "fs"
           : AddressSpace == NdMemoryAddressSpace::X86GS ? "gs"
                                                         : "";
  }
  std::string constraints() const {
    // The byte output must be an encodable low-byte register. The address
    // and value are both inputs until the exchange completes.
    return Bytes == 1 ? "=q,r,0,~{memory}" : "=r,r,0,~{memory}";
  }
  std::string assembly() const {
    const auto Prefix = segment().empty() ? "" : "%" + segment() + ":";
    return std::string(kX86AtomicExchangeAsmMarker) + std::to_string(Bytes) +
           " " + std::to_string(static_cast<unsigned>(AddressSpace)) + "\n" +
           mnemonic() + " $0, " + Prefix + "($1)";
  }
};

inline std::optional<X86AtomicExchangePlan>
x86AtomicExchangePlan(Arch Architecture, unsigned Bytes,
                      NdMemoryAddressSpace AddressSpace) {
  if ((Architecture != Arch::X86 && Architecture != Arch::X64) ||
      (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8) ||
      (Architecture == Arch::X86 && Bytes == 8))
    return std::nullopt;
  switch (AddressSpace) {
  case NdMemoryAddressSpace::Default:
  case NdMemoryAddressSpace::X86FS:
  case NdMemoryAddressSpace::X86GS:
    return X86AtomicExchangePlan{Architecture, Bytes, AddressSpace};
  }
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86ATOMICEXCHANGE_H
