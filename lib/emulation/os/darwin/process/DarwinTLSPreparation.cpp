//===- DarwinTLSPreparation.cpp - Original TLV descriptors and templates --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Validate TLS layout without repacking the original contiguous span.
#include "DarwinTLS.h"

#include "neverd/emulation/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
llvm::Error invalid() {
  return failure("unsupported or malformed Darwin TLS descriptor/template");
}
llvm::Expected<uint64_t> originalWord(const BinaryImage &Image, uint64_t VA) {
  for (const auto &S : Image.Segments)
    if (VA >= S.VA && VA - S.VA <= S.FileSz && 8 <= S.FileSz - (VA - S.VA))
      return llvm::support::endian::read64le(Image.Raw.data() + S.FileOff +
                                             (VA - S.VA));
  return invalid();
}
} // namespace
llvm::Error prepareTLSImage(ProcessImage &Out,
                            const MachOExecutionImage &Loaded,
                            const MachOProgramLinking &Facts, uint64_t Bias,
                            uint64_t &NextAddress, uint64_t Available,
                            uint64_t &Records,
                            llvm::function_ref<llvm::Error()> Check) {
  using namespace llvm::MachO;
  if (!Loaded.HasTLS)
    return (Loaded.Flags & MH_HAS_TLV_DESCRIPTORS) ? invalid()
                                                   : llvm::Error::success();
  auto Sections = Loaded.TLSSections;
  llvm::sort(Sections, [](const auto &A, const auto &B) {
    return A.Address < B.Address;
  });
  uint64_t Start = UINT64_MAX, End = 0, Descriptors = 0, PreviousEnd = 0;
  auto Charge = [&](uint64_t Bytes) -> llvm::Error {
    if (auto E = Check())
      return E;
    if (Bytes > Available - Out.MetadataBytes)
      return failure(diagnostic::ProcessLimits);
    Out.MetadataBytes += Bytes;
    return llvm::Error::success();
  };
  if (auto E = Charge(2 * Sections.size() * sizeof(MachOExecutionTLSSection)))
    return E;
  for (const auto &S : Sections) {
    if (auto E = Check())
      return E;
    if (S.Alignment > 4 || S.Address % (uint64_t(1) << S.Alignment) ||
        S.Address < PreviousEnd)
      return invalid();
    PreviousEnd = S.Address + S.Size;
    const bool Storage =
        llvm::any_of(Loaded.Image.Segments, [&](const auto &M) {
          return M.isReadable() && !M.isExecutable() && S.Address >= M.VA &&
                 S.Size <= M.Size && S.Address - M.VA <= M.Size - S.Size;
        });
    if (!Storage)
      return invalid();
    switch (S.Type) {
    case S_THREAD_LOCAL_REGULAR:
    case S_THREAD_LOCAL_ZEROFILL:
      Start = std::min(Start, S.Address);
      End = std::max(End, S.Address + S.Size);
      break;
    case S_THREAD_LOCAL_VARIABLES:
      if (S.Address % 8 || S.Size % 24)
        return invalid();
      Descriptors += S.Size / 24;
      break;
    case S_THREAD_LOCAL_VARIABLE_POINTERS:
      if (S.Address % 8 || S.Size % 8)
        return invalid();
      break;
    default:
      return invalid();
    }
  }
  if (bool(Descriptors) != bool(Loaded.Flags & MH_HAS_TLV_DESCRIPTORS))
    return invalid();
  if (!Descriptors)
    return Start == UINT64_MAX ? llvm::Error::success() : invalid();
  if (Start == UINT64_MAX || End <= Start ||
      Descriptors >
          ::neverd::limits::emulation::kMaxDarwinStartupRecords - Records)
    return invalid();
  Records += Descriptors;
  for (const auto &S : Sections) {
    if (S.Type == S_THREAD_LOCAL_REGULAR || S.Type == S_THREAD_LOCAL_ZEROFILL) {
      if ((S.Address - Start) % (uint64_t(1) << S.Alignment))
        return invalid();
    } else if (S.Address < End && S.Address + S.Size > Start)
      return invalid();
  }
  const uint64_t Page = Out.Memory.PageSize, Size = End - Start;
  // Every byte in the contiguous native template must have one readable,
  // non-executable image owner, including padding between TLS sections.
  uint64_t Cursor = Start + Bias;
  while (Cursor < End + Bias) {
    if (auto E = Check())
      return E;
    const auto Region = llvm::find_if(Out.Plan.Regions, [&](const auto &R) {
      return (R.Permissions & Read) && !(R.Permissions & Execute) &&
             Cursor >= R.Address && Cursor - R.Address < R.Bytes.size();
    });
    if (Region == Out.Plan.Regions.end())
      return invalid();
    Cursor = Region->Address + Region->Bytes.size();
  }
  if (Size > UINT64_MAX - (Page - 1))
    return invalid();
  const uint64_t Mapped = (Size + Page - 1) & ~(Page - 1);
  if (auto E = Charge(
          Descriptors * (sizeof(TLSDescriptor) + sizeof(PreparedFixup) + 128) +
          sizeof(TLSImage) + 128))
    return E;
  if (Out.Plan.MappedBytes > Available - Out.MetadataBytes ||
      Mapped > Available - Out.MetadataBytes - Out.Plan.MappedBytes ||
      NextAddress >= LibSystemGate || Mapped > LibSystemGate - NextAddress)
    return failure(diagnostic::ProcessLimits);
  const auto Key = Out.TLS.size();
  std::map<uint64_t, const MachOProgramFixup *> Fixups;
  for (const auto &F : Facts.Fixups)
    Fixups.emplace(F.Address, &F);
  for (const auto &S : Sections) {
    if (S.Type != S_THREAD_LOCAL_VARIABLES)
      continue;
    for (uint64_t Offset = 0; Offset < S.Size; Offset += 24) {
      if (auto E = Check())
        return E;
      const uint64_t D = S.Address + Offset;
      auto KeyWord = originalWord(Loaded.Image, D + 8);
      if (!KeyWord)
        return KeyWord.takeError();
      auto Relative = originalWord(Loaded.Image, D + 16);
      if (!Relative)
        return Relative.takeError();
      const bool TemplateOffset =
          *Relative < Size &&
          llvm::any_of(
              Sections,
              [&](const auto &T) {
                return (T.Type == S_THREAD_LOCAL_REGULAR ||
                        T.Type == S_THREAD_LOCAL_ZEROFILL) &&
                       Start + *Relative >= T.Address &&
                       Start + *Relative - T.Address < T.Size;
              });
      auto Thunk = Fixups.find(D);
      if (*KeyWord || !TemplateOffset || Thunk == Fixups.end() ||
          Fixups.contains(D + 8) || Fixups.contains(D + 16))
        return invalid();
      const auto &F = *Thunk->second;
      if (!F.Bind || F.Symbol != "__tlv_bootstrap" || F.WeakImport ||
          F.Addend || F.LibraryOrdinal <= 0 ||
          uint64_t(F.LibraryOrdinal) > Facts.Dependencies.size() ||
          Facts.Dependencies[F.LibraryOrdinal - 1].Name != LibSystemPath)
        return invalid();
      Out.TLSDescriptors.emplace(D + Bias, TLSDescriptor{Key, *Relative});
      Out.Fixups.push_back({D + Bias + 8, Key + 1});
    }
  }
  Out.TLS.push_back({Start + Bias, Size, NextAddress});
  Out.Memory.Reserved.push_back({NextAddress, Mapped, 3});
  Out.Memory.Maximum.push_back({NextAddress, Mapped, 3});
  Out.Plan.Regions.push_back({NextAddress, Read | Write | UserAccessible,
                              std::vector<uint8_t>(Mapped, 0)});
  Out.Plan.MappedBytes += Mapped;
  NextAddress += Mapped + Page;
  return llvm::Error::success();
}
} // namespace neverd::emulation::darwin_model
