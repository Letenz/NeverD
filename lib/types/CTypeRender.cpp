//===- CTypeRender.cpp - Local types as C and C++ -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// C and C++ keep the library's own shape: anonymous unions and structures
// in place, bitfields as bitfields, and #pragma pack where the record was
// packed.  A member the language would place below its recorded offset gets
// an explicit filler before it; padding the compiler adds by itself is a
// comment.
//
//===----------------------------------------------------------------------===//

#include "TypeRenderShared.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

namespace neverd::types::render {
namespace {

std::string hex(uint64_t Value) {
  std::string Text;
  llvm::raw_string_ostream(Text) << llvm::format_hex(Value, 0, /*Upper=*/true);
  // format_hex spells the prefix in the case of the digits.
  if (Text.size() > 1)
    Text[1] = 'x';
  return Text;
}

const char *conventionSpelling(CallingConvention Convention) {
  switch (Convention) {
#define NEVERD_CALLING_CONVENTION(Name, C, Rust, IDA, Wide)                    \
  case CallingConvention::Name:                                                \
    return C;
#include "neverd/types/CallingConventions.def"
  }
  return "";
}

/// A builtin as exact-width C spells it.
std::string portableBuiltin(Builtin Type, DataModel Model) {
  const BuiltinInfo &Info = builtinInfo(Type);
  const unsigned Bytes = builtinBytes(Type, Model);
  switch (Info.Class) {
  case BuiltinClass::Void:
    return "void";
  case BuiltinClass::Bool:
    return "bool";
  case BuiltinClass::Float:
    if (Type == Builtin::Half)
      return "_Float16";
    if (Bytes == 4)
      return "float";
    return Bytes == 8 ? "double" : "long double";
  case BuiltinClass::Character:
    if (Type == Builtin::Char)
      return "char";
    return "uint" + std::to_string(Bytes * 8) + "_t";
  case BuiltinClass::Integer:
    if (Bytes == 16)
      return Info.Signed ? "__int128" : "unsigned __int128";
    return std::string(Info.Signed ? "int" : "uint") +
           std::to_string(Bytes * 8) + "_t";
  }
  return "void";
}

class CWriter {
public:
  CWriter(const TypeLibrary &Library, const RenderOptions &Options)
      : Library(Library), Options(Options), Cpp(Options.Lang == Language::Cpp),
        Names(Library, Options.Lang) {}

  std::string builtin(Builtin Type) const;
  std::string declare(const TypeExpr &Type, std::string Declarator) const;

  void definition(const LocalType &Type, std::string &Out);
  void forwardDeclaration(const LocalType &Type, std::string &Out) const;
  /// The builtins rendered so far.
  const std::set<Builtin> &builtins() const { return UsedBuiltins; }
  /// Whether a record rendered so far declares its alignment.
  bool alignsRecords() const { return AlignsRecords; }

private:
  void record(const LocalType &Type, std::string &Out);
  void enumeration(const LocalType &Type, std::string &Out);
  void asserts(const LocalType &Type, std::string &Out);
  void body(const RecordDef &Record, uint32_t Pack, unsigned Indent,
            uint64_t Base, bool Outer, std::string &Out);
  std::string qualifiers(const TypeExpr &Type) const;
  std::string enumHead(const EnumDef &Enum);
  void enumerators(const EnumDef &Enum, unsigned Indent, std::string &Out);
  void step(const SequentialLayout::Step &Step, uint64_t At, unsigned Indent,
            std::string &Out);
  void line(unsigned Indent, llvm::StringRef Code, llvm::StringRef Comment,
            std::string &Out) const;
  std::string tagged(const LocalType &Type) const;
  /// "struct" or "union", with the alignment \p Record declares.
  std::string keyword(const RecordDef &Record);
  std::string offset(uint64_t Byte) const {
    return offsetText(Byte, Options, Digits);
  }
  /// Every named member reachable as Type.Name, with its byte offset.
  void hoisted(const RecordDef &Record, uint64_t Base,
               std::vector<std::pair<std::string, uint64_t>> &Out) const;

  const TypeLibrary &Library;
  const RenderOptions &Options;
  const bool Cpp;
  const NameSpelling Names;
  unsigned Digits = 1;
  unsigned Fillers = 0;
  mutable std::set<Builtin> UsedBuiltins;
  bool AlignsRecords = false;
};

std::string CWriter::builtin(Builtin Type) const {
  UsedBuiltins.insert(Type);
  const BuiltinInfo &Info = builtinInfo(Type);
  switch (Options.Spelling) {
  case BuiltinSpelling::Windows:
    return Info.Windows;
  case BuiltinSpelling::IDA:
    return Info.IDA;
  case BuiltinSpelling::Keywords:
    return Info.Keywords;
  case BuiltinSpelling::Portable:
    return portableBuiltin(Type, Library.dataModel());
  }
  return Info.Keywords;
}

std::string CWriter::declare(const TypeExpr &Type,
                             std::string Declarator) const {
  const auto spaced = [&](std::string Base) {
    return Declarator.empty() ? Base : Base + " " + Declarator;
  };
  std::string Qualifiers;
  if (Type.Const)
    Qualifiers += "const ";
  if (Type.Volatile)
    Qualifiers += "volatile ";
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    return spaced(Qualifiers + builtin(Type.TheBuiltin));
  case TypeExprKind::Named: {
    std::string Base = Qualifiers;
    if (!Cpp) {
      if (Type.NamedKind == TypeKind::Struct)
        Base += "struct ";
      else if (Type.NamedKind == TypeKind::Union)
        Base += "union ";
      else if (Type.NamedKind == TypeKind::Enum)
        Base += "enum ";
    }
    return spaced(Base + Names.type(Type.Name));
  }
  case TypeExprKind::Pointer: {
    std::string Inner = Type.Reference ? "&" : "*";
    if (Type.Const)
      Inner += "const";
    if (Type.Volatile)
      Inner += Type.Const ? " volatile" : "volatile";
    if (!Declarator.empty())
      Inner += (Type.Const || Type.Volatile ? " " : "") + Declarator;
    if (!Type.Element)
      return builtin(Builtin::Void) + " " + Inner;
    const TypeExpr &Pointee = *Type.Element;
    // A pointer to an array or a function binds before them.
    if (Pointee.Kind == TypeExprKind::Array ||
        Pointee.Kind == TypeExprKind::Function) {
      if (Pointee.Kind == TypeExprKind::Function && Pointee.Function) {
        const std::string Convention =
            conventionSpelling(Pointee.Function->Convention);
        if (!Convention.empty())
          Inner = Convention + " " + Inner;
      }
      Inner = "(" + Inner + ")";
    }
    return declare(Pointee, Inner);
  }
  case TypeExprKind::Array: {
    const std::string Bound = Type.Count ? std::to_string(*Type.Count) : "";
    if (!Type.Element)
      return spaced(builtin(Builtin::UChar)) + "[" + Bound + "]";
    return declare(*Type.Element, Declarator + "[" + Bound + "]");
  }
  case TypeExprKind::Function: {
    if (!Type.Function)
      return spaced(builtin(Builtin::Void));
    const FunctionDef &Function = *Type.Function;
    std::string Params;
    for (const Parameter &Param : Function.Params) {
      if (!Params.empty())
        Params += ", ";
      Params += Param.Type ? declare(*Param.Type, Names.member(Param.Name))
                           : builtin(Builtin::Int);
    }
    if (Function.Variadic)
      Params += Params.empty() ? "..." : ", ...";
    else if (Params.empty() && !Cpp)
      Params = "void";
    std::string Inner = Declarator;
    // A function named directly takes its convention before its name.
    if (!Inner.empty() && Inner.front() != '(') {
      const std::string Convention = conventionSpelling(Function.Convention);
      if (!Convention.empty())
        Inner = Convention + " " + Inner;
    }
    Inner += "(" + Params + ")";
    if (!Function.Return)
      return builtin(Builtin::Void) + " " + Inner;
    return declare(*Function.Return, Inner);
  }
  case TypeExprKind::Record:
    return spaced(Qualifiers +
                  (Type.Record && Type.Record->Union ? "union" : "struct") +
                  " /* anonymous */");
  case TypeExprKind::Enum:
    return spaced(Qualifiers + "enum /* anonymous */");
  }
  return spaced(builtin(Builtin::Void));
}

void CWriter::line(unsigned Indent, llvm::StringRef Code,
                   llvm::StringRef Comment, std::string &Out) const {
  std::string Text(Indent, ' ');
  Text += Code;
  if (Options.Offsets)
    Text = withComment(std::move(Text), Comment, Options);
  Out += Text;
  Out += '\n';
}

std::string CWriter::keyword(const RecordDef &Record) {
  std::string Text = Record.Union ? "union" : "struct";
  if (!Record.Alignment)
    return Text;
  // As the Windows headers declare it; C++ has a keyword.
  const std::string Bytes = std::to_string(Record.Alignment);
  if (Cpp)
    return Text + " alignas(" + Bytes + ")";
  AlignsRecords = true;
  return Text + " DECLSPEC_ALIGN(" + Bytes + ")";
}

std::string CWriter::tagged(const LocalType &Type) const {
  const std::string Name = Names.type(Type.Name);
  if (Cpp)
    return Name;
  switch (Type.Kind) {
  case TypeKind::Struct:
    return "struct " + Name;
  case TypeKind::Union:
    return "union " + Name;
  case TypeKind::Enum:
    return "enum " + Name;
  default:
    return Name;
  }
}

void CWriter::step(const SequentialLayout::Step &Step, uint64_t At,
                   unsigned Indent, std::string &Out) {
  if (Step.Filler) {
    const std::string Name = "__filler" + std::to_string(Fillers++);
    line(Indent,
         builtin(Builtin::UChar) + " " + Name + "[" +
             std::to_string(Step.Filler) + "];",
         offset(At) + ": " + std::to_string(Step.Filler) + " bytes moved", Out);
  } else if (Step.Padding && Options.Offsets) {
    std::string Text(Indent, ' ');
    Text += "// " + offset(At) + ": " + std::to_string(Step.Padding) +
            " bytes of padding";
    Out += Text;
    Out += '\n';
  }
}

std::string CWriter::qualifiers(const TypeExpr &Type) const {
  std::string Text;
  if (Type.Const)
    Text += "const ";
  if (Type.Volatile)
    Text += "volatile ";
  return Text;
}

std::string CWriter::enumHead(const EnumDef &Enum) {
  // C23 and C++ state an underlying type other than int, which C has
  // allowed as an extension before.
  if (Enum.Underlying == Builtin::Int)
    return "enum";
  return "enum : " + builtin(Enum.Underlying);
}

void CWriter::enumerators(const EnumDef &Enum, unsigned Indent,
                          std::string &Out) {
  const unsigned Bytes = builtinBytes(Enum.Underlying, Library.dataModel());
  const bool Signed = builtinInfo(Enum.Underlying).Signed;
  bool Hex = Enum.Flags;
  for (const Enumerator &Value : Enum.Values)
    if (!(Signed && static_cast<int64_t>(Value.Value) < 0) &&
        Value.Value >= 0x100)
      Hex = true;
  for (size_t I = 0; I < Enum.Values.size(); ++I) {
    const Enumerator &Value = Enum.Values[I];
    const int64_t AsSigned = static_cast<int64_t>(Value.Value);
    std::string Number;
    if (Signed && AsSigned < 0)
      Number = std::to_string(AsSigned);
    else if (Hex)
      Number = hex(Bytes < 8 ? Value.Value & ((uint64_t(1) << (Bytes * 8)) - 1)
                             : Value.Value);
    else
      Number = std::to_string(Value.Value);
    Out += std::string(Indent, ' ') + Names.enumerator(Enum, I) + " = " +
           Number + ",\n";
  }
}

void CWriter::body(const RecordDef &Record, uint32_t Pack, unsigned Indent,
                   uint64_t Base, bool Outer, std::string &Out) {
  SequentialLayout Layout(Pack);
  // The bitfield run being written: its storage unit, its size in bits and
  // the bit after its last field.
  struct Run {
    uint64_t Storage = 0;
    uint64_t Bits = 0;
    uint64_t Next = 0;
    std::string Type;
  };
  std::optional<Run> Bits;
  uint64_t UnionSize = 0;
  uint32_t UnionAlign = 1;
  for (const Member &Field : Record.Members) {
    if (!Field.Type)
      continue;
    const uint64_t Bit = Base + Field.BitOffset;
    const uint64_t Size = Library.sizeOf(*Field.Type).value_or(0);
    const uint32_t Align = Library.alignOf(*Field.Type);
    const std::string Name = Names.member(Field.Name);
    if (Record.Union) {
      UnionSize = std::max(UnionSize, Size);
      UnionAlign = std::max(UnionAlign, Align);
    } else if (Field.isBitfield()) {
      const uint64_t Storage = Field.StorageBitOffset;
      if (!Bits || Bits->Storage != Storage) {
        const std::string Type = declare(*Field.Type, "");
        // A run before this one with room left is closed by an unnamed
        // bitfield of width 0: MSVC would go on filling it with a field of
        // its size, and GCC with any field that fits.
        if (Bits && Bits->Next < Bits->Bits)
          line(Indent, Bits->Type + " : 0;", "", Out);
        if (Storage % 8 == 0) {
          const uint64_t At = Layout.end();
          step(Layout.place(Storage / 8, Size, Align), Base / 8 + At, Indent,
               Out);
        }
        Bits = Run{Storage, Size * 8, 0, Type};
      }
      // Bits the record leaves unnamed before this field.
      const uint64_t Position = Field.BitOffset - Storage;
      if (Position > Bits->Next)
        line(Indent,
             Bits->Type + " : " + std::to_string(Position - Bits->Next) + ";",
             offset((Base + Storage) / 8) + " " +
                 bitText(Bits->Next, Position - Bits->Next) + " unnamed",
             Out);
      Bits->Next = Position + Field.BitWidth;
    } else if (Field.BitOffset % 8 == 0) {
      Bits.reset();
      const uint64_t At = Layout.end();
      step(Layout.place(Field.BitOffset / 8, Size, Align), Base / 8 + At,
           Indent, Out);
    }

    // An anonymous type is defined in place, an array of it too.
    const TypeExpr *Element = Field.Type.get();
    std::string Bounds;
    while (Element->Kind == TypeExprKind::Array && Element->Element) {
      Bounds +=
          "[" + (Element->Count ? std::to_string(*Element->Count) : "") + "]";
      Element = Element->Element.get();
    }
    if (Element->Kind == TypeExprKind::Record && Element->Record) {
      const RecordDef &Inner = *Element->Record;
      const uint32_t InnerPack = Inner.Pack ? Inner.Pack : Pack;
      // A pack of its own goes around it; the compiler takes the pack in
      // force where a record begins.
      if (InnerPack != Pack)
        Out += "#pragma pack(push, " + std::to_string(InnerPack) + ")\n";
      // An anonymous record shows its offset where it opens.
      line(Indent, qualifiers(*Element) + keyword(Inner),
           Field.Name.empty() ? offset(Bit / 8) : "", Out);
      line(Indent, "{", "", Out);
      body(Inner, InnerPack, Indent + 4, Bit, /*Outer=*/false, Out);
      if (Field.Name.empty())
        line(Indent, "};", "", Out);
      else
        line(Indent, "} " + Name + Bounds + ";", offset(Bit / 8), Out);
      if (InnerPack != Pack)
        Out += "#pragma pack(pop)\n";
      continue;
    }
    if (Element->Kind == TypeExprKind::Enum && Element->Enum) {
      line(Indent, qualifiers(*Element) + enumHead(*Element->Enum), "", Out);
      line(Indent, "{", "", Out);
      enumerators(*Element->Enum, Indent + 4, Out);
      line(Indent, "} " + Name + Bounds + ";", offset(Bit / 8), Out);
      continue;
    }
    std::string Code = declare(*Field.Type, Name);
    std::string Comment;
    if (Field.isBitfield()) {
      Code += " : " + std::to_string(Field.BitWidth);
      const uint64_t Storage = Base + Field.StorageBitOffset;
      Comment =
          offset(Storage / 8) + " " + bitText(Bit - Storage, Field.BitWidth);
    } else {
      Comment = offset(Bit / 8);
    }
    line(Indent, Code + ";", Comment, Out);
  }
  if (Record.Union) {
    // A union larger than its alternatives keeps its size with one more, as
    // a union of empty arrays, which only Microsoft's C sizes.  A union in
    // place names it apart: its members join the record's.
    if (Record.Size > alignTo(UnionSize, UnionAlign))
      line(Indent,
           builtin(Builtin::UChar) + " __size" +
               (Outer ? std::string() : std::to_string(Fillers++)) + "[" +
               std::to_string(Record.Size) + "];",
           offset(Base / 8), Out);
    return;
  }
  if (!Outer)
    return;
  const uint64_t At = Layout.end();
  step(Layout.finish(Record.Size, Library.alignOf(Record)), Base / 8 + At,
       Indent, Out);
}

void CWriter::record(const LocalType &Type, std::string &Out) {
  const RecordDef &Record = *Type.Record;
  const char *Keyword = Record.Union ? "union" : "struct";
  if (!Record.Defined) {
    Out += "// Declared only: its members are unknown.\n";
    Out += std::string(Keyword) + " " + Names.type(Type.Name) + ";\n";
    return;
  }
  Digits = offsetDigits(Record.Size);
  Fillers = 0;
  std::string Header = "// " + Type.Name + ": " + hex(Record.Size) + " (" +
                       std::to_string(Record.Size) + ") bytes";
  if (Record.Pack)
    Header += ", packed to " + std::to_string(Record.Pack);
  Out += Header + "\n";
  if (Record.Pack)
    Out += "#pragma pack(push, " + std::to_string(Record.Pack) + ")\n";
  Out += keyword(Record) + " " + Names.type(Type.Name) + "\n{\n";
  body(Record, Record.Pack, 4, 0, /*Outer=*/true, Out);
  Out += "};\n";
  if (Record.Pack)
    Out += "#pragma pack(pop)\n";
  if (Options.Asserts)
    asserts(Type, Out);
}

void CWriter::enumeration(const LocalType &Type, std::string &Out) {
  const EnumDef &Enum = *Type.Enum;
  Out += "// " + Type.Name + ": " +
         std::to_string(builtinBytes(Enum.Underlying, Library.dataModel())) +
         " bytes\n";
  std::string Head = enumHead(Enum);
  Head.insert(4, " " + Names.type(Type.Name));
  Out += Head + "\n{\n";
  enumerators(Enum, 4, Out);
  Out += "};\n";
  if (Options.Asserts)
    asserts(Type, Out);
}

void CWriter::hoisted(
    const RecordDef &Record, uint64_t Base,
    std::vector<std::pair<std::string, uint64_t>> &Out) const {
  for (const Member &Field : Record.Members) {
    if (Field.isAnonymous() && Field.Type->Record) {
      hoisted(*Field.Type->Record, Base + Field.BitOffset, Out);
      continue;
    }
    if (Field.isBitfield() || Field.Name.empty())
      continue;
    Out.push_back({Names.member(Field.Name), (Base + Field.BitOffset) / 8});
  }
}

void CWriter::asserts(const LocalType &Type, std::string &Out) {
  const char *Assert = Cpp ? "static_assert" : "_Static_assert";
  const std::string Name = tagged(Type);
  if (Type.Kind == TypeKind::Enum) {
    Out += std::string(Assert) + "(sizeof(" + Name + ") == " +
           std::to_string(
               builtinBytes(Type.Enum->Underlying, Library.dataModel())) +
           ", \"" + Type.Name + "\");\n";
    return;
  }
  const RecordDef &Record = *Type.Record;
  Out += std::string(Assert) + "(sizeof(" + Name + ") == " + hex(Record.Size) +
         ", \"" + Type.Name + "\");\n";
  std::vector<std::pair<std::string, uint64_t>> Members;
  hoisted(Record, 0, Members);
  llvm::StringSet<> Seen;
  for (const auto &[Member, Byte] : Members)
    if (Seen.insert(Member).second)
      Out += std::string(Assert) + "(offsetof(" + Name + ", " + Member +
             ") == " + hex(Byte) + ", \"" + Type.Name + "." + Member + "\");\n";
}

void CWriter::definition(const LocalType &Type, std::string &Out) {
  switch (Type.Kind) {
  case TypeKind::Struct:
  case TypeKind::Union:
    if (Type.Record)
      record(Type, Out);
    return;
  case TypeKind::Enum:
    if (Type.Enum)
      enumeration(Type, Out);
    return;
  case TypeKind::Typedef:
    if (Type.Target)
      Out += "typedef " + declare(*Type.Target, Names.type(Type.Name)) + ";\n";
    return;
  case TypeKind::Function:
    if (Type.Function)
      Out += "typedef " +
             declare(*makeFunction(Type.Function), Names.type(Type.Name)) +
             ";\n";
    return;
  }
}

void CWriter::forwardDeclaration(const LocalType &Type,
                                 std::string &Out) const {
  if (Type.Kind == TypeKind::Struct)
    Out += "struct " + Names.type(Type.Name) + ";\n";
  else if (Type.Kind == TypeKind::Union)
    Out += "union " + Names.type(Type.Name) + ";\n";
}

/// Typedefs for the builtins whose name is no keyword: HRESULT, which the
/// Windows headers guard as _HRESULT_DEFINED, and in C the character types
/// <uchar.h> would declare.
std::string namedBuiltins(const CWriter &Writer, const TypeLibrary &Library,
                          bool Cpp) {
  std::string Out;
  for (Builtin Type : Writer.builtins()) {
    const std::string Portable = portableBuiltin(Type, Library.dataModel());
    if (Type == Builtin::HResult)
      Out += "\n#ifndef _HRESULT_DEFINED\n#define _HRESULT_DEFINED\ntypedef " +
             Portable + " HRESULT;\n#endif\n";
    else if (!Cpp && (Type == Builtin::Char8 || Type == Builtin::Char16 ||
                      Type == Builtin::Char32))
      Out += "typedef " + Portable + " " + builtinInfo(Type).Keywords + ";\n";
  }
  return Out;
}

/// The declarations the spelling needs ahead of the types.
std::string prelude(const CWriter &Writer, const TypeLibrary &Library,
                    const RenderOptions &Options) {
  const bool Cpp = Options.Lang == Language::Cpp;
  std::string Out;
  // The C headers, which freestanding C++ has too.
  Out += Cpp ? "#include <stddef.h>\n#include <stdint.h>\n"
             : "#include <stdbool.h>\n#include <stddef.h>\n#include "
               "<stdint.h>\n";
  if (Writer.alignsRecords())
    Out += "\n#ifndef DECLSPEC_ALIGN\n#if defined(_MSC_VER)\n#define "
           "DECLSPEC_ALIGN(x) __declspec(align(x))\n#else\n#define "
           "DECLSPEC_ALIGN(x) __attribute__((aligned(x)))\n#endif\n#endif\n";
  switch (Options.Spelling) {
  case BuiltinSpelling::Windows: {
    // The Windows headers declare these; elsewhere they take exact widths.
    Out += "\n#if !defined(_WINDEF_) && !defined(_NTDEF_)\n";
    for (Builtin Type : Writer.builtins()) {
      const BuiltinInfo &Info = builtinInfo(Type);
      const std::string Portable = portableBuiltin(Type, Library.dataModel());
      // A keyword, such as char16_t in C++ or long double, needs no
      // declaring.
      if (Portable == Info.Windows ||
          escapedName(Options.Lang, Info.Windows) != Info.Windows ||
          llvm::StringRef(Info.Windows).contains(' '))
        continue;
      if (Type == Builtin::Void)
        Out += "#define VOID void\n";
      else
        Out += "typedef " + Portable + " " + Info.Windows + ";\n";
    }
    Out += "#endif\n";
    break;
  }
  case BuiltinSpelling::IDA:
    Out += "\n#ifndef _MSC_VER\n#define __int8 char\n#define __int16 "
           "short\n#define __int32 int\n#define __int64 long long\n#endif\n";
    Out += namedBuiltins(Writer, Library, Cpp);
    break;
  case BuiltinSpelling::Keywords:
    if (Writer.builtins().count(Builtin::Long) ||
        Writer.builtins().count(Builtin::ULong))
      Out +=
          std::string(Cpp ? "\nstatic_assert" : "\n_Static_assert") +
          "(sizeof(long) == " +
          std::to_string(builtinBytes(Builtin::Long, Library.dataModel())) +
          ", \"these types need a " +
          std::to_string(builtinBytes(Builtin::Long, Library.dataModel()) * 8) +
          "-bit long\");\n";
    Out += namedBuiltins(Writer, Library, Cpp);
    break;
  case BuiltinSpelling::Portable:
    break;
  }
  return Out;
}

} // namespace

struct CSpeller::Impl {
  Impl(const TypeLibrary &Library, const RenderOptions &Options)
      : Options(Options), Writer(Library, this->Options) {}
  RenderOptions Options;
  CWriter Writer;
};

CSpeller::CSpeller(const TypeLibrary &Library, const RenderOptions &Options)
    : P(std::make_unique<Impl>(Library, Options)) {}

CSpeller::~CSpeller() = default;

std::string CSpeller::declare(const TypeExpr &Type,
                              const std::string &Declarator) const {
  return P->Writer.declare(Type, Declarator);
}

std::string renderC(const TypeLibrary &Library, const LocalType &Type,
                    const RenderOptions &Options) {
  CWriter Writer(Library, Options);
  std::string Out;
  Writer.definition(Type, Out);
  return Out;
}

/// Whether \p Name is a C identifier, as a prototype's name must be.
bool isIdentifier(llvm::StringRef Name) {
  return !Name.empty() && !llvm::isDigit(Name.front()) &&
         llvm::all_of(Name,
                      [](char C) { return llvm::isAlnum(C) || C == '_'; });
}

std::string renderCHeader(const TypeLibrary &Library,
                          llvm::ArrayRef<const LocalType *> Roots,
                          const RenderOptions &Options) {
  // What the roots hold by value is defined; what they only point to is
  // declared, except enumerations and typedefs, which C cannot declare.
  const std::vector<const LocalType *> All =
      neededTypes(Library, Roots, /*ThroughPointers=*/true);
  llvm::StringSet<> Defined;
  if (Roots.empty()) {
    for (const LocalType *Type : All)
      Defined.insert(Type->Name);
  } else {
    std::vector<const LocalType *> Work(Roots.begin(), Roots.end());
    for (size_t I = 0; I < Work.size(); ++I) {
      if (!Defined.insert(Work[I]->Name).second)
        continue;
      std::vector<std::string> Names;
      mentionedTypes(*Work[I], Names);
      std::vector<std::string> ByValue;
      valueDependencies(*Work[I], ByValue);
      const llvm::StringSet<> Value(llvm::from_range, ByValue);
      for (const std::string &Name : Names) {
        const LocalType *Found = Library.find(Name);
        if (!Found)
          continue;
        const bool Record =
            Found->Kind == TypeKind::Struct || Found->Kind == TypeKind::Union;
        if (!Record || Value.contains(Name))
          Work.push_back(Found);
      }
    }
  }

  CWriter Writer(Library, Options);
  std::string Declarations, Enums, Body;
  for (const LocalType *Type : All)
    Writer.forwardDeclaration(*Type, Declarations);
  // Enumerations first, then each type after what it holds by value.
  llvm::StringSet<> Done;
  std::function<void(const LocalType &)> Emit = [&](const LocalType &Type) {
    if (!Defined.contains(Type.Name) || !Done.insert(Type.Name).second)
      return;
    // What it holds by value, and the typedefs and enumerations it names
    // anywhere: C declares neither ahead of its definition.
    std::vector<std::string> Needs;
    valueDependencies(Type, Needs);
    std::vector<std::string> Named;
    mentionedTypes(Type, Named);
    for (const std::string &Name : Named)
      if (const LocalType *Found = Library.find(Name);
          Found && Found->Kind != TypeKind::Struct &&
          Found->Kind != TypeKind::Union)
        Needs.push_back(Name);
    for (const std::string &Name : Needs)
      if (const LocalType *Found = Library.find(Name))
        Emit(*Found);
    std::string &Into = Type.Kind == TypeKind::Enum ? Enums : Body;
    Writer.definition(Type, Into);
    Into += '\n';
  };
  for (const LocalType *Type : All)
    if (Type->Kind == TypeKind::Enum)
      Emit(*Type);
  for (const LocalType *Type : All)
    Emit(*Type);

  // The functions the library knows the prototypes of, by name.
  std::string Prototypes;
  size_t PrototypeCount = 0;
  if (Options.Prototypes && Roots.empty()) {
    std::vector<std::pair<llvm::StringRef, const FunctionDef *>> Sorted;
    for (const auto &Entry : Library.prototypes())
      if (Entry.second && isIdentifier(Entry.first()))
        Sorted.emplace_back(Entry.first(), Entry.second.get());
    llvm::sort(Sorted);
    const bool Cpp = Options.Lang == Language::Cpp;
    for (const auto &[Name, Function] : Sorted) {
      if (Function->NoReturn)
        Prototypes += Cpp ? "[[noreturn]] " : "_Noreturn ";
      Prototypes += Writer.declare(
                        *makeFunction(std::make_shared<FunctionDef>(*Function)),
                        Name.str()) +
                    ";\n";
    }
    PrototypeCount = Sorted.size();
  }

  std::string Out =
      "// " + std::to_string(Done.size()) + " types" +
      (PrototypeCount ? ", " + std::to_string(PrototypeCount) + " prototypes"
                      : std::string()) +
      ", rendered by NeverD.\n#pragma once\n\n";
  Out += prelude(Writer, Library, Options);
  if (!Declarations.empty())
    Out += "\n" + Declarations;
  Out += "\n" + Enums + Body;
  if (!Prototypes.empty())
    Out += "\n" + Prototypes;
  return Out;
}

} // namespace neverd::types::render
