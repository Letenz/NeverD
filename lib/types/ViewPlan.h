//===- ViewPlan.h - Records read as views over their bytes ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What the languages that read a record as a view over its bytes share,
/// JavaScript and Lua: an accessor for each member at the offset the library
/// records, the classes they read through, and how each value reads.  The
/// members of an anonymous record are the parent's, as C reaches them; a
/// named member of a record defined in place reads through a class of its
/// own, named as planRecord names it.  Each language only spells the plan.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TYPES_VIEWPLAN_H
#define NEVERD_LIB_TYPES_VIEWPLAN_H

#include "TypeRenderShared.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd::types::render {

/// How a view reads one value.
struct ViewValue {
  enum class Form : uint8_t {
    Integer, ///< Of 1, 2, 4, 8 or 16 bytes.
    Bool,    ///< One byte, true when it is not zero.
    Float,   ///< IEEE binary32 or binary64.
    Bytes,   ///< Bytes no view reads as one value, such as an x87 float's.
  };
  Form TheForm = Form::Bytes;
  unsigned Bytes = 0;
  bool Signed = false;
  /// A pointer, which reads as the address it holds.
  bool Address = false;
  /// What the program names the type, a typedef's or an enumeration's name,
  /// or a builtin's Windows name under that spelling; empty otherwise.
  std::string Named;
  /// The builtin Named gives Windows' name of, which a module declares.
  std::optional<Builtin> Windows;
};

/// One member a view reads.
struct ViewAccessor {
  enum class Kind : uint8_t {
    Value,  ///< A value, or with Bounds an array of them.
    Record, ///< A record in place, or with Bounds an array of them.
    Bits,   ///< A bitfield in its storage unit.
    Unread, ///< Nothing a view reads, as a bitfield in a 16-byte unit.
  };
  Kind TheKind = Kind::Value;
  /// Unique in its class.
  std::string Name;
  /// For an array of values, the method that writes an element.
  std::string Setter;
  /// What C declares, after its offset when comments give offsets.
  std::string Declared;
  /// From the class's record, in bytes.
  uint64_t Offset = 0;
  /// An array's bounds, outermost first, zero for a flexible one, and the
  /// bytes each index steps.
  std::vector<uint64_t> Bounds, Strides;
  ViewValue Value;
  /// Record: the class it reads through, and what C names its type.
  std::string Class, Named;
  /// Record: its bytes.
  uint64_t Size = 0;
  /// Bits: the field in its unit, and the unit's bytes.
  BitAccess Bits;
  unsigned UnitBytes = 0;
};

/// A class a view reads a record through.
struct ViewClass {
  std::string Name;
  /// What the class says of its record first.
  std::string Doc;
  const RecordDef *Record = nullptr;
  /// The digits of the outer record's offsets, which its code and comments
  /// pad to.
  unsigned Digits = 1;
  std::vector<ViewAccessor> Accessors;
};

/// How a typedef reads.
struct ViewTypedef {
  enum class Kind : uint8_t {
    Record,   ///< Another name of a record's class.
    Enum,     ///< Another name of an enumeration.
    Value,    ///< A value's type; Value.Address when it holds an address.
    Array,    ///< An array, which a member of it reads by index.
    Function, ///< A function's type: a pointer to it holds its address.
    Other,    ///< Nothing a view reads, such as void.
  };
  Kind TheKind = Kind::Other;
  std::string Name;
  /// Record and Enum: the class or enumeration it names, and what C calls
  /// that type.
  std::string Target, Named;
  /// What C declares the typedef as.
  std::string Declared;
  /// Value: whether the typedef names a pointer itself.
  bool Pointer = false;
  ViewValue Value;
};

/// An enumeration a module defines: its name, its definition, and what
/// its first comment calls it.
struct ViewEnum {
  std::string Name;
  const EnumDef *Enum = nullptr;
  std::string Doc;
};

class ViewPlanner {
public:
  /// \p Lang spells names; \p Reserved are the names a class takes for
  /// itself, such as JavaScript's constructor, which accessors step around.
  ViewPlanner(const TypeLibrary &Library, const RenderOptions &Options,
              Language Lang, std::vector<std::string> Reserved = {});

  /// The classes \p Type reads through, those of the records it defines in
  /// place for named members before its own, and the enumerations it
  /// defines in place.  A record only declared has none.
  void record(const LocalType &Type, std::vector<ViewClass> &Classes,
              std::vector<ViewEnum> &Enums) const;
  ViewTypedef typedefinition(const LocalType &Type) const;
  /// How \p Type reads, or nothing for a record, an array or a function.
  std::optional<ViewValue> value(const TypeExpr &Type) const;
  /// How \p Type reads; with \p Spelled, named as the spelling names it.
  std::optional<ViewValue> builtin(Builtin Type, bool Spelled = true) const;

  const NameSpelling &names() const { return Names; }
  const RenderOptions &options() const { return Options; }
  unsigned pointerBytes() const { return PointerBytes; }

private:
  struct State;
  /// The record \p Type names through typedefs, if it names one.
  const LocalType *namedRecord(const TypeExpr &Type) const;
  /// \p Type's element through arrays, typedefs of arrays too, with their
  /// bounds outermost first.
  const TypeExpr &element(const TypeExpr &Type,
                          std::vector<uint64_t> &Bounds) const;
  void classes(const RecordPlan &Plan, const std::string &Doc,
               const std::string &Outer, unsigned Digits,
               std::vector<ViewClass> &Classes,
               std::vector<ViewEnum> &Enums) const;
  void members(const RecordPlan &Plan, uint64_t Base, State &Into) const;
  void field(const Slot &Place, uint64_t At, State &Into) const;
  void bitfields(const Slot &Place, uint64_t At, State &Into) const;

  const TypeLibrary &Library;
  const RenderOptions &Options;
  const BuiltinSpelling Spelling;
  const NameSpelling Names;
  const unsigned PointerBytes;
  const CSpeller C;
  const std::vector<std::string> Reserved;
};

} // namespace neverd::types::render

#endif // NEVERD_LIB_TYPES_VIEWPLAN_H
