//===- SymbolSpellingObjC.cpp - Objective-C method names ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Objective-C method implementations, which clang names `-[Class selector:]`
/// and `+[Class(Category) selector]`.  The name already reads as the language
/// spells it, once its parts are spelled with the characters of Objective-C
/// names.  C calls it as the GNU Objective-C runtime has always named
/// methods in C (ObjCMethodSpellings.def): `_i` or `_c` for an instance or
/// class method, the class, the category (empty without one) and the
/// selector with each `:` written `_`, all joined by `_`.  So
/// `-[NSString length]` is `_i_NSString__length` and
/// `+[NSObject(Extras) make:with:]` is `_c_NSObject_Extras_make_with_`.
///
//===----------------------------------------------------------------------===//

#include "SymbolSpellingDetail.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"

#include <optional>

using namespace neverd;
using namespace neverd::symbol_spelling;

namespace {

struct ObjCMethodName {
  llvm::StringRef CPrefix;
  llvm::StringRef ClassName;
  llvm::StringRef Category;
  llvm::StringRef Selector;
};

/// Whether \p Text is spelled with the characters of Objective-C names, and
/// with `:` too when it is a selector.  A Swift class's mangled name is one.
bool isObjCNameText(llvm::StringRef Text, bool Selector) {
  return !Text.empty() && llvm::all_of(Text, [Selector](char C) {
    return llvm::isAlnum(C) || C == '_' || C == '$' || (Selector && C == ':');
  });
}

std::optional<ObjCMethodName> parseObjCMethodName(llvm::StringRef Name) {
  ObjCMethodName Method;
#define NEVERD_OBJC_METHOD_KIND(Marker, Prefix, IsClassMethod)                 \
  if (Method.CPrefix.empty() && Name.starts_with(Marker "["))                  \
    Method.CPrefix = Prefix;
#include "neverd/loader/ObjCMethodSpellings.def"
  if (Method.CPrefix.empty() || !Name.consume_back("]"))
    return std::nullopt;
  Name = Name.drop_front(Name.find('[') + 1);
  auto [Receiver, Selector] = Name.split(' ');
  if (Receiver.empty() || Selector.empty() || Selector.contains(' '))
    return std::nullopt;
  if (Receiver.consume_back(")")) {
    auto [ClassName, Category] = Receiver.split('(');
    if (ClassName.empty() || Category.empty())
      return std::nullopt;
    Receiver = ClassName;
    Method.Category = Category;
  }
  if (!isObjCNameText(Receiver, false) ||
      (!Method.Category.empty() && !isObjCNameText(Method.Category, false)) ||
      !isObjCNameText(Selector, true))
    return std::nullopt;
  Method.ClassName = Receiver;
  Method.Selector = Selector;
  return Method;
}

} // namespace

bool symbol_spelling::claimsObjCMethodName(llvm::StringRef Name) {
  return parseObjCMethodName(Name).has_value();
}

std::string symbol_spelling::readableObjCMethodName(llvm::StringRef Name) {
  // The name already reads as the language spells it.
  return Name.str();
}

std::string symbol_spelling::stemOfObjCMethodName(llvm::StringRef Name) {
  const auto Method = parseObjCMethodName(Name);
  if (!Method)
    return {};
  std::string Selector = Method->Selector.str();
  for (char &C : Selector)
    if (C == ':')
      C = '_';
  return (Method->CPrefix + "_" + Method->ClassName + "_" + Method->Category +
          "_" + Selector)
      .str();
}
