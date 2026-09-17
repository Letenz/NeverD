//===- CTypeFormat.h - Type to C string formatting -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Type-to-C-string formatting utilities.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_CTYPEFORMAT_H
#define NEVERD_BACKEND_C_RENDER_CTYPEFORMAT_H
#include "neverd/Common.h"
#include "neverd/ir/NdTypes.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace llvm {
class Type;
class StructType;
} // namespace llvm

namespace neverd {

std::string typeToC(const TypeRef &Ty);

/// Place a declarator inside a C type, including nested function pointers.
/// An empty declarator produces the abstract type used by a cast.
std::string declarationToC(const TypeRef &Ty, llvm::StringRef Declarator);

std::string typeToCLLVM(llvm::Type *Ty);
std::string llvmStructName(llvm::StructType *ST);

std::string escapeCString(llvm::StringRef Str);

/// MSVC `<intrin.h>` GS/FS reads (`__readgsqword` / `__readfsdword`, …).
/// Implemented with the x86 C renderer.  \p SizeBytes is the access width.
/// Returns null when that width has no matching intrinsic.
const char *x86SegmentedReadIntrinsic(bool GS, unsigned SizeBytes);

/// Returns the platform-specific intrinsic headers for the given arch.
/// Dispatches to the per-arch lists implemented alongside the intrinsic
/// renderers (HighCIntrinsicRender{X86,ARM}.cpp).
llvm::SmallVector<const char *, 3> getArchIntrinsicHeaders(Arch TheArch);
llvm::SmallVector<const char *, 3> getX86IntrinsicHeaders();
llvm::SmallVector<const char *, 3> getARMIntrinsicHeaders();

/// Emits \p Level levels of indentation (4 spaces each) to \p OS.
void emitCIndent(llvm::raw_ostream &OS, int Level);

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_CTYPEFORMAT_H
