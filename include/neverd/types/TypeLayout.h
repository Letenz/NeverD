//===- TypeLayout.h - Where a compiler puts a record's members -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Records as source declares them, members in order without offsets, laid
/// out as a compiler would: each member's offset, each bitfield's storage
/// unit, and the record's size and alignment.  Parsed C, C++, Rust and Go
/// become library records through here.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_TYPELAYOUT_H
#define NEVERD_TYPES_TYPELAYOUT_H

#include "neverd/types/LocalTypes.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::types {

/// A member as source declares it.
struct DraftMember {
  /// Empty for an anonymous record, or an unnamed bitfield, which only
  /// takes bits.
  std::string Name;
  /// A record in place is a TypeExprKind::Record already laid out.
  TypeExprRef Type;
  bool Bitfield = false;
  /// A bitfield's width; zero ends the run of bitfields before it.
  uint32_t Width = 0;
  std::string Comment;
  /// The alignment the member is declared with (__declspec(align),
  /// __attribute__((aligned))), which the pack does not lower; zero for
  /// none.
  uint32_t Alignment = 0;
  /// GNU's packed attribute on the member: it aligns to a byte.
  bool Packed = false;
};

/// A record as source declares it.
struct DraftRecord {
  bool Union = false;
  /// The #pragma pack in force where its body begins; zero for none.
  uint32_t Pack = 0;
  /// The alignment it is declared with (__declspec(align), alignas);
  /// zero for none.
  uint32_t Alignment = 0;
  /// A C++ record: one with no bytes takes one, where Microsoft's C gives it
  /// four, or its declared alignment, and GNU's C none.
  bool Cpp = false;
  std::vector<DraftMember> Members;
};

/// \p Draft laid out as \p Library's program lays records out (its
/// recordABI).  An error names a member whose size the library does not
/// know, such as a record only declared, held by value.
llvm::Expected<RecordDef> layOutRecord(const TypeLibrary &Library,
                                       const DraftRecord &Draft);

} // namespace neverd::types

#endif // NEVERD_TYPES_TYPELAYOUT_H
