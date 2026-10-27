//===- TypeFit.h - Local types that fit a pointer's accesses ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Where an access through a pointer lands among a record's members, and
/// which of a program's records the code's accesses through one pointer
/// fit.  The listing names an operand by the member it lands on
/// (memberAt), and a search for the type to give a pointer ranks records by
/// how many of its accesses land exactly on members of their width
/// (fitTypes), as an analyst checks a guess against the code.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_TYPEFIT_H
#define NEVERD_TYPES_TYPEFIT_H

#include "neverd/types/Limits.h"
#include "neverd/types/LocalTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::types {

/// What the code does with the bytes a pointer points at.
enum class AccessUse : uint8_t {
  Read,
  Write,
  /// Takes their address, as `&p->ApcListEntry` does.
  Address,
};

/// One access through a pointer: Bytes bytes at Offset from where it
/// points, none for an address.
struct PointerAccess {
  int64_t Offset = 0;
  uint64_t Bytes = 0;
  AccessUse Use = AccessUse::Read;
};

/// One step from a record to what it holds: a named member, or an element
/// of an array.  Anonymous unions and structures take no step.
struct MemberStep {
  std::string Name;
  std::optional<uint64_t> Element;
  bool operator==(const MemberStep &) const = default;
};

/// How an access lands among a record's members, best first.
enum class AccessFit : uint8_t {
  /// A member as wide as the access begins where it does, or a record or
  /// array the access reads whole; for an address, a member begins there.
  Exact,
  /// A member begins there but is wider or narrower.
  Start,
  /// It begins inside a member.
  Inside,
  /// No member holds its first byte.
  Padding,
  /// It reaches before the record or past its end.
  Outside,
};

/// Where an access lands in a record.
struct MemberAt {
  AccessFit Fit = AccessFit::Outside;
  /// The member it lands on, from the record: none for the record itself,
  /// and for padding the member that holds the padding, if any.
  std::vector<MemberStep> Path;
  /// Where that member begins in the record and its size: the access begins
  /// Offset - MemberOffset bytes into it.  Padding and an access outside the
  /// record have no size.
  uint64_t MemberOffset = 0;
  uint64_t MemberBytes = 0;
  /// Its type; null for the record itself, padding and outside.
  TypeExprRef Type;
  /// The member is the storage of bitfields, named by its first bitfield.
  bool Bitfield = false;
};

/// Where \p Bytes bytes at \p Offset land in \p Record, a structure or
/// union of \p Library.  An access of no bytes lands Exact on the outermost
/// member that begins at its offset.  A member reached whole is the access,
/// unless a scalar as wide begins with it, which is then (`Header.Lock` for
/// four bytes of a dispatcher header).  Of the alternatives of a union, the
/// one the access fits best is taken; among equal ones a named member before
/// an array's element, a member before bitfields, then the first declared.
MemberAt memberAt(const TypeLibrary &Library, const RecordDef &Record,
                  int64_t Offset, uint64_t Bytes);

/// \p Path as C spells it after `.` or `->`:
/// `Tail.Overlay.CurrentStackLocation`, `Reserved[1]`.
std::string memberPathText(llvm::ArrayRef<MemberStep> Path);

/// A run of characters a pattern matched: where it begins and its length,
/// in bytes of the text.
struct MatchRun {
  uint32_t Begin = 0;
  uint32_t Length = 0;
  bool operator==(const MatchRun &) const = default;
};

/// How a pattern matched a text.
struct FuzzyMatch {
  int Score = 0;
  std::vector<MatchRun> Runs;
};

/// \p Pattern matched against \p Text as a subsequence, ignoring the case
/// of ASCII letters, and scored as an editor's quick open scores: a match
/// at the start of the name (after leading underscores) and at the starts
/// of words (after `_`, at a capital after a small letter) counts most, a
/// run of adjacent characters more than scattered ones, and skipped
/// characters count against it.  None when \p Pattern is no subsequence.
std::optional<FuzzyMatch> fuzzyMatch(llvm::StringRef Pattern,
                                     llvm::StringRef Text);

/// A search for the types a pointer may point at.
struct FitQuery {
  /// What the code does through the pointer.  With none, the types rank by
  /// how well Text matches them.
  std::vector<PointerAccess> Accesses;
  /// Matched against each type's name and the names of the typedefs of it
  /// and of a pointer to it.  `Type.member` also matches a member path of
  /// the type after the dot, and `.member` the member paths of every type.
  /// Empty matches every type.
  std::string Text;
  /// Only structures and unions, each with its typedefs as aliases, or
  /// every named type under its own name, a typedef of a record or of a
  /// pointer to one judged by that record's fit.
  bool RecordsOnly = true;
  /// Whether the pointer may point into a record, as a pointer to a list
  /// entry does, when accesses reach before where it points.
  bool Shifts = true;
  size_t Limit = ::neverd::limits::types::kTypeFitLimit;
};

/// A type a search found.
struct TypeFit {
  const LocalType *Type = nullptr;
  /// The record the accesses were judged against: the type itself, or the
  /// record a typedef names or points at.
  const LocalType *Record = nullptr;
  /// Records only: the typedefs that name the record (`KAPC`), and those
  /// that name a pointer to it (`PKAPC`).
  std::vector<std::string> Aliases;
  std::vector<std::string> PointerAliases;
  /// Where the pointer points into the record, as CONTAINING_RECORD undoes:
  /// zero for its start, otherwise the member there.
  uint64_t Shift = 0;
  std::vector<MemberStep> ShiftPath;
  /// Where each access of the query lands, in order.
  std::vector<MemberAt> Fits;
  /// How many landed exactly on a named member and on an array's element,
  /// at the start of or inside a member, on padding, and outside.
  unsigned Exact = 0, ExactElements = 0, Partial = 0, Padding = 0, Outside = 0;
  /// The name Text matched, the type's or an alias's, and where.
  std::string MatchedName;
  std::vector<MatchRun> MatchedRuns;
  /// For `Type.member`: the member path that matched, and where in its text.
  std::vector<MemberStep> MatchedMember;
  std::vector<MatchRun> MatchedMemberRuns;
  int MatchScore = 0;
};

/// The types of \p Library that \p Query finds, best first.  With accesses,
/// the records that hold every access come first, then those the accesses
/// fit better: an access landing exactly on a named member counts most, on
/// an array's element a little less, and one landing in part on a member
/// or on padding counts against.  Then a better match of Text, less room
/// past the last access, the program's own types before a header
/// library's, and the name.  Without accesses, a better match of Text, then
/// a shorter name.  \p Total, if given, receives how many types matched
/// before the limit.
std::vector<TypeFit> fitTypes(const TypeLibrary &Library, const FitQuery &Query,
                              size_t *Total = nullptr);

} // namespace neverd::types

#endif // NEVERD_TYPES_TYPEFIT_H
