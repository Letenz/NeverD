//===- CEmitterOptions.h - C emitter configuration -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Options controlling C source emission from decompiled IR.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CEMITTEROPTIONS_H
#define NEVERD_BACKEND_C_CEMITTEROPTIONS_H

#include "neverd/Common.h"

#include <map>
#include <string>

namespace neverd {

struct BinaryImage;
struct CSourceMap;

struct CEmitterOptions {
  bool EmitIncludes = true;
  bool EmitComments = true;
  /// Wrap record declarations in stable preprocessor guards so independently
  /// emitted translation units can be concatenated. A consumer that parses one
  /// complete unit and performs its own cross-unit deduplication can disable
  /// the guards without changing the declarations themselves.
  bool EmitRecordGuards = true;
  bool UseDebugNames = true;
  /// LLVMC clients with a complete source contract can retain the module's
  /// function types and request prototypes for definitions as well as imports.
  /// Disables inferred-void and debug-signature projections in that route.
  bool PreserveLLVMFunctionTypes = false;
  /// Spell ordinary scalar byte accesses through pointers, as
  /// ScalarPointers says. Ordered and unusual-width accesses retain their
  /// existing exact semantics. This is the default; clear it for portable
  /// byte copies, which any C compiler accepts.
  bool UseUnalignedPointers = true;
  /// How UseUnalignedPointers spells an access.
  enum class ScalarPointerSpelling : uint8_t {
    /// The standard scalar types, `*(uint64_t *)p`, as decompiled code reads
    /// best. The C assumes a target that accesses scalars at any alignment
    /// (x86, AArch64) and -fno-strict-aliasing; 16-byte accesses, which
    /// compilers move with aligned vector instructions, stay byte copies.
    StandardTypes,
    /// Clang/GCC aligned(1), may_alias alias types, `*(_QWORD *)p`, which
    /// keep unaligned and overlapping storage exact under any GCC/Clang
    /// build without asserting an effective type. Generated sources that
    /// promise byte semantics to their compilers use these.
    AliasTypes,
  };
  ScalarPointerSpelling ScalarPointers = ScalarPointerSpelling::StandardTypes;
  Arch TheArch = Arch::X64;
  BinaryFormat Format = BinaryFormat::Unknown;
  /// When set, HighC can fold rdata integer loads, print printable
  /// rdata C/wchar literals, and name image-backed data objects
  /// instead of emitting raw virtual addresses.
  const BinaryImage *Image = nullptr;
  /// The names the user gave addresses, which name image data ahead of debug
  /// information and symbols.
  const std::map<va_t, std::string> *UserNames = nullptr;
  /// Optional source-map sink. Recording never changes the emitted C text.
  CSourceMap *SourceMap = nullptr;
};

} // namespace neverd

#endif // NEVERD_BACKEND_C_CEMITTEROPTIONS_H
