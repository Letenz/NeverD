//===- COFFRegistrationEHPersonality.cpp - Checked PE32 C++ dispatcher ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/X86CxxRuntimeABI.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/ArrayRef.h"

#include <set>

namespace neverd::coff_loader {
namespace {

bool decodeCookieGuards(const BinaryImage &Img, va_t PrefixVA,
                        llvm::ArrayRef<uint8_t> Bytes,
                        X86CxxHandlerABI &Result) {
  // EDX is the original registration node throughout both checks. The checked
  // leaf's success path preserves EAX and EDX; its failure path is not elided.
  if (Bytes.size() < 4 || Bytes[0] != 0x8b || Bytes[1] != 0x54 ||
      Bytes[2] != 0x24 || Bytes[3] != 8)
    return false;
  size_t At = 4;
  auto NodeDisplacement = [&](uint8_t Opcode,
                              uint8_t ShortModRM) -> std::optional<int32_t> {
    if (Bytes.size() - At < 3 || Bytes[At] != Opcode)
      return std::nullopt;
    if (Bytes[At + 1] == ShortModRM) {
      const int32_t Value = int8_t(Bytes[At + 2]);
      At += 3;
      return Value;
    }
    if (Bytes[At + 1] != ShortModRM + 0x40 || Bytes.size() - At < 6)
      return std::nullopt;
    const int32_t Value = readLE<int32_t>(Bytes.data() + At + 2);
    At += 6;
    return Value;
  };
  auto XOROffset = NodeDisplacement(0x8d, 0x42); // lea eax,[edx+disp]
  if (!XOROffset)
    return false;
  for (unsigned I = 0; I != 2; ++I) {
    const auto CookieOffset = NodeDisplacement(0x8b, 0x4a);
    if (!CookieOffset || Bytes.size() - At < 7 || Bytes[At] != 0x33 ||
        Bytes[At + 1] != 0xc8 || Bytes[At + 2] != 0xe8)
      return false;
    At += 2; // xor ecx,eax
    const va_t CallVA = PrefixVA + At;
    const va_t CheckVA =
        uint32_t(CallVA + 5 + readLE<uint32_t>(Bytes.data() + At + 1));
    const auto Check = getCheckedX86CookieCheckSuccessPath(Img, CheckVA);
    if (!Check)
      return false;
    if (Result.SecurityCookieVA &&
        Result.SecurityCookieVA != Check->SecurityCookieVA)
      return false;
    Result.SecurityCookieVA = Check->SecurityCookieVA;
    Result.Dispatch.CodeRanges.push_back(Check->CodeRange);
    Result.CookieGuards.push_back(
        {*CookieOffset, *XOROffset, CallVA, CheckVA, Check->FailureVA});
    At += 5;
    if (At == Bytes.size())
      return true;
    // Some two-cookie handlers rebase EAX from the first XOR coordinate to
    // the second. Others reuse it. Neither form changes the node in EDX.
    if (I == 0 && Bytes.size() - At >= 3 && Bytes[At] == 0x83 &&
        Bytes[At + 1] == 0xc0 && Bytes[At + 2] == 12) {
      if (*XOROffset > INT32_MAX - 12)
        return false;
      *XOROffset += 12;
      At += 3;
    }
  }
  return false;
}

} // namespace

bool isCheckedX86CxxHandlerReference(const BinaryImage &Img, va_t HandlerVA) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF || !HandlerVA || HandlerVA > UINT32_MAX)
    return false;
  registration_detail::HandlerIdentity Identity;
  if (!registration_detail::decodeCxxHandlerThunk(Img, HandlerVA, Identity))
    return false;
  auto Records = getCheckedX86CxxFuncInfoRecords(Img, Identity.CxxFuncInfoVA);
  if (!Records || !Records->HasDistinctRanges)
    return false;
  ExceptionFunction Function;
  Function.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  Function.Personality = Identity.Personality;
  Function.PersonalityVA = HandlerVA;
  Function.HandlerDataVA = Identity.CxxFuncInfoVA;
  Function.Cxx = std::move(Records->Cxx);
  Function.Registration.emplace();
  Function.Registration->HandlerVA = HandlerVA;
  Function.Registration->ScopeTableVA = Identity.CxxFuncInfoVA;
  return getCheckedX86CxxPersonalityABI(Img, Function).has_value();
}

std::optional<X86CxxPersonalityABI>
getCheckedX86CxxPersonalityABI(const BinaryImage &Img,
                               const ExceptionFunction &Function) {
  auto Handler = getCheckedX86CxxHandlerABI(Img, Function);
  // Existing native consumers require a dispatcher with no unproved guard
  // preconditions. Prefix identity alone cannot authorize skipping a check.
  if (!Handler || !Handler->CookieGuards.empty())
    return std::nullopt;
  return std::move(Handler->Dispatch);
}

std::optional<X86CxxHandlerABI>
getCheckedX86CxxHandlerABI(const BinaryImage &Img,
                           const ExceptionFunction &Function) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF || !Function.Registration ||
      !Function.Cxx ||
      Function.Personality != ExceptionPersonality::CxxFrameHandler3 ||
      Function.Registration->HandlerVA != Function.PersonalityVA ||
      !getCheckedX86CxxMetadataRanges(Img, Function))
    return std::nullopt;
  registration_detail::HandlerIdentity Identity;
  if (!registration_detail::decodeCxxHandlerThunk(Img, Function.PersonalityVA,
                                                  Identity) ||
      Identity.CxxFuncInfoVA != Function.Cxx->NativeFuncInfoVA ||
      Identity.CxxThunkBodyVA < Function.PersonalityVA ||
      Identity.CxxThunkBodyVA - Function.PersonalityVA >= 64 ||
      Identity.CxxThunkEndVA <= Identity.CxxThunkBodyVA)
    return std::nullopt;
  X86CxxHandlerABI Handler;
  auto &Result = Handler.Dispatch;
  auto ReadCode = [&](va_t VA, uint32_t Size,
                      const std::vector<va_t> &Operands = {})
      -> std::optional<std::vector<uint8_t>> {
    if (!VA || VA > UINT32_MAX || Size > uint64_t(UINT32_MAX) + 1 - VA ||
        !Img.hasExecutableCodeOwnerRange(VA, Size))
      return std::nullopt;
    auto Bytes = readImmutablePE32CodeBytes(Img, VA, Size, Operands);
    if (!Bytes)
      return std::nullopt;
    Result.CodeRanges.push_back({VA, VA + Size});
    return Bytes;
  };
  const uint32_t Prefix = Identity.CxxThunkBodyVA - Function.PersonalityVA;
  std::vector<va_t> Operands{Identity.CxxThunkBodyVA + 1};
  if (Identity.CxxDispatchIATVA)
    Operands.push_back(Identity.CxxThunkBodyVA + 7);
  const auto Thunk =
      ReadCode(Function.PersonalityVA,
               Identity.CxxThunkEndVA - Function.PersonalityVA, Operands);
  if (!Thunk)
    return std::nullopt;
  size_t Padding = 0;
  while (
      Padding < Prefix &&
      Padding <
          ::neverd::limits::loader::
              kCOFFRegistrationEHPersonalityGetCheckedX86CxxPersonalityABIPaddingLimit &&
      (*Thunk)[Padding] == 0x90)
    ++Padding;
  if (Padding != Prefix) {
    // Clang -O0 reads the four handler arguments into EAX before loading
    // FuncInfo. These fixed stack reads preserve ESP, flags and every live
    // register; the immediately following MOV replaces the scratch value.
    bool StackReads = Prefix - Padding == 16;
    if (StackReads)
      for (unsigned Arg = 0; Arg != 4; ++Arg) {
        const auto At = Padding + Arg * 4;
        StackReads &= (*Thunk)[At] == 0x8b && (*Thunk)[At + 1] == 0x44 &&
                      (*Thunk)[At + 2] == 0x24 &&
                      (*Thunk)[At + 3] == 16 - Arg * 4;
      }
    if (!StackReads &&
        !decodeCookieGuards(
            Img, Function.PersonalityVA + Padding,
            llvm::ArrayRef<uint8_t>(*Thunk).slice(Padding, Prefix - Padding),
            Handler)) {
      return std::nullopt;
    }
  }
  Result.RuntimeVA = Identity.CxxDispatchIATVA ? Identity.CxxThunkBodyVA + 5
                                               : Identity.CxxDispatchVA;
  va_t Cursor = Result.RuntimeVA;
  std::set<va_t> Seen;
  for (unsigned Hop = 0;
       Hop != ::neverd::limits::loader::kMaxRegistrationPersonalityHops;
       ++Hop) {
    if (!Seen.insert(Cursor).second)
      return std::nullopt;
    auto Code = ReadCode(Cursor, 1);
    if (!Code)
      return std::nullopt;
    if ((*Code)[0] == 0xe9) {
      Code = ReadCode(Cursor, 5);
      if (!Code)
        return std::nullopt;
      Cursor =
          uint32_t(Cursor + 5 + uint32_t(readLE<int32_t>(Code->data() + 1)));
      continue;
    }
    Code = ReadCode(Cursor, 6, {Cursor + 2});
    if (!Code || (*Code)[0] != 0xff || (*Code)[1] != 0x25)
      return std::nullopt;
    Result.IATVA = readLE<uint32_t>(Code->data() + 2);
    const Import *Import = nullptr;
    for (const auto &Candidate : Img.Imports)
      if (Candidate.IATAddr == Result.IATVA) {
        if (Import)
          return std::nullopt;
        Import = &Candidate;
      }
    if (!Import || Import->Name != "__CxxFrameHandler3" ||
        !hasKnownX86CxxExceptionRuntimeABI(Import->Module) ||
        Result.IATVA > UINT32_MAX - 3 || !Img.readVA(Result.IATVA, 4))
      return std::nullopt;
    return Handler;
  }
  return std::nullopt;
}

} // namespace neverd::coff_loader
