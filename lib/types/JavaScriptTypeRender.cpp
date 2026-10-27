//===- JavaScriptTypeRender.cpp - Local types as JavaScript ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// JavaScript reads C's types through a DataView: a class for each record,
// made on the bytes, the offset the record begins at and their byte order,
// little-endian unless the caller says, with an accessor for each member at
// the offset the library records, as ViewPlan plans them.  Integers of up to
// four bytes read as numbers and wider ones as bigints, as DataView reads
// them, and a pointer reads as the address it holds.  An array's elements
// read through a method that takes their index, and a bitfield through
// accessors whose arithmetic follows C.  JSDoc types every accessor, so that
// TypeScript checks the module too.
//
//===----------------------------------------------------------------------===//

#include "ViewPlan.h"

#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

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

/// The JavaScript type \p Value reads as, whatever the program names it.
std::string plain(const ViewValue &Value) {
  switch (Value.TheForm) {
  case ViewValue::Form::Integer:
    return Value.Bytes <= 4 ? "number" : "bigint";
  case ViewValue::Form::Bool:
    return "boolean";
  case ViewValue::Form::Float:
    return "number";
  case ViewValue::Form::Bytes:
    return "Uint8Array";
  }
  return "number";
}

/// What a class's accessors touch, for the module's checks.
struct ClassState {
  /// Statements that read each accessor and write it back.
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
      : Library(Library), Options(Options),
        Plan(Library, Options, Language::JavaScript, {"constructor"}) {}

  /// \p Type alone, for reading.
  void alone(const LocalType &Type, std::string &Out);
  /// \p Types as a module that runs.
  void module(llvm::ArrayRef<const LocalType *> Types, std::string &Out);

private:
  /// \p Value's JSDoc type, the program's name for it when it has one,
  /// which the module then declares.
  std::string type(const ViewValue &Value) {
    if (Value.Windows)
      Aliased.insert(*Value.Windows);
    return Value.Named.empty() ? plain(Value) : Value.Named;
  }
  std::string read(const ViewValue &Value, const std::string &At) const;
  /// The statement that writes \p What at \p At, or empty for bytes, which
  /// write through the Uint8Array the getter gives.
  std::string write(const ViewValue &Value, const std::string &At,
                    const std::string &What) const;

  void enumeration(const ViewEnum &Enum, std::string &Out) const;
  void typedefinition(const LocalType &Type, std::string &Out,
                      std::string &Consts);
  void record(const ViewClass &Class, std::string &Out);
  void accessor(const ViewClass &Class, const ViewAccessor &Accessor,
                ClassState &State, std::string &Out);
  void bits(const ViewClass &Class, const ViewAccessor &Accessor,
            ClassState &State, std::string &Out);
  /// \p Type's classes, a blank line between them, and the enumerations
  /// it defines in place.
  void records(const LocalType &Type, std::string &Enums, std::string &Classes);

  const TypeLibrary &Library;
  const RenderOptions &Options;
  const ViewPlanner Plan;
  std::string Checks;

public:
  std::set<Builtin> Aliased;
  bool Indexes = false;
};

std::string ScriptWriter::read(const ViewValue &Value,
                               const std::string &At) const {
  const std::string Place = "this.#offset + " + At;
  switch (Value.TheForm) {
  case ViewValue::Form::Integer:
    if (Value.Bytes == 16)
      // The halves in the order the bytes hold them.
      return "this.#wide(" + Place + ", " + (Value.Signed ? "true" : "false") +
             ")";
    return "this.#view.get" +
           (Value.Bytes == 8
                ? std::string(Value.Signed ? "BigInt64" : "BigUint64")
                : std::string(Value.Signed ? "Int" : "Uint") +
                      std::to_string(Value.Bytes * 8)) +
           "(" + Place + (Value.Bytes > 1 ? ", this.#little)" : ")");
  case ViewValue::Form::Float:
    return "this.#view.getFloat" + std::to_string(Value.Bytes * 8) + "(" +
           Place + ", this.#little)";
  case ViewValue::Form::Bool:
    return "this.#view.getUint8(" + Place + ") !== 0";
  case ViewValue::Form::Bytes:
    return "new Uint8Array(this.#view.buffer, this.#view.byteOffset + " +
           Place + ", " + std::to_string(Value.Bytes) + ")";
  }
  return "undefined";
}

std::string ScriptWriter::write(const ViewValue &Value, const std::string &At,
                                const std::string &What) const {
  const std::string Place = "this.#offset + " + At;
  switch (Value.TheForm) {
  case ViewValue::Form::Integer:
    if (Value.Bytes == 16)
      return "this.#setWide(" + Place + ", " + What + ");";
    return "this.#view.set" +
           (Value.Bytes == 8
                ? std::string(Value.Signed ? "BigInt64" : "BigUint64")
                : std::string(Value.Signed ? "Int" : "Uint") +
                      std::to_string(Value.Bytes * 8)) +
           "(" + Place + ", " + What +
           (Value.Bytes > 1 ? ", this.#little);" : ");");
  case ViewValue::Form::Float:
    return "this.#view.setFloat" + std::to_string(Value.Bytes * 8) + "(" +
           Place + ", " + What + ", this.#little);";
  case ViewValue::Form::Bool:
    return "this.#view.setUint8(" + Place + ", " + What + " ? 1 : 0);";
  case ViewValue::Form::Bytes:
    return "";
  }
  return "";
}

void ScriptWriter::enumeration(const ViewEnum &Enum, std::string &Out) const {
  const EnumDef &Def = *Enum.Enum;
  const unsigned Bytes = Library.builtinBytes(Def.Underlying);
  const bool Signed = builtinInfo(Def.Underlying).Signed;
  const bool Big = Bytes > 4;
  Out += "/** " + commented(Enum.Doc) + ": an enumeration of " +
         std::to_string(Bytes) + " bytes, which fields hold as " +
         (Big ? "bigints" : "numbers") + ". */\n";
  Out += "export const " + Enum.Name + " = Object.freeze({\n";
  for (size_t I = 0; I < Def.Values.size(); ++I) {
    const Enumerator &Value = Def.Values[I];
    std::string Number;
    if (Signed && static_cast<int64_t>(Value.Value) < 0)
      Number = std::to_string(static_cast<int64_t>(Value.Value));
    else if (Value.Value >= 0x100)
      Number = hex(Bytes < 8 ? Value.Value & ((uint64_t(1) << (Bytes * 8)) - 1)
                             : Value.Value);
    else
      Number = std::to_string(Value.Value);
    Out += "  " + Plan.names().enumerator(Def, I) + ": " + Number +
           (Big ? "n" : "") + ",\n";
  }
  Out += "});\n";
  Out += "/** @typedef {" + std::string(Big ? "bigint" : "number") + "} " +
         Enum.Name + " */\n";
}

void ScriptWriter::typedefinition(const LocalType &Type, std::string &Out,
                                  std::string &Consts) {
  const ViewTypedef Def = Plan.typedefinition(Type);
  switch (Def.TheKind) {
  case ViewTypedef::Kind::Function:
    // No value is a function; a pointer to one holds its address.
    if (!Def.Declared.empty())
      Out += "// " + Def.Name + " is the function type " + Def.Declared +
             "; a pointer to it holds its address.\n";
    return;
  case ViewTypedef::Kind::Record:
  case ViewTypedef::Kind::Enum:
    // A record's or enumeration's other name is its class's too.
    Out += "/** @typedef {" + Def.Named + "} " + Def.Name + " */\n";
    Consts += "export const " + Def.Name + " = " + Def.Target + ";\n";
    return;
  case ViewTypedef::Kind::Array:
    Out += "// " + Def.Name + " is " + Def.Declared +
           ", which a member of it reads by index.\n";
    return;
  case ViewTypedef::Kind::Value:
    Out += "/** @typedef {" + type(Def.Value) + "} " + Def.Name +
           (Def.Pointer ? " The address of " + commented(Def.Declared) + "."
                        : "") +
           " */\n";
    return;
  case ViewTypedef::Kind::Other:
    if (!Def.Declared.empty())
      Out += "// " + Def.Name + " is " + Def.Declared + ".\n";
    return;
  }
}

void ScriptWriter::accessor(const ViewClass &Class,
                            const ViewAccessor &Accessor, ClassState &State,
                            std::string &Out) {
  const std::string &Name = Accessor.Name;
  const std::string Declared = commented(Accessor.Declared);
  const std::string Offset = hex(Accessor.Offset, Class.Digits);
  if (Accessor.TheKind == ViewAccessor::Kind::Unread) {
    Out += "  // " + Declared + ": not a value that reads.\n";
    return;
  }
  const bool Record = Accessor.TheKind == ViewAccessor::Kind::Record;
  const std::string JSType = Record ? Accessor.Class : type(Accessor.Value);

  if (Accessor.Bounds.empty()) {
    if (Record) {
      Out += "  /** " + Declared + ", a view of its bytes @returns {" +
             Accessor.Named + "} */\n";
      Out += "  get " + Name + "() { return new " + Accessor.Class +
             "(this.#view, this.#offset + " + Offset + ", this.#little); }\n";
      State.Spans.push_back("[" + Offset + ", " + Accessor.Class + ".size]");
      return;
    }
    Out += "  /** " + Declared + " @returns {" + JSType + "} */\n";
    Out += "  get " + Name + "() { return " + read(Accessor.Value, Offset) +
           "; }\n";
    const std::string Setter = write(Accessor.Value, Offset, "value");
    if (Setter.empty()) {
      State.Touch.push_back("void r." + Name + ";");
      return;
    }
    Out += "  set " + Name + "(value) { " + Setter + " }\n";
    State.Wide |= Accessor.Value.TheForm == ViewValue::Form::Integer &&
                  Accessor.Value.Bytes == 16;
    State.Touch.push_back("r." + Name + " = r." + Name + ";");
    return;
  }

  // An array: each element by its index, the first index outermost.
  Indexes = true;
  static const char *const Indices[] = {"i", "j", "k"};
  std::vector<std::string> Params;
  for (size_t I = 0; I < Accessor.Bounds.size(); ++I)
    Params.push_back(I < std::size(Indices) ? Indices[I]
                                            : "i" + std::to_string(I));
  std::string Spot = Offset, ParamList, Last;
  bool Flexible = false;
  uint64_t Total = Accessor.Size;
  for (size_t I = 0; I < Accessor.Bounds.size(); ++I) {
    const uint64_t Bound = Accessor.Bounds[I];
    Spot += " + " + std::to_string(Accessor.Strides[I]) + " * index(" +
            Params[I] + ", " + (Bound ? std::to_string(Bound) : "Infinity") +
            ")";
    ParamList += (I ? ", " : "") + Params[I];
    Last += (I ? ", " : "") + std::to_string(Bound ? Bound - 1 : uint64_t(0));
    Flexible |= !Bound;
    Total *= Bound;
  }
  std::string Doc = "  /**\n   * " + Declared +
                    (Record ? ", each a view of its bytes" : "") + "\n";
  for (const std::string &Param : Params)
    Doc += "   * @param {number} " + Param + "\n";
  if (Record) {
    Out += Doc + "   * @returns {" + Accessor.Named + "}\n   */\n";
    Out += "  " + Name + "(" + ParamList + ") { return new " + Accessor.Class +
           "(this.#view, this.#offset + " + Spot + ", this.#little); }\n";
    State.Spans.push_back("[" + Offset + ", " + std::to_string(Total) + "]");
    return;
  }
  Out += Doc + "   * @returns {" + JSType + "}\n   */\n";
  Out += "  " + Name + "(" + ParamList + ") { return " +
         read(Accessor.Value, Spot) + "; }\n";
  State.Wide |= Accessor.Value.TheForm == ViewValue::Form::Integer &&
                Accessor.Value.Bytes == 16;
  const std::string Setter = write(Accessor.Value, Spot, "value");
  if (Setter.empty()) {
    if (!Flexible)
      State.Touch.push_back("void r." + Name + "(" + Last + ");");
    return;
  }
  Out += "  /**\n";
  for (const std::string &Param : Params)
    Out += "   * @param {number} " + Param + "\n";
  Out += "   * @param {" + JSType + "} value\n   */\n";
  Out += "  " + Accessor.Setter + "(" + ParamList + ", value) { " + Setter +
         " }\n";
  if (!Flexible)
    State.Touch.push_back("r." + Accessor.Setter + "(" + Last + ", r." + Name +
                          "(" + Last + "));");
}

void ScriptWriter::bits(const ViewClass &Class, const ViewAccessor &Accessor,
                        ClassState &State, std::string &Out) {
  const std::string &Name = Accessor.Name;
  const std::string Declared = commented(Accessor.Declared);
  const BitAccess &Access = Accessor.Bits;
  const unsigned Bytes = Accessor.UnitBytes;
  const std::string Named = type(Accessor.Value);
  if (Accessor.TheKind == ViewAccessor::Kind::Unread) {
    Out += "  // " + Declared + ": a unit of " + std::to_string(Bytes) +
           " bytes, which DataView reads no way.\n";
    return;
  }
  const bool Big = Bytes == 8;
  // The field's own type, when the program names one that reads as the
  // unit does.
  std::string Type = Big ? "bigint" : "number";
  if (Accessor.Value.TheForm != ViewValue::Form::Bool &&
      Accessor.Value.TheForm != ViewValue::Form::Bytes &&
      (Accessor.Value.TheForm == ViewValue::Form::Integer &&
       Accessor.Value.Bytes == 8) == Big)
    Type = Named;
  const std::string Unit = hex(Accessor.Offset, Class.Digits);
  const std::string Method =
      Big ? "BigUint64" : "Uint" + std::to_string(Bytes * 8);
  const std::string Get = "this.#view.get" + Method + "(this.#offset + " +
                          Unit + (Bytes > 1 ? ", this.#little)" : ")");
  const auto Set = [&](const std::string &What) {
    return "this.#view.set" + Method + "(this.#offset + " + Unit + ", " + What +
           (Bytes > 1 ? ", this.#little);" : ");");
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

void ScriptWriter::record(const ViewClass &Class, std::string &Out) {
  Out += "/** " + commented(Class.Doc) + " */\n";
  Out += "export class " + Class.Name + " {\n";
  Out += "  /** The bytes a " + Class.Name + " takes. */\n";
  Out += "  static size = " + hex(Class.Record->Size) + ";\n";
  Out += "  /** @type {DataView} */\n  #view;\n";
  Out += "  /** @type {number} */\n  #offset;\n";
  Out += "  /** @type {boolean} */\n  #little;\n";
  Out += "  /**\n   * @param {DataView} view The bytes.\n"
         "   * @param {number} [offset] Where the " +
         Class.Name +
         " begins in them.\n"
         "   * @param {boolean} [littleEndian] Their byte order.\n   */\n";
  Out += "  constructor(view, offset = 0, littleEndian = true) {\n"
         "    this.#view = view;\n"
         "    this.#offset = offset;\n"
         "    this.#little = littleEndian;\n"
         "  }\n";
  ClassState State;
  std::string Body;
  for (const ViewAccessor &Accessor : Class.Accessors)
    if (Accessor.TheKind == ViewAccessor::Kind::Bits || Accessor.UnitBytes)
      bits(Class, Accessor, State, Body);
    else
      accessor(Class, Accessor, State, Body);
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
  Checks += "check(" + Class.Name + ", (r) => {";
  if (State.Touch.empty()) {
    Checks += "}";
  } else {
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

void ScriptWriter::records(const LocalType &Type, std::string &Enums,
                           std::string &Classes) {
  std::vector<ViewClass> Planned;
  std::vector<ViewEnum> Defined;
  Plan.record(Type, Planned, Defined);
  for (const ViewEnum &Enum : Defined) {
    enumeration(Enum, Enums);
    Enums += '\n';
  }
  for (size_t I = 0; I < Planned.size(); ++I) {
    if (I)
      Classes += '\n';
    record(Planned[I], Classes);
  }
}

void ScriptWriter::alone(const LocalType &Type, std::string &Out) {
  const std::string Name = Plan.names().type(Type.Name);
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
    std::string Enums, Classes;
    records(Type, Enums, Classes);
    Out += Enums + Classes;
    return;
  }
  case TypeKind::Enum:
    if (Type.Enum)
      enumeration({Name, Type.Enum.get(), Type.Name}, Out);
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
    const std::string Name = Plan.names().type(Type->Name);
    switch (Type->Kind) {
    case TypeKind::Enum:
      if (Type->Enum) {
        enumeration({Name, Type->Enum.get(), Type->Name}, Enums);
        Enums += '\n';
      }
      break;
    case TypeKind::Struct:
    case TypeKind::Union:
      if (!Type->Record)
        break;
      if (!Type->Record->Defined) {
        Classes += "/** " + commented(Type->Name) +
                   ": declared only; its members are unknown. */\n";
        Classes += "export class " + Name + " {}\n\n";
        break;
      }
      records(*Type, Enums, Classes);
      Classes += '\n';
      break;
    case TypeKind::Typedef:
    case TypeKind::Function:
      typedefinition(*Type, Typedefs, Consts);
      break;
    }
  }

  if (!Aliased.empty()) {
    Out += "// The Windows headers' names of builtin types.\n";
    for (Builtin Type : Aliased)
      Out += "/** @typedef {" + plain(*Plan.builtin(Type, /*Spelled=*/false)) +
             "} " + builtinInfo(Type).Windows + " */\n";
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
