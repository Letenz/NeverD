//===- ProcessDarwinFilesJSON.cpp - Bounded Darwin file inputs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessDarwinFilesJSON.h"

#include "../os/darwin/kernel/DarwinFiles.h"

#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::DarwinFilesOptions + Name);
}
llvm::Expected<std::vector<uint8_t>> bytes(const llvm::json::Value &Value,
                                           uint64_t &Remaining) {
  auto Hex = Value.getAsString();
  if (!Hex || Hex->size() % 2 || Hex->size() / 2 > Remaining ||
      !llvm::all_of(*Hex, llvm::isHexDigit))
    return invalid(field::Bytes);
  Remaining -= Hex->size() / 2;
  const auto Data = llvm::fromHex(*Hex);
  return std::vector<uint8_t>(Data.begin(), Data.end());
}
} // namespace

llvm::Expected<DarwinFileOptions>
darwinFileOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || !Object->get(field::Files))
    return invalid(field::DarwinFiles);
  DarwinFileOptions Out;
  uint64_t Remaining = darwin_file_limits::Bytes;
  for (const auto &[Key, V] : *Object) {
    const llvm::StringRef Name = Key;
    if (Name == field::DescriptorLimit) {
      auto N = V.getAsUINT64();
      if (!N || *N > UINT32_MAX)
        return invalid(Name);
      Out.DescriptorLimit = *N;
    } else if (Name == field::StandardInput) {
      auto Data = bytes(V, Remaining);
      if (!Data)
        return Data.takeError();
      Out.StandardInput = std::move(*Data);
    } else if (Name == field::Files) {
      const auto *Files = V.getAsArray();
      if (!Files || Files->size() > darwin_file_limits::Files)
        return invalid(Name);
      for (const auto &File : *Files) {
        const auto *F = File.getAsObject();
        if (!F || F->size() != 2 || !F->get(field::Bytes))
          return invalid(Name);
        auto Path = F->getString(field::Path);
        if (!Path || Path->size() >= Remaining)
          return invalid(field::Path);
        Remaining -= Path->size() + 1;
        auto Data = bytes(*F->get(field::Bytes), Remaining);
        if (!Data)
          return Data.takeError();
        if (!Out.Files.emplace(Path->str(), std::move(*Data)).second)
          return invalid(field::Path);
      }
    } else {
      return invalid(Name);
    }
  }
  if (auto E = darwin_model::validateFileOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
