//===- UPX.cpp - The UPX protector module ---------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/Packer.h"
#include "UPXInternal.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"

#include <numeric>

namespace neverd::unpack::upx {
namespace {
using llvm::support::endian::read32le;

bool isMethod(uint8_t Method) {
  switch (Method) {
#define NEVERD_UNPACK_UPX_METHOD(Name, Value)                                  \
  case Value:                                                                  \
    return true;
#include "UPX.def"
#undef NEVERD_UNPACK_UPX_METHOD
  default:
    return false;
  }
}

class UPXPacker final : public Packer {
public:
  PackerKind kind() const override { return PackerKind::UPX; }
  void collectEvidence(const InputImage &Image,
                       std::vector<PackerEvidence> &Evidence) const override {
    if (const auto *PE = llvm::dyn_cast<pe::Image>(&Image))
      upx::collectEvidence(*PE, Evidence);
  }
  unsigned requiredEvidence() const override { return value::RequiredEvidence; }
  std::optional<uint64_t>
  declaredEntry(const InputImage &Image) const override {
    if (const auto *PE = llvm::dyn_cast<pe::Image>(&Image))
      return stubEntry(*PE);
    return std::nullopt;
  }
  void planRebuild(const InputImage &Image, const Capture &Observed,
                   RebuildPlan &Plan) const override {
    const auto *PE = llvm::dyn_cast<pe::Image>(&Image);
    if (!PE)
      return;
    if (const auto TLS = programTLSDirectory(*PE, Observed))
      Plan.Metadata.push_back({llvm::COFF::TLS_TABLE, *TLS,
                               sizeof(llvm::object::coff_tls_directory64)});
  }
};
} // namespace

std::optional<uint8_t> headerFormat(const InputImage &Image) {
#define NEVERD_UNPACK_UPX_TARGET(Format, Architecture, Byte)                   \
  if (Image.format() == FormatKind::Format &&                                  \
      Image.architecture() == emulation::GuestArchitecture::Architecture)      \
    return uint8_t(Byte);
#include "UPX.def"
#undef NEVERD_UNPACK_UPX_TARGET
  return std::nullopt;
}

bool hasPackHeader(llvm::ArrayRef<uint8_t> Bytes, uint8_t Format) {
  const llvm::ArrayRef<uint8_t> Magic(value::Magic);
  for (uint64_t At = 0; At + value::HeaderBytes <= Bytes.size(); ++At) {
    const auto Header = Bytes.slice(At, value::HeaderBytes);
    if (Header.take_front(Magic.size()) != Magic)
      continue;
    const auto Summed = Header.drop_front(Magic.size()).drop_back();
    const unsigned Sum = std::accumulate(Summed.begin(), Summed.end(), 0u) %
                         value::ChecksumModulus;
    const uint32_t Unpacked =
        read32le(Header.data() + value::UnpackedSizeOffset);
    const uint32_t Packed = read32le(Header.data() + value::PackedSizeOffset);
    if (Sum == Header.back() && Header[value::FormatOffset] == Format &&
        isMethod(Header[value::MethodOffset]) && Packed && Packed <= Unpacked &&
        read32le(Header.data() + value::FileSizeOffset))
      return true;
  }
  return false;
}

const Packer &packer() {
  static const UPXPacker Module;
  return Module;
}
} // namespace neverd::unpack::upx
