//===- DarwinImage.cpp - Mach-O process and dependency admission ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file One bounded startup graph, relocation ledger and mapping owner.
#include "DarwinLibSystem.h"
#include "DarwinProcess.h"
#include "DarwinTLS.h"

#include "neverd/loader/BinaryImageFlags.h"
#include "neverd/loader/MachO/MachOExecutionImage.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"

#include <deque>
#include <set>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
using namespace llvm::MachO;
constexpr size_t SystemProvider = SIZE_MAX;
constexpr uint64_t DylibBase = 0x200000000ULL;
struct Node {
  MachOExecutionImage Loaded;
  MachOProgramLinking Linking;
  std::vector<size_t> Dependencies;
  std::map<std::string, MachOProgramExport> Exports;
  uint64_t Bias = 0;
  bool Active = true, Linked = false;
};
class ImageGraph {
  ProfileSpec Profile;
  const ProcessOptions &Options;
  ExecutionBudget *Budget;
  std::deque<Node> Nodes;
  std::map<std::string, const DarwinModuleInput *> Catalogue;
  std::map<std::string, size_t> Resident;
  ProcessImage Out{};
  uint64_t Available = 0, FileBytes = 0, Records = 0, ModelBytes = 0;
  uint64_t NextAddress = DylibBase;

  llvm::Error check() {
    if (Budget && !Budget->remainingMicroseconds())
      return llvm::make_error<PreparationTimeout>(
          Out.Architecture, Nodes.front().Loaded.Image.Entry);
    return llvm::Error::success();
  }
  llvm::Error charge(uint64_t Bytes) {
    if (auto E = check())
      return E;
    if (Bytes > Available - Out.MetadataBytes)
      return failure(diagnostic::ProcessLimits);
    Out.MetadataBytes += Bytes;
    return llvm::Error::success();
  }
  llvm::Error validate(const MachOExecutionImage &L, bool Main) {
    const bool X64 = L.Image.Arch == Arch::X64;
    const uint32_t Subtype =
        L.CPUSubtype & ~(X64 ? uint32_t(CPU_SUBTYPE_LIB64) : 0u);
    if (L.FileType != (Main ? MH_EXECUTE : MH_DYLIB) ||
        (X64 ? !Profile.AllowX64 : L.Image.Arch != Arch::AArch64) ||
        L.Image.Arch != Nodes.front().Loaded.Image.Arch ||
        Subtype != (X64 ? uint32_t(CPU_SUBTYPE_X86_64_ALL)
                        : uint32_t(CPU_SUBTYPE_ARM64_ALL)))
      return failure(diagnostic::ImageArchitecture);
    if (L.Platforms.size() != 1 || L.Platforms.front() != Profile.Platform)
      return failure(diagnostic::ImagePlatform);
    if ((Main ? L.MainEntries + L.ThreadEntries != 1
              : L.MainEntries || L.ThreadEntries) ||
        L.HasNonEntryThreadState || L.RequestedStackSize)
      return failure(diagnostic::ImageEntry);
    if (L.HasInitializers)
      return failure(diagnostic::ImageDependencies);
    if (L.DylibIdentities != (Main ? 0u : 1u))
      return failure("Darwin image has an invalid dylib identity");
    const uint32_t Flags = MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL |
                           MH_HAS_TLV_DESCRIPTORS |
                           (Main ? MH_PIE : MH_NO_REEXPORTED_DYLIBS);
    if (L.Encrypted || !L.OtherCommands.empty() || (L.Flags & ~Flags))
      return failure(diagnostic::ImageCommands);
    if (L.Interpreters.size() > 1 ||
        (!L.Interpreters.empty() &&
         (!Main || L.Interpreters.front() != DyldPath || !L.MainEntries)))
      return failure(diagnostic::ImageInterpreter);
    return llvm::Error::success();
  }
  llvm::Error read(size_t Index) {
    auto &N = Nodes[Index];
    const bool Main = Index == 0;
    if (auto E = validate(N.Loaded, Main))
      return E;
    N.Linked = !Main || N.Loaded.HasFixups || N.Loaded.HasTLS ||
               !N.Loaded.Dependencies.empty();
    if (N.Linked) {
      if (!(N.Loaded.Flags & MH_TWOLEVEL) || (Main && !N.Loaded.MainEntries))
        return failure(diagnostic::LibSystemNamespace);
      auto Facts = readMachOProgramLinking(N.Loaded.Image.Raw,
                                           Available - Out.MetadataBytes,
                                           [&] { return check(); });
      if (!Facts)
        return Facts.takeError();
      if (Facts->Records > 65536 - Records)
        return failure("Darwin startup graph exceeds its record limit");
      Records += Facts->Records;
      if (auto E = charge(Facts->MetadataBytes + 256))
        return E;
      N.Linking = std::move(*Facts);
      if (Main ? bool(N.Linking.Identity) : !N.Linking.Identity)
        return failure("Darwin image has an invalid dylib identity");
      for (const auto &Export : N.Linking.Exports)
        N.Exports.emplace(Export.Name, Export);
    }
    for (const auto &D : N.Linking.Dependencies) {
      if (auto E = check())
        return E;
      if (D.Command != LC_LOAD_DYLIB)
        return failure(diagnostic::LibSystemProvider);
      if (D.Name == LibSystemPath) {
        if (D.CompatibilityVersion > LibSystemCompatibilityVersion)
          return failure(diagnostic::LibSystemProvider);
        N.Dependencies.push_back(SystemProvider);
        continue;
      }
      size_t Target = 0;
      auto Existing = Resident.find(D.Name);
      if (Existing != Resident.end()) {
        Target = Existing->second;
        if (Nodes[Target].Active)
          return failure("Darwin startup dylib dependency cycle");
      } else {
        auto Input = Catalogue.find(D.Name);
        if (Input == Catalogue.end())
          return failure(diagnostic::LibSystemProvider);
        if (Nodes.size() > DarwinModuleLimit)
          return failure("Darwin startup graph has too many images");
        if (auto E = check())
          return E;
        auto Loaded = loadMachOExecutionImage(Input->second->Path,
                                              Options.MemoryLimit - FileBytes);
        if (!Loaded)
          return Loaded.takeError();
        FileBytes += Loaded->Image.Raw.size();
        Target = Nodes.size();
        Nodes.push_back({std::move(*Loaded), {}, {}, {}});
        Resident.emplace(D.Name, Target);
        if (auto E = read(Target))
          return E;
      }
      const auto &ID = Nodes[Target].Linking.Identity;
      if (!ID || ID->Name != D.Name ||
          ID->CurrentVersion < ID->CompatibilityVersion ||
          ID->CurrentVersion < D.CompatibilityVersion)
        return failure(
            "Darwin dylib identity or compatibility version mismatch");
      N.Dependencies.push_back(Target);
    }
    N.Active = false;
    return check();
  }
  llvm::Error map(Node &N, bool Main) {
    auto &Image = N.Loaded.Image;
    const uint64_t Page = Out.Memory.PageSize;
    if (!Main) {
      if (NextAddress >= UserLimit || Image.Base > NextAddress)
        return failure("Darwin dylib preferred base exceeds its placement");
      N.Bias = NextAddress - Image.Base;
    }
    unsigned PageZeroCount = 0;
    std::vector<Segment> Mapped;
    for (size_t I = 0; I < Image.Segments.size(); ++I) {
      if (auto E = check())
        return E;
      const auto &Segment = Image.Segments[I];
      const auto &Info = N.Loaded.Segments[I];
      if (Segment.VA % Page || Segment.FileOff % Page ||
          Segment.Size > UINT64_MAX - (Page - 1) ||
          (Info.Flags &&
           (!N.Linked || Info.Flags != SG_READ_ONLY ||
            Info.InitialProtection != 3 || Info.MaximumProtection != 3)) ||
          (Info.InitialProtection & ~7u) || (Info.MaximumProtection & ~7u) ||
          (Info.InitialProtection & ~Info.MaximumProtection))
        return failure(diagnostic::SegmentPermissions);
      if (Segment.Name == PageZeroSegment) {
        if (!Main || ++PageZeroCount > 1 || Segment.VA || Segment.FileSz ||
            !Segment.Size || Segment.Size % Page || Info.InitialProtection ||
            Info.MaximumProtection)
          return failure(diagnostic::PageZeroReservation);
        Out.Memory.MinimumAddress = std::max(MinimumAddress, Segment.Size);
        continue;
      }
      if (!Segment.Size)
        continue;
      if (Segment.VA < Image.Base || Segment.VA > UserLimit - N.Bias ||
          Segment.Size > UserLimit - (Segment.VA + N.Bias))
        return failure(diagnostic::ImageOverlap);
      if (!Segment.FileOff && Segment.FileSz &&
          (Info.InitialProtection & (VM_PROT_READ | VM_PROT_EXECUTE)) !=
              (VM_PROT_READ | VM_PROT_EXECUTE))
        return failure(diagnostic::MachHeaderPermissions);
      if (Info.InitialProtection && !(Info.InitialProtection & VM_PROT_READ))
        return failure(diagnostic::ReadableSegment);
      Out.Memory.Maximum.push_back({Segment.VA + N.Bias,
                                    (Segment.Size + Page - 1) & ~(Page - 1),
                                    Info.MaximumProtection});
      auto Copy = Segment;
      if (Info.Flags & SG_READ_ONLY)
        Copy.Flags = machoProtToNd(Info.InitialProtection & ~VM_PROT_WRITE);
      Mapped.push_back(std::move(Copy));
    }
    Image.Segments = std::move(Mapped);
    if (Out.Plan.MappedBytes + ModelBytes > Available - Out.MetadataBytes)
      return failure(diagnostic::ProcessLimits);
    auto Plan = ImageMappingPlan::create(
        Image, N.Bias, Page,
        Available - Out.MetadataBytes - ModelBytes - Out.Plan.MappedBytes, true,
        ImagePagePadding::FilePagesPreserveTail, ImageByteSource::OriginalFile);
    if (!Plan)
      return Plan.takeError();
    for (auto &R : Plan->Regions) {
      const uint64_t End = R.Address + R.Bytes.size();
      if (R.Address < Out.Memory.MinimumAddress || End > UserLimit ||
          (R.Address < StackTop + Page &&
           End > StackTop - Options.StackSize - Page) ||
          (R.Address < ReturnGate + Page && End > ReturnGate))
        return failure(diagnostic::ImageOverlap);
      for (const auto &Reserved : Out.Memory.Reserved)
        if (R.Address < Reserved.Address + Reserved.Size &&
            End > Reserved.Address)
          return failure(diagnostic::ImageOverlap);
      for (const auto &Previous : Out.Plan.Regions)
        if (R.Address < Previous.Address + Previous.Bytes.size() &&
            End > Previous.Address)
          return failure(diagnostic::ImageOverlap);
      NextAddress = std::max(NextAddress, End + Page);
      Out.Plan.Regions.push_back(std::move(R));
    }
    Out.Plan.MappedBytes += Plan->MappedBytes;
    return llvm::Error::success();
  }
  llvm::Error link(const Node &N) {
    const bool Lazy = llvm::any_of(N.Linking.Fixups, [](const auto &F) {
      return F.Origin == MachOProgramFixupOrigin::ClassicLazyBind;
    });
    for (const auto &F : N.Linking.Fixups) {
      if (auto E = charge(sizeof(PreparedFixup)))
        return E;
      uint64_t Target = F.Target + N.Bias;
      if (F.Bind) {
        const auto Provider = N.Dependencies[F.LibraryOrdinal - 1];
        if (Provider == SystemProvider) {
          auto Binding = libSystemImport(F.Symbol);
          if (!Binding || F.WeakImport || F.Addend ||
              (*Binding == LibSystemGateKind::StubBinder &&
               (!Lazy || F.Origin != MachOProgramFixupOrigin::ClassicBind)))
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                llvm::Twine(diagnostic::LibSystemImport) + F.Symbol);
          if (*Binding == LibSystemGateKind::TLVBootstrap &&
              !Out.TLSDescriptors.contains(F.Address + N.Bias))
            return failure(
                "Darwin TLV bootstrap requires a declared descriptor");
          Target = LibSystemGate + unsigned(*Binding) * LibSystemGateStride;
          if (!Out.Imports.contains(Target)) {
            if (auto E = charge(128))
              return E;
            Out.Imports.emplace(Target, *Binding);
          }
        } else {
          if (F.WeakImport || F.Addend)
            return failure(
                "Darwin weak imports and imported addends are unsupported");
          const auto &Library = Nodes[Provider];
          auto Export = Library.Exports.find(F.Symbol);
          if (Export == Library.Exports.end())
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                "Darwin dylib has no exported symbol: " +
                    llvm::Twine(F.Symbol));
          Target = Export->second.Address + Library.Bias;
        }
      }
      Out.Fixups.push_back({F.Address + N.Bias, Target});
    }
    return llvm::Error::success();
  }

public:
  ImageGraph(ProfileSpec Profile, const ProcessOptions &Options,
             ExecutionBudget *Budget)
      : Profile(Profile), Options(Options), Budget(Budget) {}
  llvm::Expected<ProcessImage> load(const std::filesystem::path &Path) {
    auto Loaded = loadMachOExecutionImage(Path, Options.MemoryLimit);
    if (!Loaded)
      return Loaded.takeError();
    FileBytes = Loaded->Image.Raw.size();
    Out.Architecture = Loaded->Image.Arch == Arch::X64
                           ? GuestArchitecture::X64
                           : GuestArchitecture::AArch64;
    Out.Memory = {Out.Architecture == GuestArchitecture::X64 ? PageSizeX64
                                                             : PageSizeARM64,
                  MinimumAddress,
                  {}};
    Out.MainEntry = Loaded->MainEntries == 1;
    Out.Plan = {Loaded->Image.Entry, Loaded->Image.Base, 0, {}};
    Nodes.push_back({std::move(*Loaded), {}, {}, {}});
    const uint64_t Page = Out.Memory.PageSize;
    if (Options.StackSize % Page || Options.StackSize >= Options.MemoryLimit ||
        Page > Options.MemoryLimit - Options.StackSize)
      return failure(diagnostic::StackAlignment);
    Available = Options.MemoryLimit - Options.StackSize - Page;
    if (Options.Darwin) {
      if (Options.Darwin->Modules.size() > DarwinModuleLimit)
        return failure("Darwin module catalogue exceeds its limit");
      for (const auto &M : Options.Darwin->Modules) {
        const auto PathText = pathToUTF8(M.Path);
        if (M.Name.empty() || M.Name.size() > DarwinModuleNameLimit ||
            M.Name.find('\0') != std::string::npos || M.Name == LibSystemPath ||
            PathText.empty() || PathText.find('\0') != std::string::npos ||
            !Catalogue.emplace(M.Name, &M).second)
          return failure(
              "Darwin module catalogue has an invalid or duplicate name/path");
        if (auto E = charge(2 * M.Name.size() + PathText.size() + 256))
          return std::move(E);
      }
    }
    if (auto E = read(0))
      return std::move(E);
    // Reserve both provider pages before allowing any image to consume them.
    const bool Imports = llvm::any_of(Nodes, [](const auto &N) {
      return llvm::any_of(N.Linking.Fixups, [&](const auto &F) {
        return F.Bind && N.Dependencies[F.LibraryOrdinal - 1] == SystemProvider;
      });
    });
    if (Imports) {
      ModelBytes = 2 * Page;
      Out.Memory.Reserved = {{LibSystemGate, Page, 5},
                             {LibSystemGate + Page, Page, 3}};
      Out.Memory.Maximum = Out.Memory.Reserved;
    }
    for (size_t I = 0; I < Nodes.size(); ++I)
      if (auto E = map(Nodes[I], I == 0))
        return std::move(E);
    for (const auto &N : Nodes) {
      if (auto E = prepareTLSImage(Out, N.Loaded, N.Linking, N.Bias,
                                   NextAddress, Available - ModelBytes, Records,
                                   [&] { return check(); }))
        return std::move(E);
    }
    for (const auto &N : Nodes) {
      for (const auto &E : N.Linking.Exports)
        if (E.ThreadLocal && !Out.TLSDescriptors.contains(E.Address + N.Bias))
          return failure("Darwin thread-local export is not a descriptor");
      if (auto E = link(N))
        return std::move(E);
    }
    if (Out.Plan.MappedBytes + ModelBytes > Available - Out.MetadataBytes)
      return failure(diagnostic::ProcessLimits);
    if (auto E = check())
      return std::move(E);
    return std::move(Out);
  }
};
} // namespace
llvm::Expected<ProcessImage> loadImage(const std::filesystem::path &Path,
                                       ProfileSpec Profile,
                                       const ProcessOptions &Options,
                                       ExecutionBudget *Budget) {
  return ImageGraph(Profile, Options, Budget).load(Path);
}
} // namespace neverd::emulation::darwin_model
