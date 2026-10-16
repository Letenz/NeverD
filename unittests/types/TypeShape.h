//===- TypeShape.h - A record's nesting as one line -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A record's members as one line, for tests to compare: a member is
/// Name@byte, a bitfield Name@bit b:width, an anonymous record
/// union@byte{...} or struct@byte{...}, and a named member of an anonymous
/// type Name@byte:struct{...}.  Offsets are from the record holding the
/// member.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TYPES_TYPESHAPE_H
#define NEVERD_UNITTESTS_TYPES_TYPESHAPE_H

#include "neverd/types/LocalTypes.h"

#include <string>

namespace neverd::types::test {

inline std::string shapeOf(const RecordDef &Record);

inline std::string memberShape(const Member &Field) {
  const bool Inline = Field.Type && Field.Type->Kind == TypeExprKind::Record &&
                      Field.Type->Record;
  if (Field.Name.empty() && Inline) {
    const RecordDef &Inner = *Field.Type->Record;
    std::string Body = shapeOf(Inner);
    const size_t Brace = Body.find('{');
    return Body.substr(0, Brace) + "@" + std::to_string(Field.BitOffset / 8) +
           Body.substr(Brace);
  }
  if (Field.isBitfield())
    return Field.Name + "@" + std::to_string(Field.BitOffset) +
           "b:" + std::to_string(Field.BitWidth);
  std::string Shape = Field.Name + "@" + std::to_string(Field.BitOffset / 8);
  if (Inline)
    Shape += ":" + shapeOf(*Field.Type->Record);
  return Shape;
}

inline std::string shapeOf(const RecordDef &Record) {
  std::string Shape = Record.Union ? "union{" : "struct{";
  for (size_t I = 0; I < Record.Members.size(); ++I) {
    if (I)
      Shape += ' ';
    Shape += memberShape(Record.Members[I]);
  }
  return Shape + "}";
}

} // namespace neverd::types::test

#endif // NEVERD_UNITTESTS_TYPES_TYPESHAPE_H
