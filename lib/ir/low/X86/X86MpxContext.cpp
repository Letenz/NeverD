//===- X86MpxContext.cpp - Architectural bounds state -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared MPX state transitions, independent of host MPX support and memory
/// implementation. See Intel SDM Volume 1 Appendix E and Volume 2A BND*.
//===----------------------------------------------------------------------===//

#include "neverd/ir/X86/X86MpxContext.h"

namespace neverd {
namespace {

bool validMode(X86MpxMode Mode) {
  return Mode == X86MpxMode::Bits32 || Mode == X86MpxMode::Bits64;
}

uint64_t wordMask(X86MpxMode Mode) {
  return Mode == X86MpxMode::Bits64 ? UINT64_MAX : UINT32_MAX;
}

} // namespace

bool X86MpxContext::configure(const X86MpxConfiguration &C) {
  // MAWA selects bits [47+MAWA:20] of a 64-bit value. Values exceeding 16
  // cannot describe an architectural address-width slice.
  if ((C.PrivilegeLevel && *C.PrivilegeLevel > 3) ||
      (C.UserAddressWidthAdjust && *C.UserAddressWidthAdjust > 16) ||
      (C.UserConfig && (*C.UserConfig & X86MpxConfigReserved)) ||
      (C.SupervisorConfig && (*C.SupervisorConfig & X86MpxConfigReserved)))
    return false;
  Configuration = C;
  return true;
}

std::optional<uint64_t> X86MpxContext::activeConfiguration() const {
  if (!Configuration.PrivilegeLevel)
    return std::nullopt;
  return *Configuration.PrivilegeLevel == 3 ? Configuration.UserConfig
                                            : Configuration.SupervisorConfig;
}

std::optional<bool> X86MpxContext::enabled() const {
  const auto Active = activeConfiguration();
  if (Configuration.Supported == false || Configuration.OSXSAVE == false ||
      (Configuration.XCR0 &&
       (*Configuration.XCR0 & X86MpxXcr0Mask) != X86MpxXcr0Mask) ||
      (Active && !(*Active & X86MpxConfigEnable)))
    return false;
  if (!Configuration.Supported || !Configuration.OSXSAVE ||
      !Configuration.XCR0 || !Active)
    return std::nullopt;
  return true;
}

bool X86MpxContext::setBounds(unsigned Register, const X86MpxBounds &Value) {
  if (Register >= Bank.size())
    return false;
  Bank[Register] = Value;
  return true;
}

std::optional<X86MpxBounds> X86MpxContext::bounds(unsigned Register) const {
  if (Register >= Bank.size())
    return std::nullopt;
  return Bank[Register];
}

X86MpxResult X86MpxContext::operationStatus(X86MpxMode Mode,
                                            unsigned Register) const {
  if (!validMode(Mode) || Register >= Bank.size())
    return X86MpxResult::InvalidInput;
  const auto Enabled = enabled();
  if (!Enabled)
    return X86MpxResult::UnknownContext;
  if (!*Enabled)
    return X86MpxResult::Disabled;
  if (!Configuration.InTransaction)
    return X86MpxResult::UnknownContext;
  return *Configuration.InTransaction ? X86MpxResult::UnsupportedTransaction
                                      : X86MpxResult::Completed;
}

void X86MpxContext::writeBounds(X86MpxMode Mode, unsigned Register,
                                uint64_t Lower, uint64_t Upper) {
  const uint64_t Mask = wordMask(Mode);
  // SDM Vol. 1 E.5.1 does not give a portable high-half guarantee across a
  // 32-bit update and subsequent mode switch. Never turn those bits into facts.
  Bank[Register] = {{Lower & Mask, Mask}, {Upper & Mask, Mask}};
}

X86MpxResult X86MpxContext::move(X86MpxMode Mode, unsigned Destination,
                                 unsigned Source) {
  if (Source >= Bank.size())
    return X86MpxResult::InvalidInput;
  const auto Result = operationStatus(Mode, Destination);
  if (Result != X86MpxResult::Completed)
    return Result;
  Bank[Destination] = Bank[Source];
  if (Mode == X86MpxMode::Bits32) {
    Bank[Destination].Lower.KnownMask &= UINT32_MAX;
    Bank[Destination].Upper.KnownMask &= UINT32_MAX;
  }
  return Result;
}

X86MpxResult X86MpxContext::make(X86MpxMode Mode, unsigned Destination,
                                 std::optional<uint64_t> Base,
                                 std::optional<uint64_t> EffectiveAddress) {
  const auto Result = operationStatus(Mode, Destination);
  if (Result != X86MpxResult::Completed)
    return Result;
  if (!Base || !EffectiveAddress)
    return X86MpxResult::UnknownOperand;
  writeBounds(Mode, Destination, *Base, ~*EffectiveAddress);
  return Result;
}

X86MpxResult X86MpxContext::check(X86MpxMode Mode, X86MpxCheck Kind,
                                  unsigned Register,
                                  std::optional<uint64_t> Value) {
  if (Kind > X86MpxCheck::UpperRaw)
    return X86MpxResult::InvalidInput;
  const auto Result = operationStatus(Mode, Register);
  if (Result != X86MpxResult::Completed)
    return Result;
  const auto Bound =
      Kind == X86MpxCheck::Lower ? Bank[Register].Lower : Bank[Register].Upper;
  const uint64_t Mask = wordMask(Mode);
  if (!Value || (Bound.KnownMask & Mask) != Mask)
    return X86MpxResult::UnknownOperand;
  uint64_t Limit = Bound.Value;
  if (Kind == X86MpxCheck::UpperComplement)
    Limit = ~Limit;
  const bool Failed = Kind == X86MpxCheck::Lower
      ? (*Value & Mask)<(Limit & Mask) : (*Value & Mask)>(Limit & Mask);
  if (!Failed)
    return Result;
  Status = 1;
  return X86MpxResult::BoundsFault;
}

X86MpxResult X86MpxContext::completeLoad(X86MpxMode Mode, unsigned Destination,
                                         uint64_t Lower, uint64_t Upper) {
  const auto Result = operationStatus(Mode, Destination);
  if (Result == X86MpxResult::Completed)
    writeBounds(Mode, Destination, Lower, Upper);
  return Result;
}

X86MpxStoreValue X86MpxContext::storeValue(X86MpxMode Mode,
                                           unsigned Register) const {
  const auto Result = operationStatus(Mode, Register);
  if (Result != X86MpxResult::Completed)
    return {Result};
  const uint64_t Mask = wordMask(Mode);
  const auto &Bound = Bank[Register];
  if ((Bound.Lower.KnownMask & Bound.Upper.KnownMask & Mask) != Mask)
    return {X86MpxResult::UnknownOperand};
  return {Result, Bound.Lower.Value & Mask, Bound.Upper.Value & Mask,
          Mode == X86MpxMode::Bits64 ? 8u : 4u};
}

X86MpxAddress X86MpxContext::directoryAddress(
    X86MpxMode Mode, std::optional<uint64_t> LinearPointerAddress) const {
  const auto Result = operationStatus(Mode, 0);
  if (Result != X86MpxResult::Completed)
    return {Result};
  if (!LinearPointerAddress)
    return {X86MpxResult::UnknownOperand};
  const uint64_t Config = *activeConfiguration();
  if (Mode == X86MpxMode::Bits32)
    return {Result,
            uint32_t((Config & ~uint64_t(0xfff)) +
                     ((uint32_t(*LinearPointerAddress) >> 12) * 4ull)),
            4};
  unsigned Adjust = 0;
  if (*Configuration.PrivilegeLevel == 3) {
    if (!Configuration.UserAddressWidthAdjust)
      return {X86MpxResult::UnknownContext};
    Adjust = *Configuration.UserAddressWidthAdjust;
  }
  const uint64_t IndexMask = (uint64_t(1) << (28 + Adjust)) - 1;
  const uint64_t Index = (*LinearPointerAddress >> 20) & IndexMask;
  return {Result, (Config & ~uint64_t(0xfff)) + (Index << 3), 8};
}

X86MpxAddress
X86MpxContext::tableAddress(X86MpxMode Mode,
                            std::optional<uint64_t> LinearPointerAddress,
                            uint64_t DirectoryEntry) {
  const auto Directory = directoryAddress(Mode, LinearPointerAddress);
  if (Directory.Result != X86MpxResult::Completed)
    return Directory;
  if (!(DirectoryEntry & 1)) {
    Status = Directory.Address | 2;
    return {X86MpxResult::BoundsFault};
  }
  if (Mode == X86MpxMode::Bits32)
    return {X86MpxResult::Completed,
            uint32_t((DirectoryEntry & ~uint64_t(3)) +
                     (((*LinearPointerAddress >> 2) & 0x3ff) << 4)),
            4};
  return {X86MpxResult::Completed,
          (DirectoryEntry & ~uint64_t(7)) +
              (((*LinearPointerAddress >> 3) & 0x1ffff) << 5),
          8};
}

X86MpxResult X86MpxContext::completeTableLoad(X86MpxMode Mode,
                                              unsigned Destination,
                                              uint64_t Lower, uint64_t Upper,
                                              uint64_t StoredPointer,
                                              uint64_t PointerValue) {
  const auto Result = operationStatus(Mode, Destination);
  if (Result != X86MpxResult::Completed)
    return Result;
  const bool Match = ((StoredPointer ^ PointerValue) & wordMask(Mode)) == 0;
  writeBounds(Mode, Destination, Match ? Lower : 0, Match ? Upper : 0);
  return Result;
}

X86MpxResult X86MpxContext::tableReadFault(X86MpxMode Mode,
                                           unsigned Destination) {
  const auto Result = operationStatus(Mode, Destination);
  if (Result != X86MpxResult::Completed)
    return Result;
  Bank[Destination] = {};
  return X86MpxResult::PartialStateUnknown;
}

X86MpxResult X86MpxContext::completeBranch(X86MpxBranch Effect) {
  if (Effect > X86MpxBranch::Initialize)
    return X86MpxResult::InvalidInput;
  if (Effect == X86MpxBranch::Preserve)
    return X86MpxResult::Completed;
  const auto Result = operationStatus(X86MpxMode::Bits64, 0);
  if (Result != X86MpxResult::Completed)
    return Result;
  if (!(*activeConfiguration() & X86MpxConfigPreserve))
    for (auto &Bound : Bank)
      Bound = {{0, UINT64_MAX}, {0, UINT64_MAX}};
  return Result;
}

X86MpxResult X86MpxContext::completeLegacyBoundsFault() {
  const auto Result = operationStatus(X86MpxMode::Bits32, 0);
  if (Result == X86MpxResult::Completed)
    Status = 0;
  return Result;
}

} // namespace neverd
