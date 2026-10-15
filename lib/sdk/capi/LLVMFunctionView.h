//===- LLVMFunctionView.h - One function as an LLVM module ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The LLVM IR view of one native function.  The function moves into a module
/// of its own, as `llvm-extract` leaves it: the types, globals and functions
/// it refers to are declared before it, and the attribute groups and metadata
/// it uses follow it, so the text reads and assembles on its own.  Comments
/// give the image addresses behind NeverD's image mirrors and functions: each
/// mirror is declared under a comment that says which image bytes it stands
/// for and which of them the function uses.  The view records line by line
/// the image address each such name stands for.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_LLVMFUNCTIONVIEW_H
#define NEVERD_SDK_CAPI_LLVMFUNCTIONVIEW_H

#include "neverd/Common.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <string>
#include <vector>

namespace llvm {
class Function;
} // namespace llvm

namespace neverd {
struct BinaryImage;

namespace sdk {
struct Session;

/// A global that a line of the view names, and the image address it stands
/// for there: a function's entry, an image mirror's first byte, or the byte a
/// constant offset from a mirror reaches.
struct LLVMViewReference {
  /// The line of the text, from 0.
  size_t Line = 0;
  /// The global as the line spells it, `@` included (`@"a b"` keeps quotes).
  std::string Name;
  va_t Address = 0;
};

/// One function's LLVM IR view.
struct LLVMFunctionView {
  std::string Text;
  /// Where the function's own lines begin and where they end: the
  /// declarations it refers to come before them, the attribute groups and
  /// metadata it uses after them.
  size_t DefinitionBegin = 0;
  size_t DefinitionEnd = 0;
  /// One for each name of a global with an image address, in text order.
  std::vector<LLVMViewReference> References;
};

/// What a comment says of an image address after the address: the name and
/// string of the data there, or the name of the function there.  Empty for
/// nothing to say.
using LLVMViewAddressNote = llvm::function_ref<std::string(va_t Address)>;

/// \p F as its view, with \p DataNote beside what its code reaches through
/// image mirrors and \p FunctionNote beside each function's address.  When
/// \p F cannot move into a module of its own, the view shows \p F alone,
/// after a comment that says why.
LLVMFunctionView renderLLVMFunctionView(const llvm::Function &F,
                                        const BinaryImage &Image,
                                        LLVMViewAddressNote DataNote,
                                        LLVMViewAddressNote FunctionNote);

/// The view of \p S's function at \p Entry, from the inspection module that
/// shows it (no LLVM source optimization), kept with that module.  Null with
/// \p S's error set when the function has no LLVM.
const LLVMFunctionView *functionLlvmView(Session &S, va_t Entry);

} // namespace sdk
} // namespace neverd

#endif // NEVERD_SDK_CAPI_LLVMFUNCTIONVIEW_H
