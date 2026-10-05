//===- Unpack.cpp - Recover the image a packed executable builds ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "Format.h"
#include "Packer.h"
#ifdef NEVERD_UNPACK_EXECUTION
#include "../dynamic/ProcessTransfer.h"
#endif

#include <fstream>

namespace neverd::unpack {
UnpackOptions::UnpackOptions() {
#define NEVERD_UNPACK_PROCESS_DEFAULT(Field, Member, Default)                  \
  Process.Member = defaults::Default;
#define NEVERD_UNPACK_PROFILE_DEFAULT(Group, Member, JSONGroup, JSONField,     \
                                      Value)                                   \
  if (!Process.Group)                                                          \
    Process.Group.emplace();                                                   \
  Process.Group->Member = Value;
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_PROFILE_DEFAULT
#undef NEVERD_UNPACK_PROCESS_DEFAULT
}

llvm::Expected<std::vector<uint8_t>>
readInput(const std::filesystem::path &Path) {
  std::error_code Error;
  const auto Size = std::filesystem::file_size(Path, Error);
  if (Error)
    return failure(text::ReadFailed + Error.message());
  if (Size > defaults::InputBytes)
    return failure(text::InputTooLarge);
  std::vector<uint8_t> Bytes(Size);
  std::ifstream Stream(Path, std::ios::binary);
  if (!Stream.read(reinterpret_cast<char *>(Bytes.data()),
                   std::streamsize(Size)) ||
      Stream.gcount() != std::streamsize(Size))
    return failure(text::ReadFailed + Path.filename().string());
  return Bytes;
}

// The orchestration names no container, instruction set, guest system or
// protector. Each is chosen through its registry from facts of the input.
llvm::Expected<UnpackResult> unpackFile(const std::filesystem::path &Input,
                                        const UnpackOptions &Options) {
  if (Options.Transfer > defaults::MaxTransfers)
    return failure(text::Limits);
  auto File = readInput(Input);
  if (!File)
    return File.takeError();
  const Format *Container = nullptr;
  auto Parsed = readImage(*File, Container);
  if (!Parsed)
    return Parsed.takeError();
  const InputImage &Image = **Parsed;
#ifdef NEVERD_UNPACK_EXECUTION
  UnpackResult Result;
  Result.Format = Container->kind();
  Result.Packer = identify(Image);
  Result.Architecture = architectureName(Image.architecture());
  Result.ImageBase = Image.preferredBase();
  Result.PackedEntryRVA = Image.entryRVA();
  // Only an identified stub may declare an entry; an unidentified input has
  // no format whose operands could be read as one.
  const Packer *Stub = packerOf(Result.Packer.Kind);
  const std::optional<uint64_t> Declared =
      Stub ? Stub->declaredEntry(Image) : std::nullopt;
  auto Traits = architectureTraits(Image.architecture());
  if (!Traits)
    return Traits.takeError();
  auto Profile = processProfile(Image);
  if (!Profile)
    return Profile.takeError();
  TransferObserver Observer(Image, *Traits, Options.Transfer, Declared);
  auto Run =
      emulation::observeProcess(Input, *Profile, Options.Process, Observer);
  if (!Run)
    return Run.takeError();
  Result.Profile = emulation::processProfileName(Run->Profile);
  Result.Transfers = Observer.transfers();
  Result.Backend = emulation::executionBackendName(Run->SelectedBackend);
  Result.BackendSelectionReason = Run->BackendSelectionReason;
  Result.ProcessStop = emulation::processStopReasonName(Run->Stop);
  Result.ProcessDiagnostic = Run->Diagnostic;
  Result.PC = Run->PC;
  Result.Instructions = Run->Instructions;
  Result.Events = Run->Events;
  auto Observed = Observer.take();
  if (!Observed) {
    Result.Diagnostic = text::NoEntry + Result.ProcessStop;
    return Result;
  }
  RebuildPlan Plan;
  if (Stub)
    Stub->planRebuild(Image, *Observed, Plan);
  auto Rebuilt = Container->rebuild(Image, *Observed, Plan);
  if (!Rebuilt)
    return Rebuilt.takeError();
  Result.Outcome = UnpackOutcome::Unpacked;
  Result.ImageBase = Observed->Base;
  Result.EntryRVA = Observed->EntryRVA;
  Result.Source = Observed->Source;
  // An observed entry outranks a declared one, but a stub that names another
  // address than the one it reached is not the stub that was identified.
  if (Declared && Observed->Source == EntrySource::Transfer &&
      !Options.Transfer && *Declared != Observed->EntryRVA)
    Result.Diagnostic = text::DeclaredEntry;
  Result.Sections = std::move(Rebuilt->Sections);
  Result.Imports = std::move(Rebuilt->Imports);
  Result.Image = std::move(Rebuilt->File);
  return Result;
#else
  (void)Image;
  return failure(text::Disabled);
#endif
}
} // namespace neverd::unpack
