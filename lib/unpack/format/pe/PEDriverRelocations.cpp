//===- PEDriverRelocations.cpp - Authenticated snapshot relocations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Encode retained loader fields without assuming generated pointer origins.
//===----------------------------------------------------------------------===//
#include "PEDriverRelocations.h"

#include "neverd/emulation/WindowsDriverState.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>

namespace neverd::unpack::pe {
llvm::Expected<DriverRelocationMetadata> retainDriverRelocations(
    const Image &Input, const Capture &Observed, llvm::ArrayRef<uint8_t> Memory,
    llvm::ArrayRef<std::pair<uint64_t, uint64_t>> ReplacedRanges,
    uint64_t MetadataRVA, std::vector<uint8_t> &Metadata) {
  DriverRelocationMetadata Out;
  if (Input.domain() != ExecutionDomain::Kernel || !Observed.OwnedState)
    return Out;
  if (Observed.OwnedState->Profile !=
      emulation::ProcessRuntimeState::Kind::WindowsDriverX64)
    return failure("kernel snapshot has incompatible relocation ownership");
  const auto &Driver =
      static_cast<const emulation::WindowsDriverState &>(*Observed.OwnedState);
  if (!Driver.Relocations)
    return Out;
  const auto &State = *Driver.Relocations;
  if (State.ImageBase != Observed.Base ||
      State.PreferredBase != Input.preferredBase() ||
      State.FileHash != llvm::SHA256::hash(Input.file()))
    return failure("kernel relocation facts do not match the captured input");
  if (Memory.size() != Input.extent())
    return failure("kernel relocation memory extent changed");
  uint64_t PreviousEnd = 0;
  for (const auto &Field : State.Fields) {
    const auto *Region = Input.regionAt(Field.RVA);
    if (!Region || Field.RVA < PreviousEnd || Field.RVA > UINT32_MAX - 7 ||
        Field.RVA > Memory.size() || Memory.size() - Field.RVA < 8 ||
        Region->MemorySize - (Field.RVA - Region->RVA) < 8)
      return failure("invalid kernel relocation field extent or order");
    PreviousEnd = Field.RVA + 8;
    if (!Field.LoaderWritePreserved)
      continue;
    const bool Replaced = std::any_of(
        ReplacedRanges.begin(), ReplacedRanges.end(), [&](const auto &Range) {
          return Range.first < PreviousEnd && Field.RVA < Range.second;
        });
    if (Replaced || llvm::support::endian::read64le(
                        Memory.data() + Field.RVA) != Field.LoadedValue)
      continue;
    Out.Fields.push_back(Field.RVA);
  }
  if (Out.Fields.empty())
    return Out;
  const uint64_t Start = llvm::alignTo(Metadata.size(), uint64_t(4));
  std::vector<uint8_t> Records;
  for (size_t First = 0; First < Out.Fields.size();) {
    const uint32_t Page = Out.Fields[First] & ~uint32_t(4095);
    size_t End = First + 1;
    while (End < Out.Fields.size() &&
           (Out.Fields[End] & ~uint32_t(4095)) == Page)
      ++End;
    const uint64_t Size = llvm::alignTo(8 + 2 * (End - First), uint64_t(4));
    const uint64_t At = Records.size();
    Records.resize(At + Size, 0); // ABSOLUTE padding has no relocation effect.
    llvm::support::endian::write32le(Records.data() + At, Page);
    llvm::support::endian::write32le(Records.data() + At + 4, Size);
    for (size_t I = First; I < End; ++I)
      llvm::support::endian::write16le(
          Records.data() + At + 8 + 2 * (I - First),
          (llvm::COFF::IMAGE_REL_BASED_DIR64 << 12) | (Out.Fields[I] - Page));
    First = End;
  }
  if (MetadataRVA > UINT32_MAX || Start > UINT32_MAX - MetadataRVA ||
      Records.size() > UINT32_MAX - MetadataRVA - Start)
    return failure("kernel relocation metadata exceeds the PE RVA contract");
  Metadata.resize(Start, 0);
  Metadata.insert(Metadata.end(), Records.begin(), Records.end());
  Out.Directory.RelativeVirtualAddress = MetadataRVA + Start;
  Out.Directory.Size = Records.size();
  return Out;
}
} // namespace neverd::unpack::pe
