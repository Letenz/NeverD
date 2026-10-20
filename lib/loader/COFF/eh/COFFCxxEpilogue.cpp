//===- COFFCxxEpilogue.cpp - Checked shared PE32 C++ exits ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFCxxEpilogue.h"

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include <algorithm>

namespace neverd::coff_loader {

std::optional<X86CxxEpilogueCall>
getCheckedX86CxxEpilogueCall(const BinaryImage &Image,
                             const ExceptionFunction &Function, va_t CallVA) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF ||
      Function.ParseStatus != ExceptionParseStatus::Complete ||
      Function.Encoding != ExceptionEncoding::X86CxxFuncInfo || !Function.Cxx ||
      !Function.Registration ||
      Function.Registration->RegistrationOffset != -12 ||
      Function.Registration->TryLevelOffset != -4 ||
      !Function.CodeRange.isValid() || CallVA < Function.CodeRange.Begin ||
      CallVA >= Function.CodeRange.End || Function.CodeRange.End - CallVA < 5 ||
      CallVA > UINT32_MAX - 4)
    return std::nullopt;
  const auto Caller = readImmutablePE32CodeBytes(Image, CallVA, 5, {});
  if (!Caller || (*Caller)[0] != 0xe8)
    return std::nullopt;
  const auto Target = registration_detail::addSignedOffset(
      CallVA + 5, readLE<int32_t>(Caller->data() + 1));
  static constexpr uint8_t Bytes[] = {0x8b, 0x4d, 0xf4, 0x64, 0x89, 0x0d, 0,
                                      0,    0,    0,    0x59, 0x5f, 0x5f, 0x5e,
                                      0x5b, 0x8b, 0xe5, 0x5d, 0x51, 0xc3};
  if (!Target || *Target > UINT32_MAX - (sizeof(Bytes) - 1))
    return std::nullopt;
  const ExceptionAddressRange Helper{*Target, *Target + sizeof(Bytes)};
  if (Function.CodeRange.overlaps(Helper))
    return std::nullopt;
  const auto Code =
      readImmutablePE32CodeBytes(Image, Helper.Begin, sizeof(Bytes), {});
  if (!Code || !std::equal(std::begin(Bytes), std::end(Bytes), Code->begin()) ||
      !getCheckedX86CxxHandlerABI(Image, Function))
    return std::nullopt;
  return X86CxxEpilogueCall{{CallVA, CallVA + 5}, Helper};
}

} // namespace neverd::coff_loader
