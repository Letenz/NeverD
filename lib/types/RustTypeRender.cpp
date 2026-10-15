//===- RustTypeRender.cpp - Local types as Rust ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Rust as windows-rs declares Windows types: #[repr(C)] structures and
// unions, anonymous records as types of their own named <Parent>_0 and
// fields named Anonymous, a run of bitfields as one _bitfield storage field
// with an accessor for each, enumerations as an integer type and constants,
// and nullable function pointers as Option<unsafe extern fn>.
//
//===----------------------------------------------------------------------===//

#include "TypeRenderShared.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::types::render {
namespace {

std::string hex(uint64_t Value) {
  std::string Text;
  llvm::raw_string_ostream(Text) << llvm::format_hex(Value, 0, /*Upper=*/true);
  if (Text.size() > 1)
    Text[1] = 'x';
  return Text;
}

class RustWriter {
public:
  RustWriter(const TypeLibrary &Library, const RenderOptions &Options)
      : Library(Library), Options(Options) {}

  void definition(const LocalType &Type, std::string &Out);

private:
  std::string type(const TypeExpr &Type, bool Pointee = false) const;
  std::string integer(uint64_t Bytes, bool Signed) const;
  std::string builtin(Builtin Type, bool Pointee) const;
  std::string function(const FunctionDef &Function) const;
  std::string abi(CallingConvention Convention) const;
  void record(const RecordPlan &Plan, bool Outer, std::string &Out);
  std::string slotType(const Slot &Place) const;
  void enumeration(const std::string &Name, const EnumDef &Enum,
                   std::string &Out) const;
  void accessors(const RecordPlan &Plan, std::string &Out) const;
  void asserts(const RecordPlan &Plan, std::string &Out) const;
  std::string comment(uint64_t Byte) const {
    return Options.Offsets ? offsetText(Byte, Options, Digits) : "";
  }

  const TypeLibrary &Library;
  const RenderOptions &Options;
  unsigned Digits = 1;
  /// The outer record's offset of the record being written, in bytes.
  uint64_t Base = 0;
  /// The outer record's name.
  std::string Parent;
};

std::string RustWriter::integer(uint64_t Bytes, bool Signed) const {
  if (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8 && Bytes != 16)
    return "[u8; " + std::to_string(Bytes) + "]";
  return std::string(Signed ? "i" : "u") + std::to_string(Bytes * 8);
}

std::string RustWriter::builtin(Builtin Type, bool Pointee) const {
  const BuiltinInfo &Info = builtinInfo(Type);
  const unsigned Bytes = builtinBytes(Type, Library.dataModel());
  switch (Info.Class) {
  case BuiltinClass::Void:
    return Pointee ? "c_void" : "()";
  case BuiltinClass::Bool:
    return "bool";
  case BuiltinClass::Float:
    if (Type == Builtin::Half)
      return "u16";
    if (Bytes == 4)
      return "f32";
    if (Bytes == 8)
      return "f64";
    return "[u8; " + std::to_string(Bytes) + "]";
  case BuiltinClass::Character:
    return integer(Bytes, Type == Builtin::Char);
  case BuiltinClass::Integer:
    return integer(Bytes, Info.Signed);
  }
  return "u8";
}

std::string RustWriter::abi(CallingConvention Convention) const {
  // Windows code calls through "system": stdcall on x86, C on x64.
  if (Convention == CallingConvention::Unspecified &&
      Library.dataModel() == DataModel::LLP64)
    return "system";
  switch (Convention) {
#define NEVERD_CALLING_CONVENTION(Name, C, Rust, IDA)                          \
  case CallingConvention::Name:                                                \
    return Rust;
#include "neverd/types/CallingConventions.def"
  }
  return "C";
}

std::string RustWriter::function(const FunctionDef &Function) const {
  std::string Text = "unsafe extern \"" + abi(Function.Convention) + "\" fn(";
  for (size_t I = 0; I < Function.Params.size(); ++I) {
    if (I)
      Text += ", ";
    const Parameter &Param = Function.Params[I];
    if (!Param.Name.empty())
      Text += escapedName(Language::Rust, Param.Name) + ": ";
    Text += Param.Type ? type(*Param.Type) : "i32";
  }
  if (Function.Variadic)
    Text += Function.Params.empty() ? "..." : ", ...";
  Text += ")";
  if (Function.Return && !(Function.Return->Kind == TypeExprKind::Builtin &&
                           Function.Return->TheBuiltin == Builtin::Void))
    Text += " -> " + type(*Function.Return);
  return Text;
}

std::string RustWriter::type(const TypeExpr &Type, bool Pointee) const {
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    return builtin(Type.TheBuiltin, Pointee);
  case TypeExprKind::Named:
    return Type.Name;
  case TypeExprKind::Pointer: {
    if (!Type.Element)
      return "*mut c_void";
    const TypeExpr &Target = *Type.Element;
    // A function pointer may be null: Option of the fn type.
    if (Target.Kind == TypeExprKind::Function && Target.Function)
      return "Option<" + function(*Target.Function) + ">";
    if (Target.Kind == TypeExprKind::Named) {
      const LocalType *Named = Library.resolve(Target.Name);
      if (Named && Named->Kind == TypeKind::Function)
        return "Option<" + Target.Name + ">";
    }
    return std::string(Target.Const ? "*const " : "*mut ") +
           type(Target, /*Pointee=*/true);
  }
  case TypeExprKind::Array:
    return "[" + (Type.Element ? type(*Type.Element) : std::string("u8")) +
           "; " + std::to_string(Type.Count.value_or(0)) + "]";
  case TypeExprKind::Function:
    return Type.Function ? function(*Type.Function)
                         : "unsafe extern \"C\" fn()";
  case TypeExprKind::Record:
    return "[u8; " +
           std::to_string(Type.Record ? Type.Record->Size : uint64_t(0)) + "]";
  case TypeExprKind::Enum:
    return Type.Enum ? builtin(Type.Enum->Underlying, false) : "i32";
  }
  return "u8";
}

void RustWriter::record(const RecordPlan &Plan, bool Outer, std::string &Out) {
  const RecordDef &Record = *Plan.Record;
  std::string Repr =
      Plan.Pack ? "#[repr(C, packed(" + std::to_string(Plan.Pack) + "))]"
                : "#[repr(C)]";
  if (Outer)
    Out += "/// " + Plan.Name + ": " + hex(Record.Size) + " (" +
           std::to_string(Record.Size) + ") bytes" +
           (Plan.Pack ? ", packed to " + std::to_string(Plan.Pack) : "") +
           ".\n";
  else
    Out += "/// " + Plan.Name + ": the anonymous " +
           (Record.Union ? "union" : "structure") + " at " + Parent + "+" +
           comment(Base) + ", " + hex(Record.Size) + " bytes.\n";
  Out += Repr + "\n#[derive(Clone, Copy)]\n";
  Out += std::string(Record.Union ? "pub union " : "pub struct ") + Plan.Name +
         " {\n";
  uint64_t UnionSize = 0;
  for (const Slot &Place : Plan.Slots) {
    if (Place.PaddingBefore && Options.Offsets)
      Out += "    // " + comment(Base + Place.Byte - Place.PaddingBefore) +
             ": " + std::to_string(Place.PaddingBefore) + " bytes of padding\n";
    switch (Place.TheKind) {
    case Slot::Kind::Filler:
      Out += withComment("    pub " + Place.Name + ": [u8; " +
                             std::to_string(Place.Size) + "],",
                         comment(Base + Place.Byte), Options) +
             "\n";
      break;
    case Slot::Kind::Bitfields: {
      std::string Doc;
      for (const Member *Bit : Place.Bits)
        Doc += (Doc.empty() ? "" : ", ") + Bit->Name + " " +
               bitText(Bit->BitOffset - Bit->StorageBitOffset, Bit->BitWidth);
      Out += "    /// " + Doc + "\n";
      Out +=
          withComment("    pub " + Place.Name + ": u" +
                          std::to_string(
                              bitStorage(Library, *Place.Storage).Bytes * 8) +
                          ",",
                      comment(Base + Place.Byte), Options) +
          "\n";
      break;
    }
    case Slot::Kind::Field: {
      const std::string Type = slotType(Place);
      Out += withComment("    pub " + Place.Name + ": " + Type + ",",
                         comment(Base + Place.Byte), Options) +
             "\n";
      UnionSize =
          std::max(UnionSize, Library.sizeOf(*Place.Field->Type).value_or(0));
      break;
    }
    }
  }
  if (Record.Union && Record.Size > UnionSize && Outer)
    Out += "    pub _size: [u8; " + std::to_string(Record.Size) + "],\n";
  if (Plan.TailFiller)
    Out += withComment("    pub _filler_end: [u8; " +
                           std::to_string(Plan.TailFiller) + "],",
                       comment(Base + Record.Size - Plan.TailFiller), Options) +
           "\n";
  else if (Plan.TailPadding && Options.Offsets)
    Out += "    // " + comment(Base + Record.Size - Plan.TailPadding) + ": " +
           std::to_string(Plan.TailPadding) + " bytes of padding\n";
  Out += "}\n";
  accessors(Plan, Out);
  if (Options.Asserts)
    asserts(Plan, Out);
  // Anonymous types, after their parent, at their own offsets.
  for (const auto &[Name, Enum] : Plan.Enums) {
    Out += '\n';
    enumeration(Name, *Enum, Out);
  }
  const uint64_t Saved = Base;
  for (size_t I = 0; I < Plan.Nested.size(); ++I) {
    uint64_t At = 0;
    for (const Slot &Place : Plan.Slots)
      if (Place.NestedType == Plan.Nested[I].Name)
        At = Place.Byte;
    Base = Saved + At;
    Out += '\n';
    record(Plan.Nested[I], /*Outer=*/false, Out);
  }
  Base = Saved;
}

void RustWriter::accessors(const RecordPlan &Plan, std::string &Out) const {
  std::string Methods;
  llvm::StringSet<> Taken;
  const auto unique = [&](std::string Name) {
    while (!Taken.insert(Name).second)
      Name += "_";
    return Name;
  };
  for (const Slot &Place : Plan.Slots) {
    if (Place.TheKind != Slot::Kind::Bitfields)
      continue;
    for (const Member *Bit : Place.Bits) {
      const BitAccess Access = bitAccess(Library, *Bit);
      const std::string Bits = std::to_string(Access.UnitBits);
      const std::string Unsigned = "u" + Bits;
      const std::string Value = (Access.Signed ? "i" : "u") + Bits;
      const std::string Field = "self." + Place.Name;
      std::string Read;
      if (Access.Signed) {
        // Up to the top, then back down: C extends a signed field's sign.
        const uint64_t Up = Access.UnitBits - Access.Shift - Access.Width;
        const uint64_t Down = Access.UnitBits - Access.Width;
        Read = Field;
        if (Up)
          Read = "(" + Read + " << " + std::to_string(Up) + ")";
        Read += " as " + Value;
        if (Down)
          Read = "(" + Read + ") >> " + std::to_string(Down);
      } else {
        Read = Access.Shift
                   ? "(" + Field + " >> " + std::to_string(Access.Shift) + ")"
                   : Field;
        if (Access.Width < Access.UnitBits)
          Read += " & " + Access.Mask;
      }
      const std::string Shift =
          Access.Shift ? " << " + std::to_string(Access.Shift) : "";
      const std::string Stored =
          Access.Signed ? "(value as " + Unsigned + ")" : "value";
      Methods += "    /// " + Bit->Name + ", " +
                 bitText(Access.Shift, Access.Width) + " of " + Place.Name +
                 ".\n";
      Methods += "    pub const fn " +
                 unique(escapedName(Language::Rust, Bit->Name)) +
                 "(&self) -> " + Value + " {\n        " + Read + "\n    }\n";
      Methods += "    pub fn " + unique("set_" + Bit->Name) +
                 "(&mut self, value: " + Value + ") {\n        " + Field +
                 " = (" + Field + " & !(" + Access.Mask + Shift + ")) | ((" +
                 Stored + " & " + Access.Mask + ")" + Shift + ");\n    }\n";
    }
  }
  if (!Methods.empty())
    Out += "impl " + Plan.Name + " {\n" + Methods + "}\n";
}

void RustWriter::asserts(const RecordPlan &Plan, std::string &Out) const {
  Out += "const _: () = assert!(core::mem::size_of::<" + Plan.Name +
         ">() == " + hex(Plan.Record->Size) + ");\n";
  if (Plan.Record->Union)
    return;
  for (const Slot &Place : Plan.Slots)
    if (Place.TheKind != Slot::Kind::Filler)
      Out += "const _: () = assert!(core::mem::offset_of!(" + Plan.Name + ", " +
             Place.Name + ") == " + hex(Place.Byte) + ");\n";
}

std::string RustWriter::slotType(const Slot &Place) const {
  if (Place.NestedType.empty())
    return type(*Place.Field->Type);
  std::string Type = Place.NestedType;
  for (auto Bound = Place.Bounds.rbegin(); Bound != Place.Bounds.rend();
       ++Bound)
    Type = "[" + Type + "; " + std::to_string(*Bound) + "]";
  return Type;
}

void RustWriter::enumeration(const std::string &Name, const EnumDef &Enum,
                             std::string &Out) const {
  const std::string Underlying = builtin(Enum.Underlying, false);
  Out += "/// " + Name + ": an enumeration of " + Underlying + ".\n";
  Out += "pub type " + Name + " = " + Underlying + ";\n";
  const bool Signed = builtinInfo(Enum.Underlying).Signed;
  const unsigned Bytes = builtinBytes(Enum.Underlying, Library.dataModel());
  for (const Enumerator &Value : Enum.Values) {
    std::string Number;
    if (Signed && static_cast<int64_t>(Value.Value) < 0)
      Number = std::to_string(static_cast<int64_t>(Value.Value));
    else if (Value.Value >= 0x100)
      Number = hex(Bytes < 8 ? Value.Value & ((uint64_t(1) << (Bytes * 8)) - 1)
                             : Value.Value);
    else
      Number = std::to_string(Value.Value);
    Out += "pub const " + escapedName(Language::Rust, Value.Name) + ": " +
           Name + " = " + Number + ";\n";
  }
}

void RustWriter::definition(const LocalType &Type, std::string &Out) {
  switch (Type.Kind) {
  case TypeKind::Struct:
  case TypeKind::Union: {
    if (!Type.Record)
      return;
    if (!Type.Record->Defined) {
      Out += "/// " + Type.Name + ": declared only; its members are unknown.\n";
      Out += "#[repr(C)]\npub struct " + Type.Name +
             " {\n    _opaque: [u8; 0],\n}\n";
      return;
    }
    Digits = offsetDigits(Type.Record->Size);
    Base = 0;
    Parent = Type.Name;
    record(planRecord(Library, *Type.Record, Type.Name, Type.Record->Pack,
                      Language::Rust),
           /*Outer=*/true, Out);
    return;
  }
  case TypeKind::Enum:
    if (Type.Enum)
      enumeration(Type.Name, *Type.Enum, Out);
    return;
  case TypeKind::Typedef:
    if (Type.Target)
      Out += "pub type " + Type.Name + " = " + type(*Type.Target) + ";\n";
    return;
  case TypeKind::Function:
    if (Type.Function)
      Out += "pub type " + Type.Name + " = " + function(*Type.Function) + ";\n";
    return;
  }
}

} // namespace

std::string renderRust(const TypeLibrary &Library, const LocalType &Type,
                       const RenderOptions &Options) {
  RustWriter Writer(Library, Options);
  std::string Out;
  Writer.definition(Type, Out);
  return Out;
}

std::string renderRustModule(const TypeLibrary &Library,
                             llvm::ArrayRef<const LocalType *> Roots,
                             const RenderOptions &Options) {
  const std::vector<const LocalType *> Types =
      neededTypes(Library, Roots, /*ThroughPointers=*/true);
  std::string Out = "// " + std::to_string(Types.size()) +
                    " types, rendered by NeverD.\n"
                    "#![allow(non_camel_case_types, non_snake_case, "
                    "non_upper_case_globals, dead_code)]\n\n"
                    "use core::ffi::c_void;\n";
  RustWriter Writer(Library, Options);
  for (const LocalType *Type : Types) {
    Out += '\n';
    Writer.definition(*Type, Out);
  }
  return Out;
}

} // namespace neverd::types::render
