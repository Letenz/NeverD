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

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;
struct CSourceMap;
struct CSourceName;
class DataAccessWidths;
struct NdType;

/// What a unit holds of the program's own definitions of the records it
/// names, from its local types.
struct CRecordDefinitions {
  /// C that defines the records and what they hold by value, which goes
  /// after the unit's includes.
  std::string Text;
  /// The records Text defines, by the names the unit spells them by, and
  /// whether each is a union.
  std::map<std::string, bool> Records;
  /// Every record Text names or the unit asked for, defined or not, and
  /// whether each is a union, which the unit's own declarations of it say.
  std::map<std::string, bool> Tags;
  /// The ordinary identifiers Text declares, which the unit's own names
  /// step around.
  std::vector<std::string> Identifiers;
  /// The enumerations Text defines, by their tags' names.
  std::vector<std::string> Enumerations;
};

struct CEmitterOptions {
  bool EmitIncludes = true;
  bool EmitComments = true;
  /// Wrap record declarations in stable preprocessor guards so independently
  /// emitted translation units can be concatenated. A consumer that parses one
  /// complete unit and performs its own cross-unit deduplication can disable
  /// the guards without changing the declarations themselves.  The local
  /// types' definitions (RecordDefinitions) keep their guards regardless.
  bool EmitRecordGuards = true;
  bool UseDebugNames = true;
  /// Structured pseudocode may use C++ exception syntax. A plain C view
  /// keeps runtime calls and renders unwind-table edges and handler entries
  /// explicitly, as the Itanium C projection already does. Platform C
  /// extensions such as MSVC SEH remain C syntax.
  bool StructuredExceptionSyntax = true;
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
  /// The widths of the reads and writes the code of the whole image states
  /// at each address, which name unnamed image data as the listing names it
  /// (neverd/DataNames.def): `qword_` for data every such access reads or
  /// writes eight bytes at, `unk_` where they differ or none states one.
  /// Without it, data is named by the accesses of the functions emitted.
  const DataAccessWidths *AccessWidths = nullptr;
  /// Optional source-map sink. Recording never changes the emitted C text.
  CSourceMap *SourceMap = nullptr;
  /// Receives the names the text spells (CSourceMap::Names) without the
  /// marked render a source map needs.
  std::vector<CSourceName> *SourceNames = nullptr;
  /// Given the named records a unit's types reach, through pointers and
  /// arrays, the definitions the program's local types give them.  A record
  /// left out, as one the local types do not define or define otherwise
  /// than the debug information the unit reads its members by, stays
  /// declared only.
  std::function<CRecordDefinitions(
      const std::vector<std::shared_ptr<NdType>> &)>
      RecordDefinitions;
};

} // namespace neverd

#endif // NEVERD_BACKEND_C_CEMITTEROPTIONS_H
