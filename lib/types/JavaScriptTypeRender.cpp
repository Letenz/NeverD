//===- JavaScriptTypeRender.cpp - Local types as JavaScript ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// JavaScript reads C's types through a DataView: a class for each record,
// made on the bytes, the offset the record begins at and their byte order,
// little-endian unless the caller says, with an accessor for each member at
// the offset the library records.  The members of an anonymous record are the
// parent's, as C reaches them; a named member of a record defined in place
// reads through a class of its own, <Parent>_0.  Integers of up to four
// bytes read as numbers and wider ones as bigints, as DataView reads them,
// and a pointer reads as the address it holds.  An array's elements read
// through a method that takes their index, and a bitfield through accessors
// whose arithmetic follows C.  JSDoc types every accessor, so that
// TypeScript checks the module too.
//
//===----------------------------------------------------------------------===//

#include "TypeRenderShared.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <set>

namespace neverd::types::render {
namespace {

std::string hex(uint64_t Value, unsigned Digits = 1) {
  std::string Text;
  llvm::raw_string_ostream(Text)
      << "0x" << llvm::format_hex_no_prefix(Value, Digits, /*Upper=*/true);
  return Text;
}

/// \p Text safe inside a /** */ comment.
std::string commented(std::string Text) {
  for (size_t At = Text.find("*/"); At != std::string::npos;
       At = Text.find("*/", At))
    Text.insert(At + 1, " ");
  return Text;
}

/// How a DataView reads a value, and the JavaScript type it reads as.
struct Scalar {
  enum class Form : uint8_t {
    Number, ///< get<Method>, a number.
    Big,    ///< get<Method>, a bigint.
    Bool,   ///< getUint8, a boolean.
    Wide,   ///< Two 64-bit halves, low first, a bigint.
    Bytes,  ///< The bytes themselves, a Uint8Array over them.
  };
  Form TheForm = Form::Bytes;
  /// DataView's name for it, such as Uint32 or BigInt64.
  std::string Method;
  unsigned Bytes = 0;
  bool Signed = false;
  /// Its JSDoc type: number, bigint, boolean, Uint8Array, or a name.
  std::string Type;
};

/// What a class's accessors take and touch.
struct ClassState {
  llvm::StringSet<> Taken;
  /// Statements that read each accessor and write it back, for the checks.
  std::vector<std::string> Touch;
  /// [offset, bytes] of each record the class holds in place.
  std::vector<std::string> Spans;
  /// Whether an accessor reads an integer of 16 bytes, which the class's
  /// own methods put together from two halves.
  bool Wide = false;
};

class ScriptWriter {
public:
  ScriptWriter(const TypeLibrary &Library, const RenderOptions &Options)
      : Library(Library), Options(Options), Spelling(spellingOf(Options)),
        Names(Library, Language::JavaScript),
        PointerBytes(pointerBytes(Library.dataModel())),
        C(Library, cOptions(Options)) {}

  /// \p Type alone, for reading.
  void alone(const LocalType &Type, std::string &Out);
  /// \p Types as a module that runs.
  void module(llvm::ArrayRef<const LocalType *> Types, std::string &Out);

private:
  static RenderOptions cOptions(const RenderOptions &Options) {
    RenderOptions C = Options;
    C.Lang = Language::C;
    C.Spelling = spellingOf(Options);
    return C;
  }

  /// How \p Type reads; with \p Spelled, its JSDoc type as the spelling
  /// names it.
  std::optional<Scalar> builtin(Builtin Type, bool Spelled = true) const;
  /// How \p Type reads, or nothing for a record, an array or a function.
  std::optional<Scalar> scalar(const TypeExpr &Type) const;
  /// The record \p Type names through typedefs, if it names one.
  const LocalType *namedRecord(const TypeExpr &Type) const;
  /// \p Type with the bounds of the arrays it is, through typedefs too,
  /// outermost first: the element they hold.
  const TypeExpr &element(const TypeExpr &Type,
                          std::vector<uint64_t> &Bounds) const;

  std::string offset(uint64_t Byte) const { return hex(Byte, Digits); }
  /// The offset a comment gives, when comments give them.
  std::string where(uint64_t Byte) const {
    return Options.Offsets ? offsetText(Byte, Options, Digits) + " " : "";
  }
  std::string read(const Scalar &Value, const std::string &At) const;
  /// The statement that writes \p What at \p At, or empty for bytes, which
  /// write through the Uint8Array the getter gives.
  std::string write(const Scalar &Value, const std::string &At,
                    const std::string &What) const;

  void enumeration(const std::string &Name, const EnumDef &Enum,
                   const std::string &Doc, std::string &Out) const;
  void typedefinition(const LocalType &Type, std::string &Out,
                      std::string &Consts) const;
  /// \p Plan's class and, before it, the classes and enumerations of what
  /// it defines in place.
  void records(const RecordPlan &Plan, const std::string &Doc,
               std::string &Enums, std::string &Out);
  void record(const RecordPlan &Plan, const std::string &Doc, std::string &Out);
  void members(const RecordPlan &Plan, uint64_t Base, ClassState &State,
               std::string &Out);
  /// What a record's class says of it first.
  std::string recordDoc(const LocalType &Type) const {
    const RecordDef &Record = *Type.Record;
    return Type.Name + ": " + (Record.Union ? "a union of " : "") +
           hex(Record.Size) + " (" + std::to_string(Record.Size) + ") bytes" +
           (Record.Pack ? ", packed to " + std::to_string(Record.Pack) : "") +
           (Record.Alignment
                ? ", aligned to " + std::to_string(Record.Alignment)
                : "") +
           ".";
  }
  void field(const Slot &Place, uint64_t At, ClassState &State,
             std::string &Out);
  void bitfields(const Slot &Place, uint64_t At, ClassState &State,
                 std::string &Out);
  std::string unique(ClassState &State, std::string Name) const {
    if (Name == "constructor")
      Name += "_";
    while (!State.Taken.insert(Name).second)
      Name += "_";
    return Name;
  }
  static const RecordPlan *nested(const RecordPlan &Plan,
                                  llvm::StringRef Name) {
    for (const RecordPlan &Inner : Plan.Nested)
      if (Inner.Name == Name)
        return &Inner;
    return nullptr;
  }
  /// Whether \p Place's members are its parent's, as an anonymous record's
  /// are in C.
  static bool flattens(const Slot &Place) {
    return Place.TheKind == Slot::Kind::Field && Place.Field->isAnonymous() &&
           Place.Bounds.empty();
  }

  const TypeLibrary &Library;
  const RenderOptions &Options;
  const BuiltinSpelling Spelling;
  const NameSpelling Names;
  const unsigned PointerBytes;
  const CSpeller C;
  unsigned Digits = 1;
  std::string Checks;

public:
  mutable std::set<Builtin> Aliased;
  bool Indexes = false;
};

std::optional<Scalar> ScriptWriter::builtin(Builtin Type, bool Spelled) const {
  const BuiltinInfo &Info = builtinInfo(Type);
  const unsigned Bytes = builtinBytes(Type, Library.dataModel());
  Scalar Value;
  Value.Bytes = Bytes;
  Value.Signed = Info.Signed && Info.Class != BuiltinClass::Bool;
  const auto integer = [&] {
    if (Bytes == 1 || Bytes == 2 || Bytes == 4) {
      Value.TheForm = Scalar::Form::Number;
      Value.Method = std::string(Value.Signed ? "Int" : "Uint") +
                     std::to_string(Bytes * 8);
      Value.Type = "number";
    } else if (Bytes == 8) {
      Value.TheForm = Scalar::Form::Big;
      Value.Method = Value.Signed ? "BigInt64" : "BigUint64";
      Value.Type = "bigint";
    } else if (Bytes == 16) {
      Value.TheForm = Scalar::Form::Wide;
      Value.Type = "bigint";
    } else {
      Value.TheForm = Scalar::Form::Bytes;
      Value.Type = "Uint8Array";
    }
  };
  switch (Info.Class) {
  case BuiltinClass::Void:
    return std::nullopt;
  case BuiltinClass::Bool:
    if (Bytes == 1) {
      Value.TheForm = Scalar::Form::Bool;
      Value.Type = "boolean";
    } else {
      integer();
    }
    break;
  case BuiltinClass::Float:
    // Half reads as its bits, which DataView has no getFloat16 for here.
    if (Type == Builtin::Half) {
      Value.Signed = false;
      integer();
    } else if (Bytes == 4 || Bytes == 8) {
      Value.TheForm = Scalar::Form::Number;
      Value.Method = "Float" + std::to_string(Bytes * 8);
      Value.Type = "number";
    } else {
      Value.TheForm = Scalar::Form::Bytes;
      Value.Type = "Uint8Array";
    }
    break;
  case BuiltinClass::Character:
  case BuiltinClass::Integer:
    integer();
    break;
  }
  // Windows' name, as the module's typedef declares it.
  if (Spelled && Spelling == BuiltinSpelling::Windows && windowsTypedef(Type) &&
      Value.TheForm != Scalar::Form::Bool) {
    Aliased.insert(Type);
    Value.Type = builtinInfo(Type).Windows;
  }
  return Value;
}

const LocalType *ScriptWriter::namedRecord(const TypeExpr &Type) const {
  if (Type.Kind != TypeExprKind::Named)
    return nullptr;
  const LocalType *Named = Library.resolve(Type.Name);
  return Named && Named->Record &&
                 (Named->Kind == TypeKind::Struct ||
                  Named->Kind == TypeKind::Union)
             ? Named
             : nullptr;
}

std::optional<Scalar> ScriptWriter::scalar(const TypeExpr &Type) const {
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    return builtin(Type.TheBuiltin);
  case TypeExprKind::Named: {
    const LocalType *Named = Library.find(Type.Name);
    if (!Named)
      return std::nullopt;
    std::optional<Scalar> Value;
    if (Named->Kind == TypeKind::Enum && Named->Enum)
      Value = builtin(Named->Enum->Underlying);
    else if (Named->Kind == TypeKind::Typedef && Named->Target)
      Value = scalar(*Named->Target);
    // The program's own name, which the module declares; a boolean keeps
    // its JavaScript type.
    if (Value && Value->TheForm != Scalar::Form::Bool)
      Value->Type = Names.type(Type.Name);
    return Value;
  }
  case TypeExprKind::Pointer: {
    // The address a pointer holds.
    Scalar Value;
    Value.Bytes = PointerBytes;
    if (PointerBytes == 8) {
      Value.TheForm = Scalar::Form::Big;
      Value.Method = "BigUint64";
      Value.Type = "bigint";
    } else {
      Value.TheForm = Scalar::Form::Number;
      Value.Method = "Uint" + std::to_string(PointerBytes * 8);
      Value.Type = "number";
    }
    return Value;
  }
  case TypeExprKind::Enum:
    if (Type.Enum)
      return builtin(Type.Enum->Underlying);
    return std::nullopt;
  case TypeExprKind::Array:
  case TypeExprKind::Function:
  case TypeExprKind::Record:
    return std::nullopt;
  }
  return std::nullopt;
}

const TypeExpr &ScriptWriter::element(const TypeExpr &Type,
                                      std::vector<uint64_t> &Bounds) const {
  const TypeExpr *Element = &Type;
  while (true) {
    if (Element->Kind == TypeExprKind::Array && Element->Element) {
      Bounds.push_back(Element->Count.value_or(0));
      Element = Element->Element.get();
      continue;
    }
    // A typedef of an array is an array too.
    if (Element->Kind == TypeExprKind::Named)
      if (const LocalType *Named = Library.find(Element->Name);
          Named && Named->Kind == TypeKind::Typedef && Named->Target) {
        std::vector<uint64_t> More;
        const TypeExpr &Inner = element(*Named->Target, More);
        if (!More.empty()) {
          Bounds.insert(Bounds.end(), More.begin(), More.end());
          Element = &Inner;
          continue;
        }
      }
    return *Element;
  }
}

std::string ScriptWriter::read(const Scalar &Value,
                               const std::string &At) const {
  const std::string Place = "this.#offset + " + At;
  switch (Value.TheForm) {
  case Scalar::Form::Number:
  case Scalar::Form::Big:
    return "this.#view.get" + Value.Method + "(" + Place +
           (Value.Bytes > 1 ? ", this.#little)" : ")");
  case Scalar::Form::Bool:
    return "this.#view.getUint8(" + Place + ") !== 0";
  case Scalar::Form::Wide:
    // The halves in the order the bytes hold them.
    return "this.#wide(" + Place + ", " + (Value.Signed ? "true" : "false") +
           ")";
  case Scalar::Form::Bytes:
    return "new Uint8Array(this.#view.buffer, this.#view.byteOffset + " +
           Place + ", " + std::to_string(Value.Bytes) + ")";
  }
  return "undefined";
}

std::string ScriptWriter::write(const Scalar &Value, const std::string &At,
                                const std::string &What) const {
  const std::string Place = "this.#offset + " + At;
  switch (Value.TheForm) {
  case Scalar::Form::Number:
  case Scalar::Form::Big:
    return "this.#view.set" + Value.Method + "(" + Place + ", " + What +
           (Value.Bytes > 1 ? ", this.#little);" : ");");
  case Scalar::Form::Bool:
    return "this.#view.setUint8(" + Place + ", " + What + " ? 1 : 0);";
  case Scalar::Form::Wide:
    return "this.#setWide(" + Place + ", " + What + ");";
  case Scalar::Form::Bytes:
    return "";
  }
  return "";
}

void ScriptWriter::enumeration(const std::string &Name, const EnumDef &Enum,
                               const std::string &Doc, std::string &Out) const {
  const unsigned Bytes = builtinBytes(Enum.Underlying, Library.dataModel());
  const bool Signed = builtinInfo(Enum.Underlying).Signed;
  const bool Big = Bytes > 4;
  Out += "/** " + commented(Doc) + ": an enumeration of " +
         std::to_string(Bytes) + " bytes, which fields hold as " +
         (Big ? "bigints" : "numbers") + ". */\n";
  Out += "export const " + Name + " = Object.freeze({\n";
  for (size_t I = 0; I < Enum.Values.size(); ++I) {
    const Enumerator &Value = Enum.Values[I];
    std::string Number;
    if (Signed && static_cast<int64_t>(Value.Value) < 0)
      Number = std::to_string(static_cast<int64_t>(Value.Value));
    else if (Value.Value >= 0x100)
      Number = hex(Bytes < 8 ? Value.Value & ((uint64_t(1) << (Bytes * 8)) - 1)
                             : Value.Value);
    else
      Number = std::to_string(Value.Value);
    Out += "  " + Names.enumerator(Enum, I) + ": " + Number + (Big ? "n" : "") +
           ",\n";
  }
  Out += "});\n";
  Out += "/** @typedef {" + std::string(Big ? "bigint" : "number") + "} " +
         Name + " */\n";
}

void ScriptWriter::typedefinition(const LocalType &Type, std::string &Out,
                                  std::string &Consts) const {
  const std::string Name = Names.type(Type.Name);
  if (Type.Kind == TypeKind::Function) {
    // No value is a function; a pointer to one holds its address.
    if (Type.Function)
      Out += "// " + Name + " is the function type " +
             C.declare(*makeFunction(Type.Function)) +
             "; a pointer to it holds its address.\n";
    return;
  }
  if (!Type.Target)
    return;
  const TypeExpr &Target = *Type.Target;
  const std::string Says = C.declare(Target);
  if (const LocalType *Record = namedRecord(Target)) {
    // A record's other name is its class's too.
    Out += "/** @typedef {" + Names.type(Target.Name) + "} " + Name + " */\n";
    Consts += "export const " + Name + " = " + Names.type(Record->Name) + ";\n";
    return;
  }
  if (Target.Kind == TypeExprKind::Named)
    if (const LocalType *Named = Library.resolve(Target.Name);
        Named && Named->Kind == TypeKind::Enum && Named->Enum) {
      Out += "/** @typedef {" + Names.type(Target.Name) + "} " + Name + " */\n";
      Consts +=
          "export const " + Name + " = " + Names.type(Named->Name) + ";\n";
      return;
    }
  std::vector<uint64_t> Bounds;
  element(Target, Bounds);
  if (!Bounds.empty()) {
    Out += "// " + Name + " is " + Says +
           ", which a member of it reads by index.\n";
    return;
  }
  if (const std::optional<Scalar> Value = scalar(Target)) {
    std::string Note;
    if (Target.Kind == TypeExprKind::Pointer)
      Note = " The address of " + commented(Says) + ".";
    Out += "/** @typedef {" + Value->Type + "} " + Name + Note + " */\n";
    return;
  }
  Out += "// " + Name + " is " + Says + ".\n";
}

void ScriptWriter::field(const Slot &Place, uint64_t At, ClassState &State,
                         std::string &Out) {
  const Member &Field = *Place.Field;
  if (Place.Name.empty())
    return;
  const std::string Name = unique(State, Place.Name);
  std::vector<uint64_t> Bounds;
  const TypeExpr &Element = element(*Field.Type, Bounds);
  // What C declares, which the comment gives: a record or enumeration
  // defined in place by the name of its class.
  std::string Declared;
  if (!Place.NestedType.empty()) {
    Declared =
        std::string(Element.Kind == TypeExprKind::Enum        ? "enum"
                    : Element.Record && Element.Record->Union ? "union"
                                                              : "struct") +
        " " + Place.NestedType + " " + Name;
    for (uint64_t Bound : Bounds)
      Declared += "[" + (Bound ? std::to_string(Bound) : std::string()) + "]";
  } else {
    Declared = C.declare(*Field.Type, Field.Name);
  }
  Declared = commented(where(At) + Declared);

  // The class a record in place reads through.
  std::string Class;
  if (Element.Kind == TypeExprKind::Record && !Place.NestedType.empty()) {
    Class = Place.NestedType;
  } else if (const LocalType *Record = namedRecord(Element)) {
    Class = Names.type(Record->Name);
  }
  std::optional<Scalar> Value;
  if (Class.empty()) {
    Value = scalar(Element);
    if (Value && Element.Kind == TypeExprKind::Enum &&
        !Place.NestedType.empty())
      Value->Type = Place.NestedType;
    if (!Value) {
      Out += "  // " + Declared + ": not a value that reads.\n";
      return;
    }
  }
  const uint64_t ElementBytes = Library.sizeOf(Element).value_or(0);
  const std::string JSType = Class.empty() ? Value->Type : Class;

  if (Bounds.empty()) {
    if (!Class.empty()) {
      Out += "  /** " + Declared + ", a view of its bytes @returns {" +
             (Element.Kind == TypeExprKind::Named ? Names.type(Element.Name)
                                                  : Class) +
             "} */\n";
      Out += "  get " + Name + "() { return new " + Class +
             "(this.#view, this.#offset + " + offset(At) +
             ", this.#little); }\n";
      State.Spans.push_back("[" + offset(At) + ", " + Class + ".size]");
      return;
    }
    Out += "  /** " + Declared + " @returns {" + JSType + "} */\n";
    Out +=
        "  get " + Name + "() { return " + read(*Value, offset(At)) + "; }\n";
    const std::string Setter = write(*Value, offset(At), "value");
    if (Setter.empty()) {
      State.Touch.push_back("void r." + Name + ";");
      return;
    }
    Out += "  set " + Name + "(value) { " + Setter + " }\n";
    State.Wide |= Value->TheForm == Scalar::Form::Wide;
    State.Touch.push_back("r." + Name + " = r." + Name + ";");
    return;
  }

  // An array: each element by its index, the first index outermost.
  Indexes = true;
  static const char *const Indices[] = {"i", "j", "k"};
  std::vector<std::string> Params;
  for (size_t I = 0; I < Bounds.size(); ++I)
    Params.push_back(I < std::size(Indices) ? Indices[I]
                                            : "i" + std::to_string(I));
  std::string Spot = offset(At), ParamList, Last;
  bool Flexible = false;
  for (size_t I = 0; I < Bounds.size(); ++I) {
    uint64_t Stride = ElementBytes;
    for (size_t J = I + 1; J < Bounds.size(); ++J)
      Stride *= Bounds[J];
    Spot += " + " + std::to_string(Stride) + " * index(" + Params[I] + ", " +
            (Bounds[I] ? std::to_string(Bounds[I]) : "Infinity") + ")";
    ParamList += (I ? ", " : "") + Params[I];
    Last += (I ? ", " : "") +
            std::to_string(Bounds[I] ? Bounds[I] - 1 : uint64_t(0));
    Flexible |= !Bounds[I];
  }
  uint64_t Total = ElementBytes;
  for (uint64_t Bound : Bounds)
    Total *= Bound;
  std::string Doc = "  /**\n   * " + Declared +
                    (Class.empty() ? "" : ", each a view of its bytes") + "\n";
  for (const std::string &Param : Params)
    Doc += "   * @param {number} " + Param + "\n";
  if (!Class.empty()) {
    Out += Doc + "   * @returns {" +
           (Element.Kind == TypeExprKind::Named ? Names.type(Element.Name)
                                                : Class) +
           "}\n   */\n";
    Out += "  " + Name + "(" + ParamList + ") { return new " + Class +
           "(this.#view, this.#offset + " + Spot + ", this.#little); }\n";
    State.Spans.push_back("[" + offset(At) + ", " + std::to_string(Total) +
                          "]");
    return;
  }
  Out += Doc + "   * @returns {" + JSType + "}\n   */\n";
  Out += "  " + Name + "(" + ParamList + ") { return " + read(*Value, Spot) +
         "; }\n";
  State.Wide |= Value->TheForm == Scalar::Form::Wide;
  const std::string Setter = write(*Value, Spot, "value");
  if (Setter.empty()) {
    if (!Flexible)
      State.Touch.push_back("void r." + Name + "(" + Last + ");");
    return;
  }
  const std::string SetName =
      unique(State, "set" + std::string(1, llvm::toUpper(Name.front())) +
                        Name.substr(1));
  Out += "  /**\n";
  for (const std::string &Param : Params)
    Out += "   * @param {number} " + Param + "\n";
  Out += "   * @param {" + JSType + "} value\n   */\n";
  Out += "  " + SetName + "(" + ParamList + ", value) { " + Setter + " }\n";
  if (!Flexible)
    State.Touch.push_back("r." + SetName + "(" + Last + ", r." + Name + "(" +
                          Last + "));");
}

void ScriptWriter::bitfields(const Slot &Place, uint64_t At, ClassState &State,
                             std::string &Out) {
  const BitStorage Storage = bitStorage(Library, *Place.Storage);
  const std::string Unit = offset(At);
  for (const Member *Bit : Place.Bits) {
    // C's unnamed bitfields pad; nothing reads them.
    if (Bit->Name.empty())
      continue;
    const std::string Name = unique(State, Names.member(Bit->Name));
    const BitAccess Access = bitAccess(Library, *Bit);
    std::optional<Scalar> Value = scalar(*Bit->Type);
    const std::string Declared =
        commented(where(At) + bitText(Access.Shift, Access.Width) + " of " +
                  C.declare(*Bit->Type) + ": " + Bit->Name);
    const unsigned Bytes = Storage.Bytes;
    if (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8) {
      Out += "  // " + Declared + ": a unit of " + std::to_string(Bytes) +
             " bytes, which DataView reads no way.\n";
      continue;
    }
    const bool Big = Bytes == 8;
    std::string Type = Big ? "bigint" : "number";
    if (Value && Value->TheForm != Scalar::Form::Bool &&
        (Value->TheForm == Scalar::Form::Big) == Big)
      Type = Value->Type;
    const std::string Method =
        Big ? "BigUint64" : "Uint" + std::to_string(Bytes * 8);
    const std::string Get = "this.#view.get" + Method + "(this.#offset + " +
                            Unit + (Bytes > 1 ? ", this.#little)" : ")");
    const auto Set = [&](const std::string &What) {
      return "this.#view.set" + Method + "(this.#offset + " + Unit + ", " +
             What + (Bytes > 1 ? ", this.#little);" : ");");
    };
    const unsigned UnitBits = Bytes * 8;
    const uint64_t Mask =
        Access.Width >= 64 ? ~uint64_t(0) : (uint64_t(1) << Access.Width) - 1;
    const uint64_t Shifted = Mask << Access.Shift;
    const uint64_t UnitMask =
        UnitBits >= 64 ? ~uint64_t(0) : (uint64_t(1) << UnitBits) - 1;
    const uint64_t Clear = ~Shifted & UnitMask;
    std::string Reads, Writes;
    if (Big) {
      // Bigints read the unit whole; asIntN extends a signed field's sign.
      const std::string Shift =
          Access.Shift ? " >> " + std::to_string(Access.Shift) + "n" : "";
      Reads = Access.Width == 64 && !Access.Signed
                  ? Get
                  : std::string("BigInt.as") +
                        (Access.Signed ? "IntN(" : "UintN(") +
                        std::to_string(Access.Width) + ", " + Get + Shift + ")";
      Writes = Access.Width == 64
                   ? Set("BigInt.asUintN(64, value)")
                   : "const unit = " + Get + ";\n    " +
                         Set("(unit & " + hex(Clear) + "n) | (BigInt.asUintN(" +
                             std::to_string(Access.Width) + ", value)" +
                             (Access.Shift
                                  ? " << " + std::to_string(Access.Shift) + "n"
                                  : "") +
                             ")");
    } else {
      // Numbers take JavaScript's 32-bit arithmetic: a signed field's sign
      // extends by shifting its top bit to the 32nd and back.
      if (Access.Signed)
        Reads = "(" + Get + " << " +
                std::to_string(32 - Access.Shift - Access.Width) + ") >> " +
                std::to_string(32 - Access.Width);
      else if (Access.Width == UnitBits)
        Reads = Get;
      else if (Access.Shift)
        Reads = "(" + Get + " >>> " + std::to_string(Access.Shift) + ") & " +
                hex(Mask);
      else
        Reads = Get + " & " + hex(Mask);
      Writes =
          Access.Width == UnitBits
              ? Set("value")
              : "const unit = " + Get + ";\n    " +
                    Set("(unit & " + hex(Clear) + ") | (" +
                        (Access.Shift
                             ? "(value << " + std::to_string(Access.Shift) + ")"
                             : std::string("value")) +
                        " & " + hex(Shifted) + ")");
    }
    Out += "  /** " + Declared + " @returns {" + Type + "} */\n";
    Out += "  get " + Name + "() { return " + Reads + "; }\n";
    if (Writes.find('\n') == std::string::npos)
      Out += "  set " + Name + "(value) { " + Writes + " }\n";
    else
      Out += "  set " + Name + "(value) {\n    " + Writes + "\n  }\n";
    State.Touch.push_back("r." + Name + " = r." + Name + ";");
  }
}

void ScriptWriter::members(const RecordPlan &Plan, uint64_t Base,
                           ClassState &State, std::string &Out) {
  for (const Slot &Place : Plan.Slots) {
    const uint64_t At = Base + Place.Byte;
    switch (Place.TheKind) {
    case Slot::Kind::Filler:
      break;
    case Slot::Kind::Bitfields:
      bitfields(Place, At, State, Out);
      break;
    case Slot::Kind::Field:
      if (flattens(Place)) {
        if (const RecordPlan *Inner = nested(Plan, Place.NestedType))
          members(*Inner, At, State, Out);
        break;
      }
      field(Place, At, State, Out);
      break;
    }
  }
}

void ScriptWriter::record(const RecordPlan &Plan, const std::string &Doc,
                          std::string &Out) {
  const RecordDef &Record = *Plan.Record;
  Out += "/** " + commented(Doc) + " */\n";
  Out += "export class " + Plan.Name + " {\n";
  Out += "  /** The bytes a " + Plan.Name + " takes. */\n";
  Out += "  static size = " + hex(Record.Size) + ";\n";
  Out += "  /** @type {DataView} */\n  #view;\n";
  Out += "  /** @type {number} */\n  #offset;\n";
  Out += "  /** @type {boolean} */\n  #little;\n";
  Out += "  /**\n   * @param {DataView} view The bytes.\n"
         "   * @param {number} [offset] Where the " +
         Plan.Name +
         " begins in them.\n"
         "   * @param {boolean} [littleEndian] Their byte order.\n   */\n";
  Out += "  constructor(view, offset = 0, littleEndian = true) {\n"
         "    this.#view = view;\n"
         "    this.#offset = offset;\n"
         "    this.#little = littleEndian;\n"
         "  }\n";
  ClassState State;
  std::string Body;
  members(Plan, 0, State, Body);
  if (State.Wide)
    Out += R"(
  /**
   * The integer of 16 bytes at at, its halves in the bytes' order.
   * @param {number} at
   * @param {boolean} signed
   * @returns {bigint}
   */
  #wide(at, signed) {
    const low = this.#view.getBigUint64(at + (this.#little ? 0 : 8), this.#little);
    const high = this.#view.getBigUint64(at + (this.#little ? 8 : 0), this.#little);
    const value = (high << 64n) | low;
    return signed ? BigInt.asIntN(128, value) : value;
  }
  /**
   * @param {number} at
   * @param {bigint} value
   */
  #setWide(at, value) {
    this.#view.setBigUint64(at + (this.#little ? 0 : 8), BigInt.asUintN(64, value), this.#little);
    this.#view.setBigUint64(at + (this.#little ? 8 : 0), BigInt.asUintN(64, value >> 64n), this.#little);
  }
)";
  if (!Body.empty())
    Out += "\n" + Body;
  Out += "}\n";
  if (!Options.Asserts)
    return;
  Checks += "check(" + Plan.Name + ", (r) => {";
  if (State.Touch.empty())
    Checks += "}";
  else {
    Checks += "\n";
    for (const std::string &Statement : State.Touch)
      Checks += "  " + Statement + "\n";
    Checks += "}";
  }
  if (!State.Spans.empty()) {
    Checks += ", [";
    for (size_t I = 0; I < State.Spans.size(); ++I)
      Checks += (I ? ", " : "") + State.Spans[I];
    Checks += "]";
  }
  Checks += ");\n";
}

void ScriptWriter::records(const RecordPlan &Plan, const std::string &Doc,
                           std::string &Enums, std::string &Out) {
  for (const auto &[Generated, Enum] : Plan.Enums) {
    enumeration(Generated, *Enum, Generated, Enums);
    Enums += '\n';
  }
  // The records it defines in place for a named member are classes of
  // their own; an anonymous one's members are its own, and what that
  // defines in place is its too.
  std::function<void(const RecordPlan &)> inner = [&](const RecordPlan &At) {
    for (const Slot &Place : At.Slots) {
      if (Place.TheKind != Slot::Kind::Field || Place.NestedType.empty())
        continue;
      const RecordPlan *Nested = nested(At, Place.NestedType);
      if (!Nested)
        continue;
      if (flattens(Place)) {
        for (const auto &[Generated, Enum] : Nested->Enums) {
          enumeration(Generated, *Enum, Generated, Enums);
          Enums += '\n';
        }
        inner(*Nested);
        continue;
      }
      records(*Nested,
              Nested->Name + ": the " +
                  (Nested->Record->Union ? "union " : "structure ") +
                  Plan.Name + "." + Place.Name + " holds, " +
                  hex(Nested->Record->Size) + " bytes.",
              Enums, Out);
      Out += '\n';
    }
  };
  inner(Plan);
  record(Plan, Doc, Out);
}

void ScriptWriter::alone(const LocalType &Type, std::string &Out) {
  const std::string Name = Names.type(Type.Name);
  switch (Type.Kind) {
  case TypeKind::Struct:
  case TypeKind::Union: {
    if (!Type.Record)
      return;
    if (!Type.Record->Defined) {
      Out += "/** " + commented(Type.Name) +
             ": declared only; its members are unknown. */\n";
      Out += "export class " + Name + " {}\n";
      return;
    }
    Digits = offsetDigits(Type.Record->Size);
    const RecordPlan Plan =
        planRecord(Library, *Type.Record, Name, Type.Record->Pack, Names);
    std::string Enums, Classes;
    records(Plan, recordDoc(Type), Enums, Classes);
    Out += Enums + Classes;
    return;
  }
  case TypeKind::Enum:
    if (Type.Enum)
      enumeration(Name, *Type.Enum, Type.Name, Out);
    return;
  case TypeKind::Typedef:
  case TypeKind::Function: {
    std::string Consts;
    typedefinition(Type, Out, Consts);
    Out += Consts;
    return;
  }
  }
}

void ScriptWriter::module(llvm::ArrayRef<const LocalType *> Types,
                          std::string &Out) {
  std::string Enums, Classes, Typedefs, Consts;
  for (const LocalType *Type : Types) {
    const std::string Name = Names.type(Type->Name);
    switch (Type->Kind) {
    case TypeKind::Enum:
      if (Type->Enum) {
        enumeration(Name, *Type->Enum, Type->Name, Enums);
        Enums += '\n';
      }
      break;
    case TypeKind::Struct:
    case TypeKind::Union: {
      if (!Type->Record)
        break;
      if (!Type->Record->Defined) {
        Classes += "/** " + commented(Type->Name) +
                   ": declared only; its members are unknown. */\n";
        Classes += "export class " + Name + " {}\n\n";
        break;
      }
      Digits = offsetDigits(Type->Record->Size);
      const RecordPlan Plan =
          planRecord(Library, *Type->Record, Name, Type->Record->Pack, Names);
      records(Plan, recordDoc(*Type), Enums, Classes);
      Classes += '\n';
      break;
    }
    case TypeKind::Typedef:
    case TypeKind::Function:
      typedefinition(*Type, Typedefs, Consts);
      break;
    }
  }

  if (!Aliased.empty()) {
    Out += "// The Windows headers' names of builtin types.\n";
    for (Builtin Type : Aliased)
      Out += "/** @typedef {" + builtin(Type, /*Spelled=*/false)->Type + "} " +
             builtinInfo(Type).Windows + " */\n";
    Out += '\n';
  }
  if (Indexes)
    Out += R"(/**
 * i, when it indexes an array of count elements.
 * @param {number} i
 * @param {number} count
 * @returns {number}
 */
function index(i, count) {
  if (!Number.isInteger(i) || i < 0 || i >= count)
    throw new RangeError(`index ${i} is outside an array of ${count}`);
  return i;
}

)";
  Out += Enums + Classes;
  if (!Typedefs.empty() || !Consts.empty())
    Out += Typedefs + Consts + "\n";
  if (Checks.empty())
    return;
  Out += R"(/**
 * Checks Type against its size: touch reads each accessor and writes it
 * back in a buffer of Type.size, which throws RangeError past its end, and
 * each record held in place, at [offset, bytes] in spans, ends within it.
 * @template T
 * @param {{ size: number, name: string, new (view: DataView): T }} Type
 * @param {(record: T) => void} touch
 * @param {Array<[number, number]>} [spans]
 */
function check(Type, touch, spans = []) {
  touch(new Type(new DataView(new ArrayBuffer(Type.size))));
  for (const [offset, bytes] of spans)
    if (offset + bytes > Type.size)
      throw new RangeError(`${Type.name}: ${bytes} bytes at ${offset} pass its end`);
}

)";
  Out += Checks;
}

} // namespace

std::string renderJavaScript(const TypeLibrary &Library, const LocalType &Type,
                             const RenderOptions &Options) {
  ScriptWriter Writer(Library, Options);
  std::string Out;
  Writer.alone(Type, Out);
  return Out;
}

std::string renderJavaScriptModule(const TypeLibrary &Library,
                                   llvm::ArrayRef<const LocalType *> Roots,
                                   const RenderOptions &Options) {
  const std::vector<const LocalType *> Types =
      neededTypes(Library, Roots, /*ThroughPointers=*/true);
  ScriptWriter Writer(Library, Options);
  std::string Body;
  Writer.module(Types, Body);
  // TypeScript checks the module by its JSDoc.
  return "// @ts-check\n// " + std::to_string(Types.size()) +
         " types, rendered by NeverD: each record reads its bytes through a\n"
         "// DataView, every member at the offset the library records.\n\n" +
         Body;
}

} // namespace neverd::types::render
