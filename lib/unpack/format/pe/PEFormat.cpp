//===- PEFormat.cpp - The PE32+ container format --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/Format.h"
#include "PEImage.h"

namespace neverd::unpack::pe {
namespace {
class PEFormat final : public Format {
public:
  FormatKind kind() const override { return FormatKind::PE64; }
  bool recognizes(llvm::ArrayRef<uint8_t> File) const override {
    const llvm::ArrayRef<uint8_t> Magic(value::DOSMagic);
    return File.take_front(Magic.size()) == Magic;
  }
  llvm::Expected<std::unique_ptr<InputImage>>
  read(llvm::ArrayRef<uint8_t> File) const override {
    auto Parsed = Image::read(File);
    if (!Parsed)
      return Parsed.takeError();
    return std::unique_ptr<InputImage>(std::move(*Parsed));
  }
  llvm::Expected<RebuiltImage> rebuild(const InputImage &Input,
                                       const Capture &Observed,
                                       const RebuildPlan &Plan) const override {
    return pe::rebuild(llvm::cast<Image>(Input), Observed, Plan);
  }
};
} // namespace

const Format &format() {
  static const PEFormat Module;
  return Module;
}
} // namespace neverd::unpack::pe
