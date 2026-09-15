#ifndef NEVERD_LOADER_OBJC_OBJCSOURCEDECLARATIONS_H
#define NEVERD_LOADER_OBJC_OBJCSOURCEDECLARATIONS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include "llvm/ADT/StringRef.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// Merge every known declaration for a dynamically dispatched selector.
/// Runtime declarations and applicable framework declarations must agree;
/// unsupported or variadic declarations veto the source call signature.
/// This supplies source-call ABI facts, never a dynamic receiver identity or
/// permission to rewrite a method implementation.
std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector);

/// Unsupported or conflicting declarations veto a narrowed call contract.
/// Missing external hierarchy instead requires selector-wide agreement: it
/// cannot justify excluding other owners. Self includes known subclasses.
struct ObjCReceiverDeclaration {
  bool HasDeclaration = false;
  std::optional<SourceFunctionTypeHint> Signature;
  bool RequiresGlobalAgreement = false;
};
ObjCReceiverDeclaration
objcReceiverSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                           const ObjCReceiverTypeHint &Receiver);

std::optional<ObjCReceiverTypeHint>
objcMethodReceiverTypeHint(const BinaryImage &Image, va_t Entry);
bool objcReceiverTypeHintValid(const BinaryImage &Image,
                               const ObjCReceiverTypeHint &Receiver);

struct ObjCFormatDeclaration {
  SourceFunctionTypeHint Signature;
  unsigned FormatParameter = 0;
};

/// A compiler-declared NSString format contract, after agreement with every
/// runtime and protocol declaration. Signature contains only the fixed prefix;
/// callers must prove the format and assign all promoted variadic arguments.
std::optional<ObjCFormatDeclaration>
objcSelectorFormatDeclaration(const BinaryImage &Image,
                              llvm::StringRef Selector);
} // namespace neverd
#endif
