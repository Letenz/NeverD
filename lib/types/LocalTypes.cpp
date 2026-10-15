//===- LocalTypes.cpp - The named types of a program ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/LocalTypes.h"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace neverd::types {
namespace {

constexpr BuiltinInfo Builtins[] = {
#define NEVERD_BUILTIN_TYPE(Name, Class, Signed, LLP64, LP64, ILP32, Keywords, \
                            Windows, IDA)                                      \
  {BuiltinClass::Class, Signed, {LLP64, LP64, ILP32}, Keywords, Windows, IDA},
#include "neverd/types/BuiltinTypes.def"
};

/// The alignment of a builtin of \p Bytes, as every supported ABI aligns
/// its scalars: to their size, up to the largest the model allows.
uint32_t scalarAlignment(uint64_t Bytes, DataModel Model) {
  if (!Bytes)
    return 1;
  // 32-bit targets align eight-byte scalars to four outside Windows; local
  // types keep the conservative eight, which MSVC uses.
  (void)Model;
  return static_cast<uint32_t>(std::min<uint64_t>(Bytes, 16));
}

} // namespace

const BuiltinInfo &builtinInfo(Builtin Type) {
  return Builtins[static_cast<size_t>(Type)];
}

unsigned builtinBytes(Builtin Type, DataModel Model) {
  return builtinInfo(Type).Bytes[static_cast<size_t>(Model)];
}

unsigned pointerBytes(DataModel Model) {
  return Model == DataModel::ILP32 ? 4 : 8;
}

TypeExprRef makeBuiltin(Builtin Type) {
  auto Expr = std::make_shared<TypeExpr>();
  Expr->Kind = TypeExprKind::Builtin;
  Expr->TheBuiltin = Type;
  return Expr;
}

TypeExprRef makeNamed(std::string Name, TypeKind Kind) {
  auto Expr = std::make_shared<TypeExpr>();
  Expr->Kind = TypeExprKind::Named;
  Expr->Name = std::move(Name);
  Expr->NamedKind = Kind;
  return Expr;
}

TypeExprRef makePointer(TypeExprRef Pointee, unsigned Bytes, bool Reference) {
  auto Expr = std::make_shared<TypeExpr>();
  Expr->Kind = TypeExprKind::Pointer;
  Expr->Element = std::move(Pointee);
  Expr->PointerBytes = static_cast<uint8_t>(Bytes);
  Expr->Reference = Reference;
  return Expr;
}

TypeExprRef makeArray(TypeExprRef Element, std::optional<uint64_t> Count) {
  auto Expr = std::make_shared<TypeExpr>();
  Expr->Kind = TypeExprKind::Array;
  Expr->Element = std::move(Element);
  Expr->Count = Count;
  return Expr;
}

TypeExprRef makeFunction(std::shared_ptr<const FunctionDef> Function) {
  auto Expr = std::make_shared<TypeExpr>();
  Expr->Kind = TypeExprKind::Function;
  Expr->Function = std::move(Function);
  return Expr;
}

TypeExprRef makeRecord(std::shared_ptr<const RecordDef> Record) {
  auto Expr = std::make_shared<TypeExpr>();
  Expr->Kind = TypeExprKind::Record;
  Expr->Record = std::move(Record);
  return Expr;
}

TypeExprRef makeEnum(std::shared_ptr<const EnumDef> Enum) {
  auto Expr = std::make_shared<TypeExpr>();
  Expr->Kind = TypeExprKind::Enum;
  Expr->Enum = std::move(Enum);
  return Expr;
}

TypeExprRef qualified(TypeExprRef Type, bool Const, bool Volatile) {
  if (!Type || ((!Const || Type->Const) && (!Volatile || Type->Volatile)))
    return Type;
  auto Copy = std::make_shared<TypeExpr>(*Type);
  Copy->Const = Copy->Const || Const;
  Copy->Volatile = Copy->Volatile || Volatile;
  return Copy;
}

uint32_t TypeLibrary::define(LocalType Type) {
  auto Found = ByName.find(Type.Name);
  if (Found != ByName.end()) {
    Type.Ordinal = Found->second;
    ByOrdinal[Found->second - 1] = std::make_unique<LocalType>(std::move(Type));
    return Found->second;
  }
  const auto Ordinal = static_cast<uint32_t>(ByOrdinal.size() + 1);
  Type.Ordinal = Ordinal;
  ByName[Type.Name] = Ordinal;
  ByOrdinal.push_back(std::make_unique<LocalType>(std::move(Type)));
  return Ordinal;
}

bool TypeLibrary::remove(llvm::StringRef Name) {
  auto Found = ByName.find(Name);
  if (Found == ByName.end())
    return false;
  ByOrdinal[Found->second - 1].reset();
  ByName.erase(Found);
  return true;
}

const LocalType *TypeLibrary::find(llvm::StringRef Name) const {
  auto Found = ByName.find(Name);
  return Found == ByName.end() ? nullptr : ByOrdinal[Found->second - 1].get();
}

const LocalType *TypeLibrary::byOrdinal(uint32_t Ordinal) const {
  if (!Ordinal || Ordinal > ByOrdinal.size())
    return nullptr;
  return ByOrdinal[Ordinal - 1].get();
}

const LocalType *TypeLibrary::resolve(llvm::StringRef Name) const {
  const LocalType *Type = find(Name);
  // A typedef chain longer than the library would be a cycle.
  for (size_t Steps = 0;
       Type && Type->Kind == TypeKind::Typedef && Steps <= ByOrdinal.size();
       ++Steps) {
    if (!Type->Target || Type->Target->Kind != TypeExprKind::Named)
      return Type;
    Type = find(Type->Target->Name);
  }
  return Type;
}

std::vector<const LocalType *> TypeLibrary::types() const {
  std::vector<const LocalType *> Result;
  Result.reserve(ByName.size());
  for (const auto &Type : ByOrdinal)
    if (Type)
      Result.push_back(Type.get());
  return Result;
}

std::optional<uint64_t> TypeLibrary::sizeOf(const TypeExpr &Type) const {
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    if (Type.TheBuiltin == Builtin::Void)
      return std::nullopt;
    return builtinBytes(Type.TheBuiltin, Model);
  case TypeExprKind::Pointer:
    return Type.PointerBytes;
  case TypeExprKind::Array: {
    if (!Type.Count || !Type.Element)
      return std::nullopt;
    auto Element = sizeOf(*Type.Element);
    if (!Element)
      return std::nullopt;
    return *Element * *Type.Count;
  }
  case TypeExprKind::Function:
    return std::nullopt;
  case TypeExprKind::Record:
    return Type.Record ? std::optional<uint64_t>(Type.Record->Size)
                       : std::nullopt;
  case TypeExprKind::Enum:
    return Type.Enum ? std::optional<uint64_t>(
                           builtinBytes(Type.Enum->Underlying, Model))
                     : std::nullopt;
  case TypeExprKind::Named: {
    const LocalType *Named = resolve(Type.Name);
    if (!Named)
      return std::nullopt;
    switch (Named->Kind) {
    case TypeKind::Struct:
    case TypeKind::Union:
      if (!Named->Record || !Named->Record->Defined)
        return std::nullopt;
      return Named->Record->Size;
    case TypeKind::Enum:
      return Named->Enum ? std::optional<uint64_t>(
                               builtinBytes(Named->Enum->Underlying, Model))
                         : std::nullopt;
    case TypeKind::Typedef:
      return Named->Target ? sizeOf(*Named->Target) : std::nullopt;
    case TypeKind::Function:
      return std::nullopt;
    }
    return std::nullopt;
  }
  }
  return std::nullopt;
}

uint32_t TypeLibrary::alignOf(const RecordDef &Record) const {
  if (Record.Alignment)
    return Record.Alignment;
  uint32_t Alignment = 1;
  for (const Member &Field : Record.Members)
    if (Field.Type)
      Alignment = std::max(Alignment, alignOf(*Field.Type));
  if (Record.Pack)
    Alignment = std::min(Alignment, Record.Pack);
  return Alignment;
}

uint32_t TypeLibrary::alignOf(const TypeExpr &Type) const {
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    return scalarAlignment(builtinBytes(Type.TheBuiltin, Model), Model);
  case TypeExprKind::Pointer:
    return Type.PointerBytes;
  case TypeExprKind::Array:
    return Type.Element ? alignOf(*Type.Element) : 1;
  case TypeExprKind::Function:
    return 1;
  case TypeExprKind::Record:
    return Type.Record ? alignOf(*Type.Record) : 1;
  case TypeExprKind::Enum:
    return Type.Enum ? scalarAlignment(
                           builtinBytes(Type.Enum->Underlying, Model), Model)
                     : 4;
  case TypeExprKind::Named: {
    const LocalType *Named = resolve(Type.Name);
    if (!Named)
      return 1;
    switch (Named->Kind) {
    case TypeKind::Struct:
    case TypeKind::Union:
      return Named->Record ? alignOf(*Named->Record) : 1;
    case TypeKind::Enum:
      return Named->Enum
                 ? scalarAlignment(builtinBytes(Named->Enum->Underlying, Model),
                                   Model)
                 : 4;
    case TypeKind::Typedef:
      return Named->Target ? alignOf(*Named->Target) : 1;
    case TypeKind::Function:
      return 1;
    }
    return 1;
  }
  }
  return 1;
}

namespace {

/// Members of the flat list that occupy one place: a member, or a run of
/// bitfields that share a storage unit.
struct Unit {
  size_t First = 0;
  size_t End = 0; ///< One past the last member.
  uint64_t Begin = 0;
  uint64_t Finish = 0; ///< One past its last bit.
  bool Bitfields = false;
};

std::vector<Unit> unitsOf(llvm::ArrayRef<FlatMember> Flat) {
  std::vector<Unit> Units;
  for (size_t I = 0; I < Flat.size(); ++I) {
    const FlatMember &Field = Flat[I];
    if (Field.BitWidth) {
      // A bitfield continues the run before it when it lies in the same
      // storage unit after that run's last bitfield.
      if (!Units.empty() && Units.back().Bitfields) {
        const FlatMember &Last = Flat[Units.back().End - 1];
        if (Last.StorageBitOffset == Field.StorageBitOffset &&
            Last.StorageBits == Field.StorageBits &&
            Field.BitOffset >= Last.BitOffset + Last.BitWidth) {
          Units.back().End = I + 1;
          continue;
        }
      }
      Units.push_back({I, I + 1, Field.StorageBitOffset,
                       Field.StorageBitOffset + Field.StorageBits, true});
      continue;
    }
    Units.push_back({I, I + 1, Field.BitOffset,
                     Field.BitOffset + Field.StorageBits, false});
  }
  return Units;
}

class Nester {
public:
  explicit Nester(llvm::ArrayRef<FlatMember> Flat)
      : Flat(Flat), Units(unitsOf(Flat)) {}

  std::vector<Member> record(bool Union) {
    if (Units.empty())
      return {};
    if (Union)
      return alternatives(0, Units.size(), 0);
    return body(0, Units.size(), 0);
  }

private:
  /// \p Unit's members, with offsets from \p Base.
  void members(const Unit &Unit, uint64_t Base, std::vector<Member> &Out) {
    for (size_t I = Unit.First; I < Unit.End; ++I) {
      const FlatMember &Field = Flat[I];
      Member M;
      M.Name = Field.Name;
      M.Type = Field.Type;
      M.BitOffset = Field.BitOffset - Base;
      M.BitWidth = Field.BitWidth;
      Out.push_back(std::move(M));
    }
  }

  /// An anonymous record in place, holding \p Members, at \p Begin from
  /// \p Base.
  Member anonymous(bool Union, std::vector<Member> Members, uint64_t Begin,
                   uint64_t Finish, uint64_t Base) {
    auto Record = std::make_shared<RecordDef>();
    Record->Union = Union;
    Record->Size = (Finish - Begin + 7) / 8;
    Record->Members = std::move(Members);
    Member M;
    M.Type = makeRecord(std::move(Record));
    M.BitOffset = Begin - Base;
    return M;
  }

  /// One alternative of a union: units [First, End), all from \p Begin.  A
  /// single member stands alone; anything more is a structure.
  Member alternative(size_t First, size_t End, uint64_t Begin) {
    if (End - First == 1 && !Units[First].Bitfields &&
        Units[First].Begin == Begin) {
      std::vector<Member> Single;
      members(Units[First], Begin, Single);
      Member M = std::move(Single.front());
      return M;
    }
    uint64_t Finish = Begin;
    for (size_t I = First; I < End; ++I)
      Finish = std::max(Finish, Units[I].Finish);
    return anonymous(/*Union=*/false, body(First, End, Begin), Begin, Finish,
                     Begin);
  }

  /// The alternatives of a union over units [First, End) that begins at
  /// \p Begin: each unit that begins at \p Begin starts one, which takes the
  /// units after it up to the next.  When no other unit begins there, a
  /// unit that overlaps the alternative before it starts the next one, after
  /// padding; every alternative is then smaller than the union, so nesting
  /// ends.
  std::vector<Member> alternatives(size_t First, size_t End, uint64_t Begin) {
    std::vector<size_t> Starts{First};
    for (size_t I = First + 1; I < End; ++I)
      if (Units[I].Begin == Begin)
        Starts.push_back(I);
    if (Starts.size() == 1 && End - First > 1) {
      uint64_t ArmFinish = Units[First].Finish;
      for (size_t I = First + 1; I < End; ++I) {
        if (Units[I].Begin < ArmFinish) {
          Starts.push_back(I);
          ArmFinish = Units[I].Finish;
          continue;
        }
        ArmFinish = std::max(ArmFinish, Units[I].Finish);
      }
    }
    std::vector<Member> Arms;
    for (size_t S = 0; S < Starts.size(); ++S)
      Arms.push_back(alternative(
          Starts[S], S + 1 < Starts.size() ? Starts[S + 1] : End, Begin));
    return Arms;
  }

  /// The members of units [First, End) laid out one after another, with
  /// offsets from \p Base, and unions where units overlap.
  std::vector<Member> body(size_t First, size_t End, uint64_t Base) {
    std::vector<Member> Out;
    size_t I = First;
    while (I < End) {
      const uint64_t Begin = Units[I].Begin;
      uint64_t Finish = Units[I].Finish;
      bool Overlaps = false;
      size_t J = I + 1;
      // A union reaches as far as any unit inside it reaches.
      while (J < End && Units[J].Begin < Finish) {
        Overlaps = true;
        Finish = std::max(Finish, Units[J].Finish);
        ++J;
      }
      if (!Overlaps) {
        members(Units[I], Base, Out);
        I = J;
        continue;
      }
      Out.push_back(anonymous(/*Union=*/true, alternatives(I, J, Begin), Begin,
                              Finish, Base));
      I = J;
    }
    return Out;
  }

  llvm::ArrayRef<FlatMember> Flat;
  std::vector<Unit> Units;
};

void flattenInto(const RecordDef &Record, uint64_t Base,
                 std::vector<FlatMember> &Out) {
  for (const Member &Field : Record.Members) {
    if (Field.isAnonymous() && Field.Type->Record) {
      flattenInto(*Field.Type->Record, Base + Field.BitOffset, Out);
      continue;
    }
    FlatMember Flat;
    Flat.Name = Field.Name;
    Flat.Type = Field.Type;
    Flat.BitOffset = Base + Field.BitOffset;
    Flat.BitWidth = Field.BitWidth;
    Out.push_back(std::move(Flat));
  }
}

} // namespace

std::vector<Member> nestFlatMembers(llvm::ArrayRef<FlatMember> Flat,
                                    bool Union) {
  return Nester(Flat).record(Union);
}

std::vector<FlatMember> flattenMembers(const RecordDef &Record) {
  std::vector<FlatMember> Out;
  flattenInto(Record, 0, Out);
  return Out;
}

} // namespace neverd::types
