//===- ControlFlowEntries.cpp - PE address-taken function identity --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/ControlFlowEntries.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"

namespace neverd {

llvm::Expected<std::vector<va_t>>
readPEControlFlowEntries(const BinaryImage &Image) {
  std::vector<va_t> Entries;
  if (!Image.isCOFF() || Image.IsRelocatable)
    return Entries;
  const auto &Config = Image.DynInfo;
  const uint32_t Flags = Config.GuardFlags;
  const bool Present =
      Flags & uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT);
  const auto Invalid = [](const char *Message) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
  };
  if (!Present) {
    if (Config.GuardCFFunctionTableRVA || Config.GuardCFFunctionCount)
      return Invalid("PE CFG function table is missing its presence flag");
    return Entries;
  }
  const uint32_t Extra = (Flags >> 28) & 15;
  if (Extra > 1)
    return Invalid("unsupported PE CFG function table metadata stride");
  const uint64_t Count = Config.GuardCFFunctionCount;
  const uint64_t RVA = Config.GuardCFFunctionTableRVA;
  if (!Count && !RVA)
    return Entries;
  const uint64_t Stride = 4 + Extra;
  // Use the common immutable-read limit and reject before multiplication.
  if (!RVA || !Count || Count > (1024 * 1024) / Stride || RVA > UINT32_MAX ||
      Count * Stride > UINT32_MAX - RVA || Image.Base > InvalidVA - RVA)
    return Invalid(
        "PE CFG function table has an invalid or unsupported extent");
  const va_t Table = Image.Base + RVA;
  const auto Size = static_cast<uint32_t>(Count * Stride);
  // Linked images may merge metadata into RX storage. The code-byte reader
  // proves immutability and fixups without interpreting the bytes; the complete
  // GFIDS structure below supplies their metadata role.
  auto Bytes = Image.isCodeAddress(Table)
                   ? readImmutableCodeBytes(Image, Table, Size)
                   : readImmutableImageBytes(Image, Table, Size);
  if (!Bytes)
    return Invalid("PE CFG function table lacks unique immutable file storage");
  uint32_t Previous = 0;
  va_t PreviousTarget = 0;
  for (uint64_t I = 0; I < Count; ++I) {
    const auto *Record = Bytes->data() + I * Stride;
    const uint32_t TargetRVA = readLE<uint32_t>(Record);
    const uint8_t EntryFlags = Extra ? Record[4] : 0;
    if (!TargetRVA || (I && TargetRVA <= Previous) ||
        Image.Base > InvalidVA - TargetRVA)
      return Invalid("PE CFG function entries are not unique increasing RVAs");
    Previous = TargetRVA;
    // Admit ordinary records and suppression flags. Handler/XFG metadata
    // needs a separate contract; it must not silently become callable proof.
    if (EntryFlags & ~uint8_t(3))
      return Invalid("unsupported PE CFG function entry flags");
    const va_t Target =
        normalizeCodeAddress(Image.Base + TargetRVA, Image.Arch, Image.Mode);
    const unsigned Alignment =
        Image.Arch == Arch::AArch64 ? 4
        : Image.Arch == Arch::ARM
            ? (Image.instructionModeAt(Target) == InstructionMode::Thumb ? 2
                                                                         : 4)
            : 1;
    if (Target % Alignment || (I && Target <= PreviousTarget))
      return Invalid(
          "PE CFG function entries have invalid instruction alignment");
    PreviousTarget = Target;
    if (!Image.isFunctionEntryCandidateAt(Target))
      return Invalid("PE CFG function entry has no independent code identity");
    if (!isFileBackedCodeImageRange(Image, Target, Alignment))
      return Invalid("PE CFG function entry lacks executable file storage");
    if (!EntryFlags)
      Entries.push_back(Target);
  }
  return Entries;
}

} // namespace neverd
