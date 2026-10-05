//===- UPXPE.cpp - UPX stubs in PE images ---------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UPXInternal.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"

#include <cstring>

namespace neverd::unpack::upx {
namespace {
using emulation::GuestArchitecture;
using llvm::support::endian::read32le;

/// The register saves and the two address computations every x64 executable
/// stub begins with, at the image's own entry point.
bool hasX64EntryStub(const pe::Image &Image) {
  const auto Code = Image.fileBytesFrom(Image.entryRVA());
  const llvm::ArrayRef<uint8_t> Head(value::PEX64StubHead);
  const llvm::ArrayRef<uint8_t> Tail(value::PEX64StubTail);
  const uint64_t TailAt = Head.size() + value::StubDisplacementBytes;
  return Code.size() >= TailAt + Tail.size() &&
         Code.take_front(Head.size()) == Head &&
         Code.slice(TailAt, Tail.size()) == Tail;
}
} // namespace

void collectEvidence(const pe::Image &Image,
                     std::vector<PackerEvidence> &Evidence) {
  const auto Regions = Image.regions();
  if (Regions.size() >= 2 && Regions[0].Name == text::PEFirstSection &&
      Regions[1].Name == text::PESecondSection)
    Evidence.push_back(PackerEvidence::UPXSectionNames);
  const auto File = Image.file();
  const auto &H = Image.headers();
  const uint64_t End = std::min<uint64_t>(
      File.size(), uint64_t(H.SizeOfHeaders) + value::PEHeaderWindow);
  if (const auto Format = headerFormat(Image);
      Format && End > H.SectionTableOffset &&
      hasPackHeader(
          File.slice(H.SectionTableOffset, End - H.SectionTableOffset),
          *Format))
    Evidence.push_back(PackerEvidence::UPXPackHeader);
  if (Image.architecture() == GuestArchitecture::X64 && hasX64EntryStub(Image))
    Evidence.push_back(PackerEvidence::UPXEntryStub);
}

std::optional<uint64_t> stubEntry(const pe::Image &Image) {
  if (Image.architecture() != GuestArchitecture::X64)
    return std::nullopt;
  const auto Code = Image.fileBytesFrom(Image.entryRVA());
  const llvm::ArrayRef<uint8_t> Exit(value::PEX64StubExit);
  const uint64_t Length = Exit.size() + value::StubDisplacementBytes;
  std::optional<uint64_t> Found;
  for (uint64_t At = 0; At + Length <= Code.size(); ++At) {
    if (Code.slice(At, Exit.size()) != Exit)
      continue;
    // Two candidates name two entries; neither can be trusted.
    if (Found)
      return std::nullopt;
    Found = At;
  }
  if (!Found)
    return std::nullopt;
  const int64_t Displacement =
      int32_t(read32le(Code.data() + *Found + Exit.size()));
  const int64_t Target =
      int64_t(Image.entryRVA()) + int64_t(*Found + Length) + Displacement;
  if (Target < 0 || !Image.regionAt(uint64_t(Target)))
    return std::nullopt;
  return uint64_t(Target);
}

std::optional<uint64_t> programTLSDirectory(const pe::Image &Image,
                                            const Capture &Observed) {
  using Record = llvm::object::coff_tls_directory64;
  const llvm::ArrayRef<uint8_t> Memory(Observed.Memory);
  const uint64_t Packed =
      Image.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress;
  if (!Packed || Packed > Memory.size() ||
      sizeof(Record) > Memory.size() - Packed)
    return std::nullopt;
  auto Load = [&](uint64_t RVA) {
    Record R;
    std::memcpy(&R, Memory.data() + RVA, sizeof(R));
    return R;
  };
  const Record Own = Load(Packed);
  auto Inside = [&](uint64_t VA) {
    return VA >= Observed.Base && VA - Observed.Base < Memory.size();
  };
  std::optional<uint64_t> Found;
  for (const auto &R : Image.regions()) {
    const uint64_t End =
        std::min<uint64_t>(R.RVA + R.MemorySize, Memory.size());
    for (uint64_t RVA = R.RVA; RVA + sizeof(Record) <= End;
         RVA += pe::value::PointerSize) {
      if (RVA == Packed)
        continue;
      const Record Candidate = Load(RVA);
      if (Candidate.AddressOfIndex != Own.AddressOfIndex ||
          Candidate.EndAddressOfRawData - Candidate.StartAddressOfRawData !=
              Own.EndAddressOfRawData - Own.StartAddressOfRawData ||
          Candidate.SizeOfZeroFill != Own.SizeOfZeroFill ||
          Candidate.Characteristics != Own.Characteristics ||
          !Inside(Candidate.StartAddressOfRawData) ||
          (Candidate.AddressOfCallBacks &&
           !Inside(Candidate.AddressOfCallBacks)))
        continue;
      if (Found)
        return std::nullopt;
      Found = RVA;
    }
  }
  return Found;
}
} // namespace neverd::unpack::upx
