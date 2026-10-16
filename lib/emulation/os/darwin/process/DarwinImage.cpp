//===- DarwinImage.cpp - Mach-O process admission ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinLibSystem.h"
#include "DarwinProcess.h"

#include "neverd/loader/BinaryImageFlags.h"
#include "neverd/loader/MachO/MachOExecutionImage.h"

#include "llvm/BinaryFormat/MachO.h"

namespace neverd::emulation::darwin_model {
using namespace value;
llvm::Expected<ProcessImage> loadImage(const std::filesystem::path &Path,
                                       ProfileSpec Profile,
                                       const ProcessOptions &Options,
                                       ExecutionBudget *Budget) {
  auto Loaded = loadMachOExecutionImage(Path, Options.MemoryLimit);
  if (!Loaded)
    return Loaded.takeError();
  auto &Image = Loaded->Image;
  using namespace llvm::MachO;
  const bool X64 = Image.Arch == Arch::X64;
  // LIB64 is a Mach-O library capability bit, not the x86_64h ISA subtype.
  const uint32_t Subtype =
      Loaded->CPUSubtype & ~(X64 ? uint32_t(CPU_SUBTYPE_LIB64) : 0u);
  if (Loaded->FileType != MH_EXECUTE ||
      (X64 ? !Profile.AllowX64 : Image.Arch != Arch::AArch64) ||
      Subtype != (X64 ? uint32_t(CPU_SUBTYPE_X86_64_ALL)
                      : uint32_t(CPU_SUBTYPE_ARM64_ALL)))
    return failure(diagnostic::ImageArchitecture);
  if (Loaded->Platforms.size() != 1 ||
      Loaded->Platforms.front() != Profile.Platform)
    return failure(diagnostic::ImagePlatform);
  if (Loaded->MainEntries + Loaded->ThreadEntries != 1 ||
      Loaded->HasNonEntryThreadState || Loaded->RequestedStackSize)
    return failure(diagnostic::ImageEntry);
  if (Loaded->HasInitializers || Loaded->HasTLS)
    return failure(diagnostic::ImageDependencies);
  if (Loaded->Encrypted || !Loaded->OtherCommands.empty() ||
      (Loaded->Flags & ~(MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_PIE)))
    return failure(diagnostic::ImageCommands);
  if (Loaded->Interpreters.size() > 1 ||
      (!Loaded->Interpreters.empty() &&
       (Loaded->Interpreters.front() != DyldPath || !Loaded->MainEntries)))
    return failure(diagnostic::ImageInterpreter);
  ProcessImage Result{X64 ? GuestArchitecture::X64 : GuestArchitecture::AArch64,
                      {X64 ? PageSizeX64 : PageSizeARM64, MinimumAddress, {}},
                      Loaded->MainEntries == 1,
                      {}};
  const uint64_t Page = Result.Memory.PageSize;
  if (Options.StackSize % Page || Options.StackSize >= Options.MemoryLimit ||
      Page > Options.MemoryLimit - Options.StackSize)
    return failure(diagnostic::StackAlignment);
  const bool Linking = !Loaded->Dependencies.empty() || Loaded->HasFixups;
  const uint64_t Available = Options.MemoryLimit - Options.StackSize - Page;
  uint64_t ModelBytes = 0;
  if (Linking) {
    if (!Result.MainEntry || !(Loaded->Flags & MH_TWOLEVEL))
      return failure(diagnostic::LibSystemNamespace);
    auto Facts =
        readMachOProgramLinking(Image.Raw, Available, [&]() -> llvm::Error {
          if (Budget && !Budget->remainingMicroseconds())
            return llvm::make_error<PreparationTimeout>(Result.Architecture,
                                                        Image.Entry);
          return llvm::Error::success();
        });
    if (!Facts)
      return Facts.takeError();
    if (Facts->Dependencies.size() > 1)
      return failure(diagnostic::LibSystemProvider);
    for (const auto &D : Facts->Dependencies)
      if (D.Command != LC_LOAD_DYLIB || D.Name != LibSystemPath ||
          D.CompatibilityVersion > LibSystemCompatibilityVersion)
        return failure(diagnostic::LibSystemProvider);
    Result.MetadataBytes = Facts->MetadataBytes;
    for (const auto &F : Facts->Fixups) {
      if (!F.Bind)
        continue;
      auto Binding = libSystemImport(F.Symbol);
      if (!Binding || F.WeakImport || F.Addend)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            llvm::Twine(diagnostic::LibSystemImport) + F.Symbol);
      const auto Address = LibSystemGate + *Binding * LibSystemGateStride;
      if (!Result.Imports.contains(Address)) {
        if (128 > Available - Result.MetadataBytes)
          return failure(diagnostic::ProcessLimits);
        Result.MetadataBytes += 128;
        Result.Imports.emplace(Address, *Binding);
      }
    }
    Result.Fixups = std::move(Facts->Fixups);
    if (!Result.Imports.empty()) {
      ModelBytes = 2 * Page;
      Result.Memory.Reserved = {{LibSystemGate, Page, 5},
                                {LibSystemGate + Page, Page, 3}};
      Result.Memory.Maximum = Result.Memory.Reserved;
    }
    if (ModelBytes > Available - Result.MetadataBytes)
      return failure(diagnostic::ProcessLimits);
  }
  unsigned PageZeroCount = 0;
  std::vector<Segment> Mapped;
  for (size_t I = 0; I < Image.Segments.size(); ++I) {
    const auto &Segment = Image.Segments[I];
    const auto &Info = Loaded->Segments[I];
    if (Segment.VA % Page || Segment.FileOff % Page ||
        Segment.Size > UINT64_MAX - (Page - 1) ||
        (Info.Flags &&
         (!Linking || Info.Flags != SG_READ_ONLY ||
          Info.InitialProtection != 3 || Info.MaximumProtection != 3)) ||
        (Info.InitialProtection & ~7u) || (Info.MaximumProtection & ~7u) ||
        (Info.InitialProtection & ~Info.MaximumProtection))
      return failure(diagnostic::SegmentPermissions);
    if (Segment.Name == PageZeroSegment) {
      if (++PageZeroCount > 1 || Segment.VA || Segment.FileSz ||
          !Segment.Size || Segment.Size % Page || Info.InitialProtection ||
          Info.MaximumProtection)
        return failure(diagnostic::PageZeroReservation);
      Result.Memory.MinimumAddress = std::max(MinimumAddress, Segment.Size);
      continue;
    }
    if (!Segment.Size)
      continue;
    if (!Segment.FileOff && Segment.FileSz &&
        (Info.InitialProtection & (VM_PROT_READ | VM_PROT_EXECUTE)) !=
            (VM_PROT_READ | VM_PROT_EXECUTE))
      return failure(diagnostic::MachHeaderPermissions);
    if (Info.InitialProtection && !(Info.InitialProtection & VM_PROT_READ))
      return failure(diagnostic::ReadableSegment);
    const bool ReadOnly = Info.Flags & SG_READ_ONLY;
    Result.Memory.Maximum.push_back({Segment.VA,
                                     (Segment.Size + Page - 1) & ~(Page - 1),
                                     Info.MaximumProtection});
    auto MappedSegment = Segment;
    if (ReadOnly)
      MappedSegment.Flags =
          machoProtToNd(Info.InitialProtection & ~VM_PROT_WRITE);
    Mapped.push_back(std::move(MappedSegment));
  }
  Image.Segments = std::move(Mapped);
  auto Plan = ImageMappingPlan::create(
      Image, 0, Page, Available - Result.MetadataBytes - ModelBytes, true,
      ImagePagePadding::FilePagesPreserveTail, ImageByteSource::OriginalFile);
  if (!Plan)
    return Plan.takeError();
  for (const auto &Region : Plan->Regions) {
    const uint64_t End = Region.Address + Region.Bytes.size();
    if (Region.Address < Result.Memory.MinimumAddress || End > UserLimit ||
        (Region.Address < StackTop + Page &&
         End > StackTop - Options.StackSize - Page) ||
        (Region.Address < ReturnGate + Page && End > ReturnGate))
      return failure(diagnostic::ImageOverlap);
    for (const auto &R : Result.Memory.Reserved)
      if (Region.Address < R.Address + R.Size && End > R.Address)
        return failure(diagnostic::ImageOverlap);
  }
  Result.Plan = std::move(*Plan);
  return Result;
}
} // namespace neverd::emulation::darwin_model
