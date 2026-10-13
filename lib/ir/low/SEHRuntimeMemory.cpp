//===- SEHRuntimeMemory.cpp - Runtime frame-effect freshness -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds runtime memory receipts to their complete source catalog.

#include "neverd/ir/low/SEHRuntimeMemory.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

namespace neverd {

std::string lowSEHRuntimeMemoryDigest(const LowSEHRuntimeMemory &Memory) {
  constexpr size_t MaxCalls = 256, MaxFormats = 4096, MaxBytes = 65536;
  if (Memory.calls().size() > MaxCalls ||
      Memory.scalarFormats().size() > MaxFormats ||
      Memory.writableImageRanges().size() > MaxCalls)
    return {};
  llvm::SHA256 Hash;
  Hash.update("neverd-low-seh-runtime-memory-v1");
  const auto Word = [&](uint64_t Value) {
    uint8_t Bytes[8];
    for (unsigned I = 0; I != 8; ++I)
      Bytes[I] = Value >> (I * 8);
    Hash.update(Bytes);
  };
  const auto String = [&](const std::string &Text) {
    Word(Text.size());
    Hash.update(Text);
  };
  Word(Memory.calls().size());
  for (const auto &[Identity, Call] : Memory.calls()) {
    if (!Identity.first || !Call.IATAddress ||
        Call.IATAddress > InvalidVA - 8 || Call.Module.empty() ||
        Call.Module.size() > 256 || Call.Name.empty() ||
        Call.Name.size() > 256 ||
        Call.Routine > LowSEHRuntimeCall::Kind::CRTControlFP)
      return {};
    Word(Identity.first);
    Word(Identity.second);
    Word(static_cast<unsigned>(Call.Routine));
    Word(Call.IATAddress);
    String(Call.Module);
    String(Call.Name);
  }
  size_t Bytes = 0;
  Word(Memory.scalarFormats().size());
  for (const auto &[Address, Format] : Memory.scalarFormats()) {
    if (!Address || Format.empty() || Format.back() ||
        Format.size() > MaxBytes - Bytes ||
        Address > InvalidVA - Format.size() ||
        Format.find('\0') != Format.size() - 1)
      return {};
    Bytes += Format.size();
    Word(Address);
    String(Format);
  }
  Word(Memory.writableImageRanges().size());
  va_t End = 0;
  for (const auto &Range : Memory.writableImageRanges()) {
    if (Range.Begin >= Range.End || Range.Begin < End)
      return {};
    Word(Range.Begin);
    Word(Range.End);
    End = Range.End;
  }
  return llvm::toHex(Hash.final(), true);
}

} // namespace neverd
