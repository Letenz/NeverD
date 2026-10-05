//===- ProcessLinuxFilesJSON.cpp - Bounded explicit file inputs ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessLinuxFilesJSON.h"

#include "../os/linux/kernel/LinuxFiles.h"

#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::LinuxFilesOptions + Name);
}
} // namespace

llvm::Expected<LinuxFileOptions>
linuxFileOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || !Object->get(field::Files))
    return invalid(field::LinuxFiles);
  LinuxFileOptions Out;
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::DescriptorLimit) {
      auto N = V.getAsUINT64();
      if (!N || *N > UINT32_MAX)
        return invalid(Name);
      Out.DescriptorLimit = *N;
      continue;
    }
    if (Name != field::Files)
      return invalid(Name);
    const auto *Files = V.getAsArray();
    if (!Files || Files->size() > linux_model::FileCountLimit)
      return invalid(Name);
    for (const auto &File : *Files) {
      const auto *F = File.getAsObject();
      if (!F || F->size() != 2)
        return invalid(Name);
      auto Path = F->getString(field::Path);
      auto Bytes = F->getString(field::Bytes);
      if (!Path || !Bytes || Bytes->size() % 2 ||
          !llvm::all_of(*Bytes, llvm::isHexDigit))
        return invalid(Name);
      std::string Data = llvm::fromHex(*Bytes);
      if (!Out.Files
               .emplace(Path->str(),
                        std::vector<uint8_t>(Data.begin(), Data.end()))
               .second)
        return invalid(field::Path);
    }
  }
  if (auto E = linux_model::validateFileOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
