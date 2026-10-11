//===- Primitive.cpp - Closed JavaScript primitive expressions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact UTF-16 and Number/BigInt output without global identifier lookups.
///
//===----------------------------------------------------------------------===//

#include "../PrimitiveNumbers.h"
#include "RewriteInternal.h"

#include "neverd/web/Error.h"

namespace neverd::web::rewrite {
namespace {
bool nan(uint64_t Bits) {
  return (Bits & UINT64_C(0x7ff0000000000000)) ==
             UINT64_C(0x7ff0000000000000) &&
         (Bits & UINT64_C(0x000fffffffffffff));
}
} // namespace

void charge(uint64_t &Steps, uint64_t Count) {
  if (Steps > MaxSourceRewriteSteps || Count > MaxSourceRewriteSteps - Steps)
    throw Error("source_rewrite_work_budget_exceeded");
  Steps += Count;
}

void append(std::string &Output, std::string_view Bytes, uint64_t &Steps) {
  if (Output.size() > MaxSourceRewriteBytes ||
      Bytes.size() > MaxSourceRewriteBytes - Output.size())
    throw Error("source_rewrite_byte_budget_exceeded");
  charge(Steps, Bytes.size() + 1);
  Output += Bytes;
}

std::string primitive(const SourcePrimitive &V, uint64_t &Steps) {
  std::string Text = "(";
  switch (V.Kind) {
  case PrimitiveKind::Undefined:
    append(Text, "void 0", Steps);
    break;
  case PrimitiveKind::Null:
    append(Text, "null", Steps);
    break;
  case PrimitiveKind::Boolean:
    append(Text, V.Boolean ? "true" : "false", Steps);
    break;
  case PrimitiveKind::Number:
    if (nan(V.NumberBits))
      append(Text, "0/0", Steps);
    else if (V.NumberBits == UINT64_C(0x7ff0000000000000))
      append(Text, "1/0", Steps);
    else if (V.NumberBits == UINT64_C(0xfff0000000000000))
      append(Text, "-1/0", Steps);
    else if (V.NumberBits == UINT64_C(0x8000000000000000))
      append(Text, "-0", Steps);
    else
      append(Text, primitiveNumberToString(V.NumberBits), Steps);
    break;
  case PrimitiveKind::String:
    append(Text, "\"", Steps);
    for (const auto Unit : V.String) {
      charge(Steps);
      if (Unit >= 0x20 && Unit <= 0x7e && Unit != '"' && Unit != '\\') {
        const char ASCII = static_cast<char>(Unit);
        append(Text, std::string_view(&ASCII, 1), Steps);
      } else {
        constexpr char Hex[] = "0123456789abcdef";
        const char Escape[] = {'\\',
                               'u',
                               Hex[(Unit >> 12) & 15],
                               Hex[(Unit >> 8) & 15],
                               Hex[(Unit >> 4) & 15],
                               Hex[Unit & 15]};
        append(Text, std::string_view(Escape, sizeof(Escape)), Steps);
      }
    }
    append(Text, "\"", Steps);
    break;
  case PrimitiveKind::BigInt:
    append(Text, V.BigInt, Steps);
    append(Text, "n", Steps);
    break;
  }
  append(Text, ")", Steps);
  return Text;
}

bool sameValue(const SourcePrimitive &A, const SourcePrimitive &B,
               uint64_t &Steps) {
  charge(Steps);
  if (A.Kind != B.Kind)
    return false;
  switch (A.Kind) {
  case PrimitiveKind::Undefined:
  case PrimitiveKind::Null:
    return true;
  case PrimitiveKind::Boolean:
    return A.Boolean == B.Boolean;
  case PrimitiveKind::Number:
    return A.NumberBits == B.NumberBits ||
           (nan(A.NumberBits) && nan(B.NumberBits));
  case PrimitiveKind::String:
    charge(Steps, A.String.size() + B.String.size());
    return A.String == B.String;
  case PrimitiveKind::BigInt:
    charge(Steps, A.BigInt.size() + B.BigInt.size());
    return A.BigInt == B.BigInt;
  }
  return false;
}

} // namespace neverd::web::rewrite
