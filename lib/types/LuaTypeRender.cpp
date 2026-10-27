//===- LuaTypeRender.cpp - Local types as Lua -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Lua 5.3 and later read C's types through string.unpack: a record type for
// each record, whose view reads a bytes object at the offset the record
// begins at, in the byte order the caller gives, little-endian unless said,
// with an accessor for each member at the offset the library records, as
// ViewPlan plans them.  Integers are Lua's own, an unsigned one of eight
// bytes past 2^63 wrapping as they do; a pointer reads as the address it
// holds.  An array's elements read through methods that take their index,
// a bitfield through accessors whose arithmetic follows C, and bytes no
// integer holds, such as a 16-byte integer's, as the string they are.
//
//===----------------------------------------------------------------------===//

#include "ViewPlan.h"

#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::types::render {
namespace {

std::string hex(uint64_t Value, unsigned Digits = 1) {
  std::string Text;
  llvm::raw_string_ostream(Text)
      << "0x" << llvm::format_hex_no_prefix(Value, Digits, /*Upper=*/true);
  return Text;
}

/// string.pack's format for an integer of \p Bytes, without its order.
std::string integerFormat(unsigned Bytes, bool Signed) {
  if (Bytes == 1)
    return Signed ? "b" : "B";
  return (Signed ? "i" : "I") + std::to_string(Bytes);
}

/// What a class's accessors touch, for the module's checks.
struct ClassState {
  std::vector<std::string> Touch;
  std::vector<std::string> Spans;
};

class LuaWriter {
public:
  LuaWriter(const TypeLibrary &Library, const RenderOptions &Options)
      : Library(Library), Options(Options),
        Plan(Library, Options, Language::Lua) {}

  /// \p Type alone, for reading.
  void alone(const LocalType &Type, std::string &Out);
  /// \p Types as a module that runs.
  void module(llvm::ArrayRef<const LocalType *> Types, std::string &Out);

private:
  /// What \p Value reads as: an integer, an address, a number, a boolean
  /// or a string, or with \p Many, several of them.
  static std::string kind(const ViewValue &Value, bool Many = false) {
    if (Value.Address)
      return Many ? "addresses" : "address";
    const std::string One = kind(Value.TheForm, Value.Bytes);
    return Many ? One + "s" : One;
  }
  static std::string kind(ViewValue::Form Form, unsigned Bytes) {
    switch (Form) {
    case ViewValue::Form::Integer:
      return Bytes == 16 ? "string" : "integer";
    case ViewValue::Form::Bool:
      return "boolean";
    case ViewValue::Form::Float:
      return "number";
    case ViewValue::Form::Bytes:
      return "string";
    }
    return "integer";
  }
  std::string read(const ViewValue &Value, const std::string &At) const;
  std::string write(const ViewValue &Value, const std::string &At,
                    const std::string &What) const;

  void enumeration(const ViewEnum &Enum, std::string &Out) const;
  void typedefinition(const LocalType &Type, std::string &Out,
                      std::string &Aliases) const;
  void record(const ViewClass &Class, std::string &Out);
  void accessor(const ViewClass &Class, const ViewAccessor &Accessor,
                ClassState &State, std::string &Out);
  void bits(const ViewClass &Class, const ViewAccessor &Accessor,
            ClassState &State, std::string &Out) const;
  void records(const LocalType &Type, std::string &Enums, std::string &Classes);

  const TypeLibrary &Library;
  const RenderOptions &Options;
  const ViewPlanner Plan;
  std::string Checks;

public:
  bool Indexes = false;
};

std::string LuaWriter::read(const ViewValue &Value,
                            const std::string &At) const {
  switch (Value.TheForm) {
  case ViewValue::Form::Integer:
    if (Value.Bytes == 16)
      return "raw(r, " + At + ", 16)";
    return "get(r, " + At + ", \"" + integerFormat(Value.Bytes, Value.Signed) +
           "\")";
  case ViewValue::Form::Float:
    return "get(r, " + At + ", \"" + (Value.Bytes == 4 ? "f" : "d") + "\")";
  case ViewValue::Form::Bool:
    return "get(r, " + At + ", \"B\") ~= 0";
  case ViewValue::Form::Bytes:
    return "raw(r, " + At + ", " + std::to_string(Value.Bytes) + ")";
  }
  return "nil";
}

std::string LuaWriter::write(const ViewValue &Value, const std::string &At,
                             const std::string &What) const {
  switch (Value.TheForm) {
  case ViewValue::Form::Integer:
    if (Value.Bytes == 16)
      return "putRaw(r, " + At + ", 16, " + What + ")";
    return "put(r, " + At + ", \"" + integerFormat(Value.Bytes, Value.Signed) +
           "\", " + What + ")";
  case ViewValue::Form::Float:
    return "put(r, " + At + ", \"" + (Value.Bytes == 4 ? "f" : "d") + "\", " +
           What + ")";
  case ViewValue::Form::Bool:
    return "put(r, " + At + ", \"B\", " + What + " and 1 or 0)";
  case ViewValue::Form::Bytes:
    return "putRaw(r, " + At + ", " + std::to_string(Value.Bytes) + ", " +
           What + ")";
  }
  return "";
}

void LuaWriter::enumeration(const ViewEnum &Enum, std::string &Out) const {
  const EnumDef &Def = *Enum.Enum;
  const unsigned Bytes = Library.builtinBytes(Def.Underlying);
  const bool Signed = builtinInfo(Def.Underlying).Signed;
  Out += "-- " + Enum.Doc + ": an enumeration of " + std::to_string(Bytes) +
         " bytes, which fields hold as integers.\n";
  Out += "M." + Enum.Name + " = {\n";
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
    Out += "  " + Plan.names().enumerator(Def, I) + " = " + Number + ",\n";
  }
  Out += "}\n";
}

void LuaWriter::typedefinition(const LocalType &Type, std::string &Out,
                               std::string &Aliases) const {
  const ViewTypedef Def = Plan.typedefinition(Type);
  switch (Def.TheKind) {
  case ViewTypedef::Kind::Function:
    if (!Def.Declared.empty())
      Out += "-- " + Def.Name + " is the function type " + Def.Declared +
             "; a pointer to it holds its address.\n";
    return;
  case ViewTypedef::Kind::Record:
  case ViewTypedef::Kind::Enum:
    // A record's or enumeration's other name is its type's too.
    Aliases += "M." + Def.Name + " = M." + Def.Target + "\n";
    return;
  case ViewTypedef::Kind::Array:
    Out += "-- " + Def.Name + " is " + Def.Declared +
           ", which a member of it reads by index.\n";
    return;
  case ViewTypedef::Kind::Value:
    // An address is an integer too.
    Out += "-- " + Def.Name + " is " + Def.Declared + ", which reads as " +
           (Def.Value.Address || kind(Def.Value) == "integer" ? "an " : "a ") +
           kind(Def.Value) + ".\n";
    return;
  case ViewTypedef::Kind::Other:
    if (!Def.Declared.empty())
      Out += "-- " + Def.Name + " is " + Def.Declared + ".\n";
    return;
  }
}

void LuaWriter::accessor(const ViewClass &Class, const ViewAccessor &Accessor,
                         ClassState &State, std::string &Out) {
  const std::string &Name = Accessor.Name;
  const std::string Offset = hex(Accessor.Offset, Class.Digits);
  if (Accessor.TheKind == ViewAccessor::Kind::Unread) {
    Out += "  -- " + Accessor.Declared + ": not a value that reads.\n";
    return;
  }
  const bool Record = Accessor.TheKind == ViewAccessor::Kind::Record;
  const std::string View = "M." + Accessor.Class;

  if (Accessor.Bounds.empty()) {
    if (Record) {
      Out += "  -- " + Accessor.Declared + ", a view of its bytes\n";
      Out += "  function T.get." + Name + "(r) return " + View +
             ".view(r[BYTES], r[AT] + " + Offset + ", r[ORDER] == \">\") end\n";
      State.Spans.push_back("{ " + Offset + ", " + View + ".size }");
      return;
    }
    Out += "  -- " + Accessor.Declared + ": " + kind(Accessor.Value) + "\n";
    Out += "  function T.get." + Name + "(r) return " +
           read(Accessor.Value, Offset) + " end\n";
    Out += "  function T.set." + Name + "(r, value) " +
           write(Accessor.Value, Offset, "value") + " end\n";
    State.Touch.push_back("r." + Name + " = r." + Name);
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
            Params[I] + ", " + (Bound ? std::to_string(Bound) : "math.huge") +
            ")";
    ParamList += ", " + Params[I];
    Last += (I ? ", " : "") + std::to_string(Bound ? Bound - 1 : uint64_t(0));
    Flexible |= !Bound;
    Total *= Bound;
  }
  if (Record) {
    Out += "  -- " + Accessor.Declared + ", each a view of its bytes\n";
    Out += "  function T.methods." + Name + "(r" + ParamList + ") return " +
           View + ".view(r[BYTES], r[AT] + " + Spot +
           ", r[ORDER] == \">\") end\n";
    State.Spans.push_back("{ " + Offset + ", " + std::to_string(Total) + " }");
    return;
  }
  Out += "  -- " + Accessor.Declared + ": " + kind(Accessor.Value, true) + "\n";
  Out += "  function T.methods." + Name + "(r" + ParamList + ") return " +
         read(Accessor.Value, Spot) + " end\n";
  if (Accessor.Setter.empty()) {
    if (!Flexible)
      State.Touch.push_back("local _ = r:" + Name + "(" + Last + ")");
    return;
  }
  Out += "  function T.methods." + Accessor.Setter + "(r" + ParamList +
         ", value) " + write(Accessor.Value, Spot, "value") + " end\n";
  if (!Flexible)
    State.Touch.push_back("r:" + Accessor.Setter + "(" + Last + ", r:" + Name +
                          "(" + Last + "))");
}

void LuaWriter::bits(const ViewClass &Class, const ViewAccessor &Accessor,
                     ClassState &State, std::string &Out) const {
  const std::string &Name = Accessor.Name;
  const BitAccess &Access = Accessor.Bits;
  const unsigned Bytes = Accessor.UnitBytes;
  if (Accessor.TheKind == ViewAccessor::Kind::Unread) {
    Out += "  -- " + Accessor.Declared + ": a unit of " +
           std::to_string(Bytes) + " bytes, which no integer holds.\n";
    return;
  }
  const std::string Unit = hex(Accessor.Offset, Class.Digits);
  const std::string Format = integerFormat(Bytes, /*Signed=*/false);
  const std::string Get = "get(r, " + Unit + ", \"" + Format + "\")";
  const unsigned UnitBits = Bytes * 8;
  const uint64_t Mask =
      Access.Width >= 64 ? ~uint64_t(0) : (uint64_t(1) << Access.Width) - 1;
  const uint64_t Shifted = Mask << Access.Shift;
  const uint64_t UnitMask =
      UnitBits >= 64 ? ~uint64_t(0) : (uint64_t(1) << UnitBits) - 1;
  const uint64_t Clear = ~Shifted & UnitMask;
  // Lua's integers are 64-bit and shift logically: a field's bits mask out,
  // and a signed one's sign extends by its top bit.
  std::string Bits = Get;
  if (Access.Shift)
    Bits = "(" + Bits + " >> " + std::to_string(Access.Shift) + ")";
  if (Access.Width < 64)
    Bits += " & " + hex(Mask);
  std::string Reads;
  if (Access.Signed && Access.Width < 64) {
    const std::string Top = hex(uint64_t(1) << (Access.Width - 1));
    Reads = "local v = " + Bits + "; return (v ~ " + Top + ") - " + Top;
  } else {
    Reads = "return " + Bits;
  }
  // string.pack takes an unsigned unit whole.
  const std::string Writes =
      Access.Width == UnitBits
          ? "put(r, " + Unit + ", \"" + Format + "\", value" +
                (UnitBits < 64 ? " & " + hex(UnitMask) : std::string()) + ")"
          : "put(r, " + Unit + ", \"" + Format + "\", (" + Get + " & " +
                hex(Clear) + ") | (" +
                (Access.Shift
                     ? "(value << " + std::to_string(Access.Shift) + ")"
                     : std::string("value")) +
                " & " + hex(Shifted) + "))";
  Out += "  -- " + Accessor.Declared + "\n";
  Out += "  function T.get." + Name + "(r) " + Reads + " end\n";
  Out += "  function T.set." + Name + "(r, value) " + Writes + " end\n";
  State.Touch.push_back("r." + Name + " = r." + Name);
}

void LuaWriter::record(const ViewClass &Class, std::string &Out) {
  Out += "-- " + Class.Doc + "\n";
  Out += "do\n";
  Out += "  local T = record(\"" + Class.Name + "\", " +
         hex(Class.Record->Size) + ")\n";
  Out += "  M." + Class.Name + " = T\n";
  ClassState State;
  for (const ViewAccessor &Accessor : Class.Accessors)
    if (Accessor.TheKind == ViewAccessor::Kind::Bits || Accessor.UnitBytes)
      bits(Class, Accessor, State, Out);
    else
      accessor(Class, Accessor, State, Out);
  Out += "end\n";
  if (!Options.Asserts)
    return;
  Checks += "check(M." + Class.Name + ", function(r)";
  if (State.Touch.empty()) {
    Checks += " end";
  } else {
    Checks += "\n";
    for (const std::string &Statement : State.Touch)
      Checks += "  " + Statement + "\n";
    Checks += "end";
  }
  if (!State.Spans.empty()) {
    Checks += ", {";
    for (size_t I = 0; I < State.Spans.size(); ++I)
      Checks += (I ? ", " : " ") + State.Spans[I];
    Checks += " }";
  }
  Checks += ")\n";
}

void LuaWriter::records(const LocalType &Type, std::string &Enums,
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

void LuaWriter::alone(const LocalType &Type, std::string &Out) {
  const std::string Name = Plan.names().type(Type.Name);
  switch (Type.Kind) {
  case TypeKind::Struct:
  case TypeKind::Union: {
    if (!Type.Record)
      return;
    if (!Type.Record->Defined) {
      Out += "-- " + Type.Name + ": declared only; its members are unknown.\n";
      Out += "M." + Name + " = record(\"" + Name + "\")\n";
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
    std::string Aliases;
    typedefinition(Type, Out, Aliases);
    Out += Aliases;
    return;
  }
  }
}

void LuaWriter::module(llvm::ArrayRef<const LocalType *> Types,
                       std::string &Out) {
  std::string Enums, Classes, Typedefs, Aliases;
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
        Classes +=
            "-- " + Type->Name + ": declared only; its members are unknown.\n";
        Classes += "M." + Name + " = record(\"" + Name + "\")\n\n";
        break;
      }
      records(*Type, Enums, Classes);
      Classes += '\n';
      break;
    case TypeKind::Typedef:
    case TypeKind::Function:
      typedefinition(*Type, Typedefs, Aliases);
      break;
    }
  }

  Out += R"(local M = {}

-- A view's own keys, which no member's name can be.
local BYTES, AT, ORDER = {}, {}, {}

--- Bytes that views read and write: { data = <string> }, whose data a
--- write replaces.
function M.bytes(data)
  return { data = data }
end

local function get(r, at, format)
  return (string.unpack(r[ORDER] .. format, r[BYTES].data, r[AT] + at + 1))
end

--- The bytes at at and the n after, which must lie within the data.
local function span(r, at, n)
  local from = r[AT] + at
  if from < 0 or from + n > #r[BYTES].data then
    error(n .. " bytes at " .. from .. " pass the end of the data", 3)
  end
  return from
end

local function put(r, at, format, value)
  local packed = string.pack(r[ORDER] .. format, value)
  local from = span(r, at, #packed)
  local data = r[BYTES].data
  r[BYTES].data = data:sub(1, from) .. packed .. data:sub(from + #packed + 1)
end

local function raw(r, at, n)
  local from = span(r, at, n)
  return r[BYTES].data:sub(from + 1, from + n)
end

local function putRaw(r, at, n, value)
  if #value ~= n then
    error("a string of " .. n .. " bytes is due, not " .. #value, 2)
  end
  local from = span(r, at, n)
  local data = r[BYTES].data
  r[BYTES].data = data:sub(1, from) .. value .. data:sub(from + n + 1)
end

--- A record type: its members' getters and setters, and its methods.
local function record(name, size)
  local T = { name = name, size = size, get = {}, set = {}, methods = {} }
  T.__index = function(r, key)
    local getter = T.get[key]
    if getter then return getter(r) end
    local method = T.methods[key]
    if method then return method end
    error(name .. " has no member " .. tostring(key), 2)
  end
  T.__newindex = function(r, key, value)
    local setter = T.set[key]
    if not setter then
      error(name .. "." .. tostring(key) .. " is no value to write", 2)
    end
    setter(r, value)
  end
  --- A view of the record at offset in bytes, little-endian unless big.
  function T.view(bytes, offset, big)
    return setmetatable(
      { [BYTES] = bytes, [AT] = offset or 0, [ORDER] = big and ">" or "<" }, T)
  end
  return T
end
)";
  if (Indexes)
    Out += R"(
--- i, when it indexes an array of count elements.
local function index(i, count)
  if math.type(i) ~= "integer" or i < 0 or i >= count then
    error("index " .. tostring(i) .. " is outside an array of " .. count, 3)
  end
  return i
end
)";
  Out += "\n" + Enums + Classes;
  if (!Typedefs.empty() || !Aliases.empty())
    Out += Typedefs + Aliases + "\n";
  if (!Checks.empty()) {
    Out +=
        R"(--- Checks T against its size: touch reads each accessor and writes it
--- back in bytes of T.size, which raises an error past their end, and each
--- record held in place, at { offset, bytes } in spans, ends within it.
local function check(T, touch, spans)
  touch(T.view(M.bytes(string.rep("\0", T.size))))
  for _, at in ipairs(spans or {}) do
    if at[1] + at[2] > T.size then
      error(T.name .. ": " .. at[2] .. " bytes at " .. at[1] .. " pass its end")
    end
  end
end

)";
    Out += Checks + "\n";
  }
  Out += "return M\n";
}

} // namespace

std::string renderLua(const TypeLibrary &Library, const LocalType &Type,
                      const RenderOptions &Options) {
  LuaWriter Writer(Library, Options);
  std::string Out;
  Writer.alone(Type, Out);
  return Out;
}

std::string renderLuaModule(const TypeLibrary &Library,
                            llvm::ArrayRef<const LocalType *> Roots,
                            const RenderOptions &Options) {
  const std::vector<const LocalType *> Types =
      neededTypes(Library, Roots, /*ThroughPointers=*/true);
  LuaWriter Writer(Library, Options);
  std::string Body;
  Writer.module(Types, Body);
  return "-- " + std::to_string(Types.size()) +
         " types, rendered by NeverD for Lua 5.3 and later: each record\n"
         "-- reads its bytes through string.unpack, every member at the "
         "offset the\n"
         "-- library records.  An unsigned integer of eight bytes past 2^63 "
         "wraps\n"
         "-- as Lua's integers do; math.ult compares them.\n\n" +
         Body;
}

} // namespace neverd::types::render
