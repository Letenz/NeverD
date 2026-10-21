//===- Reading.cpp - Lossless shorter spelling of NeverD C ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Hand-written scanners over the engine's C: no regular expression engine
/// recurses over a function's text, however long.
///
//===----------------------------------------------------------------------===//

#include "Reading.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd::binmcp {
namespace {

struct TypeName {
  std::string_view Long, Short;
};
constexpr TypeName TypeNames[] = {
    {"int64_t", "i64"},  {"uint64_t", "u64"}, {"int32_t", "i32"},
    {"uint32_t", "u32"}, {"int16_t", "i16"},  {"uint16_t", "u16"},
    {"int8_t", "i8"},    {"uint8_t", "u8"},   {"uintptr_t", "uptr"}};

/// A byte of an identifier; text beyond ASCII counts, as a letter would.
bool wordByte(char C) {
  const auto U = static_cast<unsigned char>(C);
  return U >= 0x80 || std::isalnum(U) || C == '_';
}
bool identifierStart(char C) {
  const auto U = static_cast<unsigned char>(C);
  return U >= 0x80 || std::isalpha(U) || C == '_';
}
bool upperHex(char C) {
  return (C >= '0' && C <= '9') || (C >= 'A' && C <= 'F');
}

/// The length of the string or character literal or comment at \p At, or 0.
std::size_t literalAt(std::string_view Text, std::size_t At) {
  const char C = Text[At];
  if (C == '"' || C == '\'') {
    for (std::size_t I = At + 1; I < Text.size();) {
      if (Text[I] == C)
        return I + 1 - At;
      if (Text[I] == '\n')
        return 0;
      if (Text[I] == '\\') {
        if (I + 1 >= Text.size())
          return 0;
        I += 2;
        continue;
      }
      ++I;
    }
    return 0;
  }
  if (C == '/' && At + 1 < Text.size()) {
    if (Text[At + 1] == '*') {
      const auto End = Text.find("*/", At + 2);
      return End == std::string_view::npos ? 0 : End + 2 - At;
    }
    if (Text[At + 1] == '/') {
      const auto End = Text.find('\n', At + 2);
      return (End == std::string_view::npos ? Text.size() : End) - At;
    }
  }
  return 0;
}

/// \p Text with \p Rewrite applied to the code between literals and comments.
std::string
outsideLiterals(std::string_view Text,
                const std::function<std::string(std::string_view)> &Rewrite) {
  std::string Out;
  Out.reserve(Text.size());
  std::size_t Last = 0;
  for (std::size_t I = 0; I < Text.size();) {
    if (const auto Length = literalAt(Text, I)) {
      Out += Rewrite(Text.substr(Last, I - Last));
      Out.append(Text.substr(I, Length));
      I += Length;
      Last = I;
    } else {
      ++I;
    }
  }
  Out += Rewrite(Text.substr(Last));
  return Out;
}

/// Each identifier of \p Code with where it starts, as a word does: one
/// starting with a digit is not an identifier.
template <typename VisitT>
void identifiers(std::string_view Code, VisitT Visit) {
  for (std::size_t I = 0; I < Code.size();) {
    if (!wordByte(Code[I])) {
      ++I;
      continue;
    }
    std::size_t End = I;
    while (End < Code.size() && wordByte(Code[End]))
      ++End;
    if (identifierStart(Code[I]))
      Visit(I, Code.substr(I, End - I));
    I = End;
  }
}

std::set<std::string> identifierSet(std::string_view Text) {
  std::set<std::string> Found;
  (void)outsideLiterals(Text, [&](std::string_view Code) {
    identifiers(
        Code, [&](std::size_t, std::string_view Name) { Found.emplace(Name); });
    return std::string(Code);
  });
  return Found;
}

/// \p Code with each whole identifier in \p Names replaced; \p Changed set
/// when one was.
std::string replaceIdentifiers(
    std::string_view Code,
    const std::map<std::string_view, std::string_view, std::less<>> &Names,
    bool *Changed = nullptr) {
  std::string Out;
  Out.reserve(Code.size());
  std::size_t Last = 0;
  identifiers(Code, [&](std::size_t At, std::string_view Name) {
    const auto It = Names.find(Name);
    if (It == Names.end())
      return;
    Out.append(Code.substr(Last, At - Last));
    Out.append(It->second);
    Last = At + Name.size();
    if (Changed)
      *Changed = true;
  });
  Out.append(Code.substr(Last));
  return Out;
}

/// The frame struct the engine declares a function's stack with:
///   \n<indent>_Alignas(N) struct frame_NAME {\n<members>\n<indent>}
///   stack_storage;
struct FrameBlock {
  std::size_t Begin, End; // [Begin, End) from the newline before it
  std::string_view Indent, Head, Members;
};

std::optional<FrameBlock> frameAt(std::string_view Text, std::size_t Newline) {
  std::size_t I = Newline + 1;
  while (I < Text.size() && (Text[I] == ' ' || Text[I] == '\t'))
    ++I;
  const std::size_t HeadBegin = I;
  constexpr std::string_view Align = "_Alignas(", Struct = ") struct frame_";
  if (Text.substr(I, Align.size()) != Align)
    return std::nullopt;
  I += Align.size();
  const std::size_t Digits = I;
  while (I < Text.size() && std::isdigit(static_cast<unsigned char>(Text[I])))
    ++I;
  if (I == Digits || Text.substr(I, Struct.size()) != Struct)
    return std::nullopt;
  I += Struct.size();
  const std::size_t Name = I;
  while (I < Text.size() && wordByte(Text[I]))
    ++I;
  if (I == Name || Text.substr(I, 3) != " {\n")
    return std::nullopt;
  const std::size_t HeadEnd = I;
  const std::size_t Body = I + 3;
  constexpr std::string_view Close = "} stack_storage;";
  // The members end at the first newline that a closing line follows.
  for (std::size_t Line = Text.find('\n', Body); Line != std::string_view::npos;
       Line = Text.find('\n', Line + 1)) {
    std::size_t J = Line + 1;
    while (J < Text.size() && (Text[J] == ' ' || Text[J] == '\t'))
      ++J;
    if (Text.substr(J, Close.size()) == Close)
      return FrameBlock{Newline, J + Close.size(),
                        Text.substr(Newline, HeadBegin - Newline),
                        Text.substr(HeadBegin, HeadEnd - HeadBegin),
                        Text.substr(Body, Line - Body)};
  }
  return std::nullopt;
}

/// \p Text with each frame struct passed to \p Replace.
std::string
replaceFrames(std::string_view Text,
              const std::function<std::string(const FrameBlock &)> &Replace) {
  std::string Out;
  std::size_t Last = 0;
  for (std::size_t I = Text.find('\n'); I != std::string_view::npos;) {
    if (const auto Frame = frameAt(Text, I)) {
      Out.append(Text.substr(Last, I - Last));
      Out += Replace(*Frame);
      Last = Frame->End;
      I = Text.find('\n', Frame->End);
    } else {
      I = Text.find('\n', I + 1);
    }
  }
  Out.append(Text.substr(Last));
  return Out;
}

std::string foldFrames(std::string_view Text) {
  return replaceFrames(Text, [](const FrameBlock &Frame) {
    std::string Out(Frame.Indent);
    Out.append(Frame.Head);
    Out += " { ";
    bool First = true;
    for (std::size_t I = 0; I <= Frame.Members.size();) {
      auto End = Frame.Members.find('\n', I);
      if (End == std::string_view::npos)
        End = Frame.Members.size();
      auto Line = Frame.Members.substr(I, End - I);
      while (!Line.empty() &&
             std::isspace(static_cast<unsigned char>(Line.front())))
        Line.remove_prefix(1);
      while (!Line.empty() &&
             std::isspace(static_cast<unsigned char>(Line.back())))
        Line.remove_suffix(1);
      if (!Line.empty()) {
        if (!First)
          Out += ' ';
        Out.append(Line);
        First = false;
      }
      I = End + 1;
    }
    Out += " } stack_storage;";
    return Out;
  });
}

/// A line that declares one variable: indentation, type and name.
struct Declaration {
  std::string_view Indent, Type, Name;
};

std::optional<Declaration> declaration(std::string_view Line) {
  std::size_t I = 0;
  while (I < Line.size() && (Line[I] == ' ' || Line[I] == '\t'))
    ++I;
  if (I == 0)
    return std::nullopt;
  const std::size_t TypeBegin = I;
  if (Line.substr(I, 6) == "const ")
    I += 6;
  if (I >= Line.size() || !identifierStart(Line[I]))
    return std::nullopt;
  const std::size_t First = I;
  while (I < Line.size() && wordByte(Line[I]))
    ++I;
  const auto Keyword = Line.substr(First, I - First);
  for (std::string_view Statement :
       {"return", "goto", "else", "break", "continue", "case", "default", "do",
        "struct", "union", "enum", "typedef"})
    if (Keyword == Statement)
      return std::nullopt;
  bool Separated = false;
  if (I + 1 < Line.size() && Line[I] == ' ' && Line[I + 1] == '*')
    ++I;
  while (I < Line.size() && Line[I] == '*') {
    ++I;
    Separated = true;
  }
  const std::size_t TypeEnd = I;
  while (I < Line.size() && std::isspace(static_cast<unsigned char>(Line[I]))) {
    ++I;
    Separated = true;
  }
  if (!Separated || I >= Line.size() || !identifierStart(Line[I]))
    return std::nullopt;
  const std::size_t Name = I;
  while (I < Line.size() && wordByte(Line[I]))
    ++I;
  if (I + 1 != Line.size() || Line[I] != ';')
    return std::nullopt;
  return Declaration{Line.substr(0, TypeBegin),
                     Line.substr(TypeBegin, TypeEnd - TypeBegin),
                     Line.substr(Name, I - Name)};
}

std::string mergeDeclarations(std::string_view Text) {
  std::vector<std::string_view> Lines;
  for (std::size_t I = 0; I <= Text.size();) {
    auto End = Text.find('\n', I);
    if (End == std::string_view::npos)
      End = Text.size();
    Lines.push_back(Text.substr(I, End - I));
    I = End + 1;
  }
  std::string Out;
  Out.reserve(Text.size());
  for (std::size_t Index = 0; Index < Lines.size();) {
    if (Index)
      Out += '\n';
    const auto First = declaration(Lines[Index]);
    if (!First) {
      Out.append(Lines[Index++]);
      continue;
    }
    Out.append(First->Indent);
    Out.append(First->Type);
    Out += ' ';
    Out.append(First->Name);
    for (++Index; Index < Lines.size(); ++Index) {
      const auto Next = declaration(Lines[Index]);
      if (!Next || Next->Indent != First->Indent || Next->Type != First->Type)
        break;
      Out += ", ";
      Out.append(Next->Name);
    }
    Out += ';';
  }
  return Out;
}

/// *(TYPE *)(uintptr_t)(&g_BASE_bytes[OFFSET]): a typed access at BASE plus
/// OFFSET.  Spaces may come where the engine's C admits them.
struct Access {
  std::size_t Length;
  std::string Type;
  std::uint64_t Address;
};

std::optional<Access> accessAt(std::string_view Code, std::size_t At) {
  std::size_t I = At;
  const auto Expect = [&](std::string_view Literal) {
    if (Code.substr(I, Literal.size()) != Literal)
      return false;
    I += Literal.size();
    return true;
  };
  const auto Spaces = [&] {
    while (I < Code.size() && std::isspace(static_cast<unsigned char>(Code[I])))
      ++I;
  };
  if (!Expect("*("))
    return std::nullopt;
  const std::size_t TypeBegin = I;
  if (Code.substr(I, 6) == "const ")
    I += 6;
  if (I >= Code.size() || !identifierStart(Code[I]))
    return std::nullopt;
  while (I < Code.size() && wordByte(Code[I]))
    ++I;
  const auto Type = Code.substr(TypeBegin, I - TypeBegin);
  Spaces();
  if (!Expect("*)"))
    return std::nullopt;
  Spaces();
  if (!Expect("(uintptr_t)"))
    return std::nullopt;
  Spaces();
  if (!Expect("(&g_"))
    return std::nullopt;
  const std::size_t Base = I;
  while (I < Code.size() && upperHex(Code[I]))
    ++I;
  const auto BaseText = Code.substr(Base, I - Base);
  if (BaseText.empty() || BaseText.size() > 16 || !Expect("_bytes["))
    return std::nullopt;
  const std::size_t Offset = I;
  while (I < Code.size() && std::isdigit(static_cast<unsigned char>(Code[I])))
    ++I;
  const auto OffsetText = Code.substr(Offset, I - Offset);
  if (OffsetText.empty() || !Expect("])"))
    return std::nullopt;
  std::uint64_t BaseValue = 0, OffsetValue = 0;
  if (std::from_chars(BaseText.data(), BaseText.data() + BaseText.size(),
                      BaseValue, 16)
              .ec != std::errc() ||
      std::from_chars(OffsetText.data(), OffsetText.data() + OffsetText.size(),
                      OffsetValue)
              .ec != std::errc() ||
      OffsetValue > UINT64_MAX - BaseValue)
    return std::nullopt;
  return Access{I - At, std::string(Type), BaseValue + OffsetValue};
}

std::string globalName(std::uint64_t Address) {
  static constexpr char Digits[] = "0123456789ABCDEF";
  std::string Hex;
  do {
    Hex.insert(Hex.begin(), Digits[Address & 0xF]);
    Address >>= 4;
  } while (Address);
  return "g_" + Hex;
}

template <typename VisitT> void accesses(std::string_view Code, VisitT Visit) {
  for (std::size_t I = Code.find('*'); I != std::string_view::npos;) {
    if (const auto Found = accessAt(Code, I)) {
      Visit(I, *Found);
      I = Code.find('*', I + Found->Length);
    } else {
      I = Code.find('*', I + 1);
    }
  }
}

/// \p Code with each `stack_storage.var_mN` and `stack_storage.gapN` passed
/// to \p Replace with its member's name, at word boundaries.
std::string
replaceFrameMembers(std::string_view Code,
                    const std::function<std::string(std::string_view)> &Replace,
                    bool *Changed = nullptr) {
  constexpr std::string_view Prefix = "stack_storage.";
  std::string Out;
  std::size_t Last = 0;
  for (std::size_t I = Code.find(Prefix); I != std::string_view::npos;
       I = Code.find(Prefix, I + 1)) {
    if (I > 0 && wordByte(Code[I - 1]))
      continue;
    std::size_t J = I + Prefix.size();
    const auto Member = Code.substr(J);
    std::size_t Stem = Member.starts_with("var_m") ? 5
                       : Member.starts_with("gap") ? 3
                                                   : 0;
    if (!Stem)
      continue;
    J += Stem;
    const std::size_t Digits = J;
    while (J < Code.size() && upperHex(Code[J]))
      ++J;
    if (J == Digits || (J < Code.size() && wordByte(Code[J])))
      continue;
    Out.append(Code.substr(Last, I - Last));
    Out += Replace(Code.substr(I + Prefix.size(), J - I - Prefix.size()));
    Last = J;
    I = J - 1;
    if (Changed)
      *Changed = true;
  }
  Out.append(Code.substr(Last));
  return Out;
}

} // namespace

ReadingView readingView(std::string_view Text) {
  const auto Taken = identifierSet(Text);
  std::map<std::string_view, std::string_view, std::less<>> Short;
  for (const auto &Type : TypeNames)
    if (!Taken.count(std::string(Type.Short)))
      Short.emplace(Type.Long, Type.Short);

  // A frame member's bare name must not also name something else.
  const auto Elsewhere = identifierSet(replaceFrameMembers(
      replaceFrames(Text, [](const FrameBlock &) { return std::string(); }),
      [](std::string_view) { return std::string(); }));
  bool FrameNamesFree = true;
  for (const auto &Name : Elsewhere)
    if (Name.starts_with("var_m") || Name.starts_with("gap"))
      FrameNamesFree = false;

  // Typed accesses into byte-array globals, by address.
  std::map<std::uint64_t, std::set<std::string>> Types;
  (void)outsideLiterals(Text, [&](std::string_view Code) {
    accesses(Code, [&](std::size_t, const Access &Found) {
      Types[Found.Address].insert(Found.Type);
    });
    return std::string(Code);
  });
  std::map<std::uint64_t, std::string> Named;
  for (const auto &[Address, Kinds] : Types)
    if (Kinds.size() == 1 && !Taken.count(globalName(Address)))
      Named.emplace(Address, *Kinds.begin());
  std::string View = outsideLiterals(Text, [&](std::string_view Code) {
    std::string Out;
    std::size_t Last = 0;
    accesses(Code, [&](std::size_t At, const Access &Found) {
      if (!Named.count(Found.Address))
        return;
      Out.append(Code.substr(Last, At - Last));
      Out += globalName(Found.Address);
      Last = At + Found.Length;
    });
    Out.append(Code.substr(Last));
    return Out;
  });

  View = mergeDeclarations(foldFrames(View));
  bool UsedTypes = false, UsedFrame = false;
  View = outsideLiterals(View, [&](std::string_view Code) {
    const auto Bare =
        FrameNamesFree
            ? replaceFrameMembers(
                  Code, [](std::string_view Name) { return std::string(Name); },
                  &UsedFrame)
            : std::string(Code);
    return replaceIdentifiers(Bare, Short, &UsedTypes);
  });

  // The globals' types, spelled as the view spells types, by type in order
  // of first address.
  std::vector<std::pair<std::string, std::vector<std::string>>> ByType;
  for (const auto &[Address, Declared] : Named) {
    const auto Type = replaceIdentifiers(Declared, Short, &UsedTypes);
    auto It =
        std::find_if(ByType.begin(), ByType.end(),
                     [&](const auto &Group) { return Group.first == Type; });
    if (It == ByType.end())
      It = ByType.insert(ByType.end(), {Type, {}});
    It->second.push_back(globalName(Address));
  }
  std::vector<std::string> Notes;
  if (UsedTypes)
    Notes.push_back(std::string("iN/uN: intN_t/uintN_t") +
                    (Short.count("uintptr_t") ? ", uptr: uintptr_t" : ""));
  if (UsedFrame)
    Notes.push_back("var_mN/gapN: stack_storage members");
  if (!ByType.empty()) {
    std::string Globals = "globals: ";
    for (std::size_t I = 0; I < ByType.size(); ++I) {
      if (I)
        Globals += "; ";
      Globals += ByType[I].first + ' ';
      for (std::size_t J = 0; J < ByType[I].second.size(); ++J)
        Globals += (J ? ", " : "") + ByType[I].second[J];
    }
    Notes.push_back(std::move(Globals));
  }
  std::string Legend;
  for (std::size_t I = 0; I < Notes.size(); ++I)
    Legend += (I ? "; " : "") + Notes[I];
  return {std::move(View), std::move(Legend)};
}

} // namespace neverd::binmcp
