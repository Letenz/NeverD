//===- Registry.cpp - Format and protector modules ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "Format.h"

namespace neverd::unpack {
#define NEVERD_UNPACK_FORMAT_MODULE(Kind, Namespace)                           \
  namespace Namespace {                                                        \
  const Format &format();                                                      \
  }
#include "UnpackValues.def"
#undef NEVERD_UNPACK_FORMAT_MODULE

llvm::ArrayRef<const Format *> formats() {
  static const Format *const Modules[] = {
#define NEVERD_UNPACK_FORMAT_MODULE(Kind, Namespace) &Namespace::format(),
#include "UnpackValues.def"
#undef NEVERD_UNPACK_FORMAT_MODULE
  };
  return Modules;
}

llvm::Expected<const Format *> formatOf(llvm::ArrayRef<uint8_t> File) {
  for (const Format *F : formats())
    if (F->recognizes(File))
      return F;
  std::string Supported;
  for (const Format *F : formats()) {
    if (!Supported.empty())
      Supported += text::FormatSeparator;
    Supported += formatKindName(F->kind());
  }
  return failure(text::UnknownFormat + Supported);
}

llvm::Expected<std::unique_ptr<InputImage>>
readImage(llvm::ArrayRef<uint8_t> File, const Format *&Selected) {
  auto F = formatOf(File);
  if (!F)
    return F.takeError();
  Selected = *F;
  return Selected->read(File);
}
} // namespace neverd::unpack
