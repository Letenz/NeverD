//===- DriverMSR.cpp - Declared read-only MSR inputs
//-----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded, explicit environment inputs; no host MSR access or memory rights.
///
//===----------------------------------------------------------------------===//

#include "DriverMSR.h"

#include "../../../arch/x86_64/X64MSRRead.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>

namespace neverd::emulation::driver_msr {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError("driver msr_reads: " + Text);
}
} // namespace

llvm::Error validate(llvm::ArrayRef<DriverMSRRead> Entries) {
  if (Entries.size() > profile::MaxMSRReadEntries)
    return invalid("inventory exceeds the declared entry limit");
  for (size_t I = 0; I < Entries.size(); ++I) {
    if (!x64::msr::supportsDeclaredRead(Entries[I].Index))
      return invalid("unsupported MSR selector");
    for (size_t J = 0; J < I; ++J)
      if (Entries[I].Index == Entries[J].Index)
        return invalid("duplicate MSR selector");
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<DriverMSRRead>>
parse(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > profile::MaxMSRReadEntries)
    return invalid("inventory must be a bounded array");
  std::vector<DriverMSRRead> Entries;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object || Object->size() != 2)
      return invalid("each declaration requires index and value");
    auto Index = Object->getInteger("index");
    auto Text = Object->getString("value");
    if (!Index || *Index < 0 || uint64_t(*Index) > UINT32_MAX)
      return invalid("index must be a 32-bit unsigned integer");
    uint64_t Bits = 0;
    if (!Text || !(Text->consume_front("0x") || Text->consume_front("0X")) ||
        Text->empty() || Text->size() > 16 ||
        !std::all_of(Text->begin(), Text->end(), llvm::isHexDigit) ||
        Text->getAsInteger(16, Bits))
      return invalid(
          "value must be a 64-bit hexadecimal string beginning with 0x");
    Entries.push_back({uint32_t(*Index), Bits});
  }
  if (auto E = validate(Entries))
    return std::move(E);
  return Entries;
}

llvm::json::Array toJSON(llvm::ArrayRef<DriverMSRRead> Entries) {
  llvm::json::Array Array;
  for (const auto &Entry : Entries)
    Array.push_back(llvm::json::Object{
        {"index", Entry.Index},
        {"value", llvm::formatv("{0:x}", Entry.Value).str()}});
  return Array;
}

llvm::Expected<uint64_t> query(llvm::ArrayRef<DriverMSRRead> Entries,
                               uint32_t Index) {
  if (!x64::msr::supportsDeclaredRead(Index))
    return invalid(llvm::formatv("unsupported selector {0:x}", Index).str());
  for (const auto &Entry : Entries)
    if (Entry.Index == Index)
      return Entry.Value;
  return invalid(llvm::formatv("undeclared selector {0:x}", Index).str());
}
} // namespace neverd::emulation::driver_msr
