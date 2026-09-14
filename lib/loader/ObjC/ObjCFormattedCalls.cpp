#include "neverd/loader/ObjC/ObjCFormattedCalls.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCConstantStrings.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include <algorithm>
#include <map>

namespace neverd {
std::optional<std::vector<TypeRef>>
objcFormatArgumentTypes(llvm::ArrayRef<uint16_t> Format) {
  if (Format.size() > 65536 || llvm::is_contained(Format, uint16_t(0)))
    return std::nullopt;
  size_t Cursor = 0;
  unsigned Sequential = 0;
  bool Positional = false, Unnumbered = false;
  std::map<unsigned, TypeRef> Types;
  auto Peek = [&]() { return Cursor < Format.size() ? Format[Cursor] : 0; };
  auto Digit = [&]() { return Peek() >= '0' && Peek() <= '9'; };
  auto Position = [&]() -> std::optional<unsigned> {
    const auto Start = Cursor;
    unsigned N = 0;
    while (Digit()) {
      N = std::min(65U, N * 10 + (Format[Cursor++] - '0'));
    }
    if (Peek() != '$') {
      Cursor = Start;
      return 0;
    }
    ++Cursor;
    return N && N <= 64 ? std::optional(N) : std::nullopt;
  };
  auto Argument = [&](unsigned Position, TypeRef Type) {
    Positional |= Position != 0;
    Unnumbered |= Position == 0;
    if (Positional && Unnumbered)
      return false;
    if (!Position)
      Position = ++Sequential;
    if (Position > 64)
      return false;
    auto [It, Added] = Types.emplace(Position, Type);
    return Added || equalSourceTypes(It->second, Type);
  };
  auto Width = [&]() {
    if (Peek() == '*') {
      ++Cursor;
      auto P = Position();
      return P && Argument(*P, NdType::makeInt(4, true));
    }
    while (Digit())
      ++Cursor;
    return true;
  };
  while (Cursor < Format.size()) {
    if (Format[Cursor++] != '%')
      continue;
    if (Peek() == '%') {
      ++Cursor;
      continue;
    }
    auto P = Position();
    if (!P)
      return std::nullopt;
    while (Peek() && Peek() < 128 &&
           llvm::StringRef("-+ #0'").contains(char(Peek())))
      ++Cursor;
    if (!Width())
      return std::nullopt;
    if (Peek() == '.') {
      ++Cursor;
      if (!Width())
        return std::nullopt;
    }
    std::string Length;
    if (Peek() && Peek() < 128 &&
        llvm::StringRef("hlqjztL").contains(char(Peek()))) {
      Length += char(Format[Cursor++]);
      if ((Length == "h" || Length == "l") && Peek() == Length.front())
        Length += char(Format[Cursor++]);
    }
    const auto Conversion = Peek();
    if (!Conversion || Conversion > 127)
      return std::nullopt;
    ++Cursor;
    TypeRef Type;
    if (llvm::StringRef("diouxX").contains(char(Conversion))) {
      if (Length == "L")
        return std::nullopt;
      const bool Narrow = Length.empty() || Length == "h" || Length == "hh";
      Type = NdType::makeInt(Narrow ? 4 : 8,
                             Conversion == 'd' || Conversion == 'i' ||
                                 Length == "h" || Length == "hh");
    } else if (llvm::StringRef("aAeEfFgG").contains(char(Conversion))) {
      if (!Length.empty() && Length != "l")
        return std::nullopt;
      Type = NdType::makeFloat(8);
    } else if (Conversion == 'c' || Conversion == 'C') {
      if (!Length.empty())
        return std::nullopt;
      Type = NdType::makeInt(4, true);
    } else if (Conversion == 's' || Conversion == 'S') {
      if (!Length.empty())
        return std::nullopt;
      Type = NdType::makePtr(
          NdType::makeInt(Conversion == 's' ? 1 : 2, Conversion == 's'));
    } else if (Conversion == '@' || Conversion == 'p') {
      if (!Length.empty())
        return std::nullopt;
      Type = NdType::makePtr(NdType::makeVoid());
    } else {
      // Count writes, locale extensions, long double, and wide C strings need
      // their own contracts; a plausible argument count is insufficient.
      return std::nullopt;
    }
    if (!Argument(*P, std::move(Type)))
      return std::nullopt;
  }
  std::vector<TypeRef> Result;
  for (const auto &[Position, Type] : Types) {
    if (Position != Result.size() + 1)
      return std::nullopt;
    Result.push_back(Type);
  }
  return Result;
}

std::optional<SourceCallTypeHint>
bindObjCFormatArguments(const BinaryImage &Image, SourceCallTypeHint Result,
                        unsigned FormatParameter, va_t FormatAddress) {
  auto Format = readObjCConstantString(Image, FormatAddress);
  auto Arguments =
      Format ? objcFormatArgumentTypes(Format->Units) : std::nullopt;
  if (!Arguments || FormatParameter >= Result.Signature.Parameters.size() ||
      !Result.Signature.Parameters[FormatParameter].Type ||
      Result.Signature.Parameters[FormatParameter].Type->Kind !=
          NdTypeKind::Ptr ||
      Result.Signature.Parameters.size() + Arguments->size() > 64)
    return std::nullopt;
  const auto Fixed = unsigned(Result.Signature.Parameters.size());
  Result.Format = SourceCallTypeHint::FormatArguments{Fixed, FormatParameter,
                                                      FormatAddress};
  for (const auto &Type : *Arguments)
    Result.Signature.Parameters.push_back({"format_arg", Type});
  std::string Diagnostic;
  if (!assignDarwinVariadicSourceABI(Result.Signature, Fixed, Image.Arch,
                                     Diagnostic))
    return std::nullopt;
  return Result;
}

std::optional<SourceCallTypeHint>
objcFormattedSourceCallHint(const BinaryImage &Image, llvm::StringRef Selector,
                            va_t FormatAddress) {
  auto Declaration = objcSelectorFormatDeclaration(Image, Selector);
  if (!Declaration)
    return std::nullopt;
  SourceCallTypeHint Call;
  Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Call.Selector = Selector.str();
  Call.TargetName = "objc_msgSend";
  Call.Signature = std::move(Declaration->Signature);
  return bindObjCFormatArguments(Image, std::move(Call),
                                 Declaration->FormatParameter, FormatAddress);
}
} // namespace neverd
