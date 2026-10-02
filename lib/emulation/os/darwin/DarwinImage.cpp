//===- DarwinImage.cpp - Mach-O process admission ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinProcess.h"

#include "neverd/loader/MachO/MachOExecutionImage.h"

#include "llvm/BinaryFormat/MachO.h"

namespace neverd::emulation::darwin_model {
using namespace value;
llvm::Expected<ProcessImage> loadImage(const std::filesystem::path &Path,
                                       ProfileSpec Profile,
                                       const ProcessOptions &Options) {
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
    return failure(
        "Darwin profile requires a baseline x64 or ARM64 executable; "
        "iOS devices require ARM64");
  if (Loaded->Platforms.size() != 1 ||
      Loaded->Platforms.front() != Profile.Platform)
    return failure(
        "Mach-O platform does not match the explicit Darwin profile");
  if (Loaded->MainEntries + Loaded->ThreadEntries != 1 ||
      Loaded->HasNonEntryThreadState || Loaded->RequestedStackSize)
    return failure(
        "unsupported or ambiguous Mach-O entry/thread/stack contract");
  if (!Loaded->Dependencies.empty() || Loaded->HasFixups ||
      Loaded->HasInitializers || Loaded->HasTLS)
    return failure("Darwin profile does not implement dynamic dependencies, "
                   "fixups, initializers or TLS");
  if (Loaded->Encrypted || !Loaded->OtherCommands.empty() ||
      (Loaded->Flags & ~(MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_PIE)))
    return failure(
        "unsupported Mach-O execution flags, commands or encryption");
  if (Loaded->Interpreters.size() > 1 ||
      (!Loaded->Interpreters.empty() &&
       (Loaded->Interpreters.front() != "/usr/lib/dyld" ||
        !Loaded->MainEntries)))
    return failure("unsupported Mach-O interpreter startup");
  ProcessImage Result{X64 ? GuestArchitecture::X64 : GuestArchitecture::AArch64,
                      X64 ? PageSizeX64 : PageSizeARM64,
                      MinimumAddress,
                      Loaded->MainEntries == 1,
                      {},
                      {}};
  const uint64_t Page = Result.PageSize;
  if (Options.StackSize % Page || Options.StackSize >= Options.MemoryLimit ||
      Page > Options.MemoryLimit - Options.StackSize)
    return failure(
        "Darwin stack size must be aligned to the guest OS page size");
  unsigned PageZeroCount = 0;
  std::vector<Segment> Mapped;
  for (size_t I = 0; I < Image.Segments.size(); ++I) {
    const auto &Segment = Image.Segments[I];
    const auto &Info = Loaded->Segments[I];
    if (Segment.VA % Page || Segment.FileOff % Page ||
        Segment.Size > UINT64_MAX - (Page - 1) || Info.Flags ||
        (Info.InitialProtection & ~7u) || (Info.MaximumProtection & ~7u) ||
        (Info.InitialProtection & ~Info.MaximumProtection))
      return failure(
          "unsupported Darwin segment alignment, flags or permissions");
    if (Segment.Name == "__PAGEZERO") {
      if (++PageZeroCount > 1 || Segment.VA || Segment.FileSz ||
          !Segment.Size || Segment.Size % Page || Info.InitialProtection ||
          Info.MaximumProtection)
        return failure("invalid Mach-O __PAGEZERO reservation");
      Result.MinimumAddress = std::max(MinimumAddress, Segment.Size);
      continue;
    }
    if (!Segment.Size)
      continue;
    if (!Segment.FileOff && Segment.FileSz &&
        (Info.InitialProtection & (VM_PROT_READ | VM_PROT_EXECUTE)) !=
            (VM_PROT_READ | VM_PROT_EXECUTE))
      return failure(
          "Darwin Mach header segment requires read and execute permissions");
    if (Info.InitialProtection && !(Info.InitialProtection & VM_PROT_READ))
      return failure("Darwin profile requires readable nonempty permissions");
    Result.Maximum.push_back({Segment.VA,
                              (Segment.Size + Page - 1) & ~(Page - 1),
                              Info.MaximumProtection});
    Mapped.push_back(Segment);
  }
  Image.Segments = std::move(Mapped);
  auto Plan = ImageMappingPlan::create(
      Image, 0, Page, Options.MemoryLimit - Options.StackSize - Page, true,
      ImagePagePadding::FilePagesPreserveTail, ImageByteSource::OriginalFile);
  if (!Plan)
    return Plan.takeError();
  for (const auto &Region : Plan->Regions) {
    const uint64_t End = Region.Address + Region.Bytes.size();
    if (Region.Address < Result.MinimumAddress || End > UserLimit ||
        (Region.Address < StackTop + Page &&
         End > StackTop - Options.StackSize - Page) ||
        (Region.Address < ReturnGate + Page && End > ReturnGate))
      return failure("Darwin image overlaps a reserved or non-user address");
  }
  Result.Plan = std::move(*Plan);
  return Result;
}
} // namespace neverd::emulation::darwin_model
