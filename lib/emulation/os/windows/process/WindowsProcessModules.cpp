//===- WindowsProcessModules.cpp - Explicit acyclic PE module linking ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessModules.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <functional>

namespace neverd::emulation::windows_process {
using namespace value;
llvm::Expected<std::string> moduleName(llvm::StringRef Name) {
  if (Name.size() >= windows_process_limits::NameBytes ||
      Name.size() <= sizeof(text::DLLExtension) - 1 ||
      !Name.ends_with_insensitive(text::DLLExtension) ||
      !llvm::all_of(Name,
                    [](char C) {
                      return llvm::isAlnum(C) || C == '_' || C == '-' ||
                             C == '.' || C == ' ';
                    }) ||
      Name.front() == ' ' || Name.front() == '.')
    return failure(text::ModuleName + Name);
  return Name.lower();
}

llvm::Expected<Program> loadProgram(const std::filesystem::path &Path,
                                    const ProcessOptions &Options,
                                    const ExecutionBudget &Budget,
                                    VirtualMemory &Memory) {
  auto CheckTime = [&]() -> llvm::Error {
    return Budget.remainingMicroseconds() ? llvm::Error::success()
                                          : failure(text::ModuleTimeout);
  };
  if (auto E = CheckTime())
    return std::move(E);
  std::map<std::string, std::filesystem::path> Catalogue;
  if (Options.Windows) {
    if (Options.Windows->Modules.size() > windows_process_limits::Modules)
      return failure(text::ModuleBudget);
    for (const auto &Input : Options.Windows->Modules) {
      auto Name = moduleName(Input.Name);
      if (!Name)
        return Name.takeError();
      if (Input.Path.empty() ||
          Input.Path.native().find(typename std::filesystem::path::value_type(
              0)) != std::filesystem::path::string_type::npos ||
          findProvider(*Name) || !Catalogue.emplace(*Name, Input.Path).second)
        return failure(text::ModuleName + Input.Name);
    }
  }
  const uint64_t RuntimeBytes =
      EnvironmentEnd - TEB + GateSize + Options.StackSize;
  if (RuntimeBytes >= Options.MemoryLimit)
    return failure(text::ModuleBudget);
  Program Out;
  Out.Reads = {Options.MemoryLimit, Options.MemoryLimit - RuntimeBytes};
  std::map<std::string, size_t> LoadedNames;
  std::vector<bool> Loading;
  std::vector<std::vector<size_t>> Edges;
  std::function<llvm::Error(const std::filesystem::path &, std::string, bool)>
      Visit;
  Visit = [&](const std::filesystem::path &File, std::string Name,
              bool DLL) -> llvm::Error {
    if (auto E = CheckTime())
      return E;
    if (auto I = LoadedNames.find(Name); I != LoadedNames.end())
      return Loading[I->second] ? failure(text::ModuleCycle + Name)
                                : llvm::Error::success();
    if (Out.Modules.size() > windows_process_limits::Modules)
      return failure(text::ModuleBudget);
    auto Image = loadProgramImage(File, Out.Reads, DLL);
    if (!Image)
      return Image.takeError();
    if (auto E = CheckTime())
      return E;
    if (DLL && Image->Architecture != Out.Modules.front().Loaded.Architecture)
      return failure(text::ModuleISA);
    auto Base = Memory.reserveImage(Image->Base, Image->Size,
                                    DLL && Image->Relocatable);
    if (!Base)
      return Base.takeError();
    if (auto E = relocateImage(*Image, *Base, Out.Reads))
      return E;
    const size_t Index = Out.Modules.size();
    LoadedNames.emplace(Name, Index);
    Loading.push_back(true);
    Edges.emplace_back();
    Out.Identities.push_back({Name, Image->Base, Image->Size, Image->Entry});
    Module M{std::move(*Image), {}, {}, {}};
    for (size_t I = 0; I < M.Loaded.Exports.Entries.size(); ++I) {
      const auto &E = M.Loaded.Exports.Entries[I];
      M.Ordinals.emplace(E.Ordinal, I);
      for (const auto &Name : E.Names)
        M.Names.emplace(Name, I);
    }
    auto Ranges = M.Loaded.Exports.Metadata;
    // Export decoding reads PE headers separately from RVA metadata. Include
    // them even when the original image has no export directory.
    for (const auto &R : M.Loaded.Regions)
      if (R.Address == M.Loaded.Base)
        Ranges.push_back({0, R.ContentSize});
    std::sort(Ranges.begin(), Ranges.end(),
              [](const auto &A, const auto &B) { return A.RVA < B.RVA; });
    for (const auto &R : Ranges) {
      if (!M.ExportMetadata.empty() &&
          R.RVA <= M.ExportMetadata.back().RVA + M.ExportMetadata.back().Size) {
        auto &Last = M.ExportMetadata.back();
        Last.Size = std::max(Last.Size, R.RVA + R.Size - Last.RVA);
      } else
        M.ExportMetadata.push_back(R);
    }
    Out.Modules.push_back(std::move(M));
    // A recursive append may move the module vector; retain indices, not
    // borrows.
    const auto Dependencies = Out.Modules[Index].Loaded.Dependencies;
    for (const auto &Dependency : Dependencies) {
      if (findProvider(Dependency))
        continue;
      auto Key = moduleName(Dependency);
      if (!Key)
        return Key.takeError();
      auto Input = Catalogue.find(*Key);
      if (Input == Catalogue.end())
        return failure(text::ModuleMissing + Dependency);
      if (auto E = Visit(Input->second, *Key, true))
        return E;
      Edges[Index].push_back(LoadedNames.at(*Key));
    }
    Loading[Index] = false;
    return llvm::Error::success();
  };
  const auto FileName = Path.filename().u8string();
  const std::string MainName(reinterpret_cast<const char *>(FileName.data()),
                             FileName.size());
  if (Catalogue.contains(llvm::StringRef(MainName).lower()))
    return failure(text::ModuleName + MainName);
  if (auto E = Visit(Path, MainName, false))
    return std::move(E);
  // The registry is small and bounded. Reserve its gates without inspecting
  // unused forwarders, which must not load modules or fail preparation.
  for (const char *Provider : {text::Kernel32, text::KernelBase, text::NTDLL})
    for (const auto &S : services()) {
      if (findProvider(Provider) != S.Provider)
        continue;
      if (Out.Gates.size() == MaxImports)
        return failure(text::ModuleBudget);
      const uint64_t Gate =
          GateBase + (FirstImportGate + Out.Gates.size()) * GateStride;
      Out.Gates.push_back({0, &S, Provider, Gate, S.Name, std::nullopt});
      Out.ServiceGates.emplace(std::pair{Provider, S.Name}, Gate);
    }
  auto Forward = [&](size_t Owner,
                     llvm::StringRef Name) -> llvm::Expected<size_t> {
    auto Found = LoadedNames.find(Name.str());
    if (Found == LoadedNames.end()) {
      auto Input = Catalogue.find(Name.str());
      if (Input == Catalogue.end())
        return failure(text::ModuleMissing + Name);
      if (auto E = Visit(Input->second, Name.str(), true))
        return std::move(E);
      Found = LoadedNames.find(Name.str());
    }
    // Distinct symbols may forward within one module without an init cycle.
    if (Owner != Found->second)
      Edges[Owner].push_back(Found->second);
    return Found->second;
  };
  // Resolving an import can append another complete module graph. Keep indices
  // and copies, not references into either vector, across that operation.
  for (size_t M = 0; M < Out.Modules.size(); ++M) {
    if (auto E = CheckTime())
      return std::move(E);
    for (size_t N = 0; N < Out.Modules[M].Loaded.Imports.size(); ++N) {
      const auto I = Out.Modules[M].Loaded.Imports[N];
      uint64_t Address = 0;
      if (I.Target) {
        Address = Out.ServiceGates.at({I.Module, I.Name});
      } else {
        auto Provider = LoadedNames.find(I.Module);
        if (Provider == LoadedNames.end())
          return failure(text::ModuleMissing + I.Module);
        auto Target = resolveExport(Out, Provider->second, I.Name, I.Ordinal,
                                    Budget, nullptr, Forward);
        if (!Target)
          return Target.takeError();
        if (!*Target)
          return failure(
              text::ModuleExport + I.Module +
              llvm::Twine(text::ImportSeparator) +
              (I.Ordinal ? llvm::Twine(*I.Ordinal) : llvm::Twine(I.Name)));
        Address = **Target;
      }
      Out.Modules[M].Loaded.Imports[N].Gate = Address;
    }
  }
  std::vector<bool> Ordered(Out.Modules.size());
  std::function<llvm::Error(size_t)> Order = [&](size_t Index) -> llvm::Error {
    if (auto E = CheckTime())
      return E;
    if (Loading[Index])
      return failure(text::ModuleCycle + Out.Identities[Index].Name);
    if (Ordered[Index])
      return llvm::Error::success();
    Loading[Index] = true;
    for (size_t Dependency : Edges[Index])
      if (auto E = Order(Dependency))
        return E;
    Loading[Index] = false;
    Ordered[Index] = true;
    if (Index)
      Out.InitializationOrder.push_back(Index);
    return llvm::Error::success();
  };
  if (auto E = Order(0))
    return std::move(E);
  if (auto E = CheckTime())
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation::windows_process
