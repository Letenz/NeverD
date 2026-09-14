#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include <map>

namespace neverd {
namespace {
bool mergeSignature(SourceFunctionTypeHint &A,
                    const SourceFunctionTypeHint &B) {
  if (A.Parameters.size() != B.Parameters.size())
    return false;
  for (size_t I = 0; I < A.Parameters.size(); ++I)
    if (!equalSourceTypes(A.Parameters[I].Type, B.Parameters[I].Type))
      return false;
  if (equalSourceTypes(A.ReturnType, B.ReturnType))
    return true;
  // A full-width integer result carries the same 64 bits regardless of
  // signedness. Comparisons and conversions retain their own IR semantics.
  // Narrow results, floating-point and pointer identities cannot use this.
  if (!A.ReturnType || !B.ReturnType || A.ReturnType->Kind != NdTypeKind::Int ||
      B.ReturnType->Kind != NdTypeKind::Int || A.ReturnType->Size != 8 ||
      B.ReturnType->Size != 8)
    return false;
  A.ReturnType = NdType::makeInt(8, false);
  return true;
}

using DeclarationIndex =
    std::map<std::string, std::optional<SourceFunctionTypeHint>, std::less<>>;

DeclarationIndex foundationDeclarations(Arch Architecture) {
  static constexpr struct {
    const char *Selector;
    const char *AArch64;
    const char *X64;
  } Declarations[] = {
#include "ObjCFoundationDeclarations.inc"
  };
  DeclarationIndex Result;
  for (const auto &Declaration : Declarations) {
    const auto *Encoding =
        Architecture == Arch::AArch64 ? Declaration.AArch64 : Declaration.X64;
    if (!Encoding)
      continue;
    auto Hint = parseObjCMethodEncoding(Declaration.Selector, Encoding);
    std::string Diagnostic;
    if (Hint) {
      Hint->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
      if (!assignDarwinObjCSourceABI(*Hint, Architecture, Diagnostic))
        Hint.reset();
    }
    auto [It, Inserted] = Result.try_emplace(Declaration.Selector, Hint);
    // Negative evidence survives later valid declarations and their order.
    if (!Inserted && It->second &&
        (!Hint || !mergeSignature(*It->second, *Hint)))
      It->second.reset();
  }
  return Result;
}

const DeclarationIndex *frameworkDeclarations(const BinaryImage &Image) {
  bool Foundation = false;
  for (const auto &Name : Image.DynInfo.NeededLibs)
    Foundation |=
        Name == "/System/Library/Frameworks/Foundation.framework/Foundation" ||
        Name == "/System/Library/Frameworks/Foundation.framework/Versions/C/"
                "Foundation";
  if (!Foundation)
    return nullptr;
  // Compiler-derived facts are immutable and shared between sessions. Image
  // metadata is never cached here, so reloads and conflicts remain observable.
  if (Image.Arch == Arch::AArch64) {
    static const auto Declarations = foundationDeclarations(Arch::AArch64);
    return &Declarations;
  }
  if (Image.Arch == Arch::X64) {
    static const auto Declarations = foundationDeclarations(Arch::X64);
    return &Declarations;
  }
  return nullptr;
}
} // namespace

static std::optional<SourceFunctionTypeHint>
selectorSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                       const SourceFunctionTypeHint *FormatSignature) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return std::nullopt;
  std::optional<SourceFunctionTypeHint> Result;
  // The dynamic receiver class is unknown. No declaration is selected by
  // visitation order or by assuming that the receiver has a framework class.
  auto Include = [&](const auto &Method) {
    if (Method.Selector != Selector)
      return true;
    if (!Method.TypeHint)
      return false;
    auto Hint = *Method.TypeHint;
    std::string Diagnostic;
    if (!assignDarwinObjCSourceABI(Hint, Image.Arch, Diagnostic) ||
        (Result && !mergeSignature(*Result, Hint)))
      return false;
    if (!Result)
      Result = std::move(Hint);
    return true;
  };
  for (const auto &Method : Image.ObjCMethods)
    if (!Include(Method))
      return std::nullopt;
  for (const auto &Protocol : Image.ObjCProtocols)
    for (const auto &Method : Protocol.Methods)
      if (!Include(Method))
        return std::nullopt;
  if (const auto *Declarations = frameworkDeclarations(Image)) {
    auto Found = Declarations->find(Selector.str());
    if (Found != Declarations->end()) {
      const auto *Declared = Found->second ? &*Found->second : FormatSignature;
      if (!Declared || (Result && !mergeSignature(*Result, *Declared)))
        return std::nullopt;
      if (!Result)
        Result = *Declared;
      Result->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
    }
  }
  return Result;
}

std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector) {
  return selectorSourceTypeHint(Image, Selector, nullptr);
}

std::optional<ObjCFormatDeclaration>
objcSelectorFormatDeclaration(const BinaryImage &Image,
                              llvm::StringRef Selector) {
  const auto *Framework = frameworkDeclarations(Image);
  if (!Framework)
    return std::nullopt;
  const auto Ordinary = Framework->find(Selector.str());
  if (Ordinary == Framework->end() || Ordinary->second)
    return std::nullopt;
  static constexpr struct {
    const char *Selector;
    const char *AArch64;
    const char *X64;
    unsigned FormatParameter;
    unsigned FixedCount;
  } Formats[] = {
#include "ObjCFormatDeclarations.inc"
  };
  std::optional<ObjCFormatDeclaration> Result;
  for (const auto &D : Formats) {
    if (D.Selector != Selector)
      continue;
    // More than one declaration must never be resolved by visitation order.
    if (Result)
      return std::nullopt;
    auto Signature = parseObjCMethodEncoding(
        Selector, Image.Arch == Arch::AArch64 ? D.AArch64 : D.X64);
    std::string Diagnostic;
    if (!Signature || Signature->Parameters.size() != D.FixedCount ||
        D.FormatParameter >= D.FixedCount ||
        Signature->Parameters[D.FormatParameter].Type->Kind != NdTypeKind::Ptr)
      return std::nullopt;
    Signature->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
    if (!assignDarwinObjCSourceABI(*Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    Signature = selectorSourceTypeHint(Image, Selector, &*Signature);
    if (!Signature)
      return std::nullopt;
    Result = ObjCFormatDeclaration{std::move(*Signature), D.FormatParameter};
  }
  return Result;
}
} // namespace neverd
