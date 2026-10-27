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
namespace {

std::optional<ExceptionAddressRange> checkedRestore(const BinaryImage &Image,
                                                    va_t Target) {
  static constexpr uint8_t Bytes[] = {0x8b, 0x4d, 0xf4, 0x64, 0x89, 0x0d, 0,
                                      0,    0,    0,    0x59, 0x5f, 0x5f, 0x5e,
                                      0x5b, 0x8b, 0xe5, 0x5d, 0x51, 0xc3};
  if (Target > UINT32_MAX - (sizeof(Bytes) - 1))
    return std::nullopt;
  const auto Code =
      readImmutablePE32CodeBytes(Image, Target, sizeof(Bytes), {});
  if (!Code || !std::equal(std::begin(Bytes), std::end(Bytes), Code->begin()))
    return std::nullopt;
  return ExceptionAddressRange{Target, Target + sizeof(Bytes)};
}

} // namespace

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
  if (!Target)
    return std::nullopt;
  if (const auto Restore = checkedRestore(Image, *Target)) {
    if (Function.CodeRange.overlaps(*Restore) ||
        !getCheckedX86CxxHandlerABI(Image, Function))
      return std::nullopt;
    return X86CxxEpilogueCall{{CallVA, CallVA + 5}, *Restore, *Restore, {}};
  }
  if (*Target > UINT32_MAX - 14)
    return std::nullopt;
  const ExceptionAddressRange Wrapper{*Target, *Target + 15};
  static constexpr uint8_t Prefix[] = {0x8b, 0x4d, 0xf0, 0x33, 0xcd, 0xe8};
  const auto Code = readImmutablePE32CodeBytes(Image, *Target, 15, {});
  if (!Code || Function.CodeRange.overlaps(Wrapper) ||
      !std::equal(std::begin(Prefix), std::end(Prefix), Code->begin()) ||
      (*Code)[10] != 0xe9)
    return std::nullopt;
  const auto CheckVA = registration_detail::addSignedOffset(
      *Target + 10, readLE<int32_t>(Code->data() + 6));
  const auto RestoreVA = registration_detail::addSignedOffset(
      Wrapper.End, readLE<int32_t>(Code->data() + 11));
  const auto Check = CheckVA
                         ? getCheckedX86CookieCheckSuccessPath(Image, *CheckVA)
                         : std::nullopt;
  const auto Restore =
      RestoreVA ? checkedRestore(Image, *RestoreVA) : std::nullopt;
  if (!Check || !Restore || Restore->overlaps(Wrapper) ||
      Function.CodeRange.overlaps(*Restore) ||
      Function.CodeRange.overlaps(Check->CodeRange) ||
      Wrapper.overlaps(Check->CodeRange) ||
      Restore->overlaps(Check->CodeRange) ||
      !getCheckedX86CxxHandlerABI(Image, Function))
    return std::nullopt;
  return X86CxxEpilogueCall{{CallVA, CallVA + 5},
                            Wrapper,
                            *Restore,
                            X86CxxEpilogueCookieCheck{Check->CodeRange,
                                                      Check->SecurityCookieVA,
                                                      Check->FailureVA}};
}

} // namespace neverd::coff_loader
