//===- DriverProcessPeb.cpp - Declared process environment inputs
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "DriverProcessPeb.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>

namespace neverd::emulation::driver_process_peb {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError("driver process_pebs: " + Text);
}
} // namespace

llvm::Error validate(llvm::ArrayRef<DriverProcessPeb> Entries) {
  if (Entries.size() > profile::MaxProcessPebEntries)
    return invalid("inventory exceeds the declared entry limit");
  for (size_t I = 0; I < Entries.size(); ++I) {
    if (!Entries[I].ProcessID)
      return invalid("process_id must be nonzero");
    for (size_t J = 0; J < I; ++J)
      if (Entries[I].ProcessID == Entries[J].ProcessID)
        return invalid("duplicate process identity");
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<DriverProcessPeb>>
parse(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > profile::MaxProcessPebEntries)
    return invalid("inventory must be a bounded array");
  std::vector<DriverProcessPeb> Entries;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object || Object->size() != 2)
      return invalid("each declaration requires process_id and value");
    auto ProcessID = Object->getInteger("process_id");
    auto Text = Object->getString("value");
    if (!ProcessID || *ProcessID <= 0 || uint64_t(*ProcessID) > UINT32_MAX)
      return invalid("process_id must be a nonzero 32-bit unsigned integer");
    uint64_t Bits = 0;
    if (!Text || !(Text->consume_front("0x") || Text->consume_front("0X")) ||
        Text->empty() || Text->size() > 16 ||
        !std::all_of(Text->begin(), Text->end(), llvm::isHexDigit) ||
        Text->getAsInteger(16, Bits))
      return invalid(
          "value must be a 64-bit hexadecimal string beginning with 0x");
    Entries.push_back({uint32_t(*ProcessID), Bits});
  }
  if (auto E = validate(Entries))
    return std::move(E);
  return Entries;
}

llvm::json::Array toJSON(llvm::ArrayRef<DriverProcessPeb> Entries) {
  llvm::json::Array Array;
  for (const auto &Entry : Entries)
    Array.push_back(llvm::json::Object{
        {"process_id", Entry.ProcessID},
        {"value", llvm::formatv("{0:x}", Entry.Value).str()}});
  return Array;
}

llvm::Expected<uint64_t> query(llvm::ArrayRef<DriverProcessPeb> Entries,
                               uint32_t ProcessID) {
  for (const auto &Entry : Entries)
    if (Entry.ProcessID == ProcessID)
      return Entry.Value;
  return invalid("undeclared process identity");
}
} // namespace neverd::emulation::driver_process_peb
