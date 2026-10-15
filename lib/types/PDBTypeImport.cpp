//===- PDBTypeImport.cpp - Local types from a PDB's type records ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/PDBTypeImport.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/CodeView/CVTypeVisitor.h"
#include "llvm/DebugInfo/CodeView/LazyRandomTypeCollection.h"
#include "llvm/DebugInfo/CodeView/TypeDeserializer.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/CodeView/TypeVisitorCallbacks.h"
#include "llvm/DebugInfo/PDB/IPDBSession.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/DebugInfo/PDB/PDBTypes.h"

#include <optional>
#include <string>

namespace neverd::types {
namespace {

namespace cv = llvm::codeview;

/// A record MSVC names only by its place: an anonymous union, structure or
/// enumeration, which the member that uses it defines in place.
bool isUnnamed(llvm::StringRef Name) {
  return Name.empty() || Name.starts_with("<unnamed-") ||
         Name.starts_with("<anonymous-") || Name.starts_with("__unnamed") ||
         Name.contains("::<unnamed-");
}

std::optional<Builtin> builtinOf(cv::SimpleTypeKind Kind) {
  switch (Kind) {
#define NEVERD_PDB_SIMPLE_TYPE(CodeViewKind, BuiltinName)                      \
  case cv::SimpleTypeKind::CodeViewKind:                                       \
    return Builtin::BuiltinName;
#include "PDBSimpleTypes.def"
  default:
    return std::nullopt;
  }
}

/// The size of a pointer a simple type mode makes; zero for a direct type.
unsigned simplePointerBytes(cv::SimpleTypeMode Mode) {
  switch (Mode) {
  case cv::SimpleTypeMode::Direct:
    return 0;
  case cv::SimpleTypeMode::NearPointer:
    return 2;
  case cv::SimpleTypeMode::FarPointer:
  case cv::SimpleTypeMode::HugePointer:
  case cv::SimpleTypeMode::NearPointer32:
  case cv::SimpleTypeMode::FarPointer32:
    return 4;
  case cv::SimpleTypeMode::NearPointer64:
    return 8;
  case cv::SimpleTypeMode::NearPointer128:
    return 16;
  }
  return 8;
}

DataModel dataModelOf(llvm::pdb::PDB_Machine Machine) {
  switch (Machine) {
  case llvm::pdb::PDB_Machine::x86:
  case llvm::pdb::PDB_Machine::ArmNT:
  case llvm::pdb::PDB_Machine::Arm:
  case llvm::pdb::PDB_Machine::Thumb:
    return DataModel::ILP32;
  default:
    return DataModel::LLP64;
  }
}

template <typename RecordT>
std::optional<RecordT> deserialize(const cv::CVType &Type) {
  RecordT Record(static_cast<cv::TypeRecordKind>(Type.kind()));
  if (auto Err = cv::TypeDeserializer::deserializeAs<RecordT>(
          const_cast<cv::CVType &>(Type), Record)) {
    llvm::consumeError(std::move(Err));
    return std::nullopt;
  }
  return Record;
}

bool isClassKind(cv::TypeLeafKind Kind) {
  return Kind == cv::LF_STRUCTURE || Kind == cv::LF_CLASS ||
         Kind == cv::LF_INTERFACE;
}

/// What one field list holds, in order.
class FieldCollector final : public cv::TypeVisitorCallbacks {
public:
  struct Data {
    std::string Name;
    cv::TypeIndex Type;
    uint64_t Offset = 0;
    bool Base = false;
  };

  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::DataMemberRecord &Record) override {
    Fields.push_back(
        {Record.getName().str(), Record.getType(), Record.getFieldOffset()});
    return llvm::Error::success();
  }
  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::BaseClassRecord &Record) override {
    Fields.push_back({"baseclass_" + std::to_string(Record.getBaseOffset()),
                      Record.getBaseType(), Record.getBaseOffset(), true});
    return llvm::Error::success();
  }
  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::VFPtrRecord &Record) override {
    Fields.push_back({"__vftable", Record.getType(), 0});
    return llvm::Error::success();
  }
  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::EnumeratorRecord &Record) override {
    const llvm::APSInt &Value = Record.getValue();
    Enumerators.push_back({Record.getName().str(),
                           Value.isSigned()
                               ? static_cast<uint64_t>(Value.getSExtValue())
                               : Value.getZExtValue(),
                           std::string()});
    return llvm::Error::success();
  }
  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::ListContinuationRecord &Record) override {
    Continuation = Record.getContinuationIndex();
    return llvm::Error::success();
  }

  std::vector<Data> Fields;
  std::vector<Enumerator> Enumerators;
  std::optional<cv::TypeIndex> Continuation;
};

class Importer {
public:
  Importer(llvm::pdb::TpiStream &Tpi, TypeLibrary &Library)
      : Tpi(Tpi), Types(Tpi.typeCollection()), Library(Library) {}

  PDBTypeImportStats run();

private:
  /// The type index of the definition a record names: itself when it is
  /// one, the definition of its unique name or name otherwise.
  std::optional<cv::TypeIndex> definition(cv::TypeIndex Index);
  TypeExprRef expr(cv::TypeIndex Index);
  TypeExprRef exprUncached(cv::TypeIndex Index);
  uint64_t sizeOf(cv::TypeIndex Index);
  std::shared_ptr<const RecordDef> record(cv::TypeIndex Definition);
  std::shared_ptr<const EnumDef> enumeration(cv::TypeIndex Definition);
  std::shared_ptr<const FunctionDef> function(cv::TypeIndex Return,
                                              cv::CallingConvention Convention,
                                              cv::TypeIndex Arguments);
  /// The members of the field list \p List, its continuations included.
  FieldCollector fields(cv::TypeIndex List);
  CallingConvention convention(cv::CallingConvention Convention) const;

  llvm::pdb::TpiStream &Tpi;
  cv::LazyRandomTypeCollection &Types;
  TypeLibrary &Library;
  llvm::StringMap<cv::TypeIndex> ByUniqueName;
  llvm::StringMap<cv::TypeIndex> ByName;
  llvm::DenseMap<uint32_t, TypeExprRef> Exprs;
  llvm::DenseMap<uint32_t, uint64_t> Sizes;
  /// Records, named or in place, whose nesting did not flatten back.
  unsigned Mismatches = 0;
};

std::optional<cv::TypeIndex> Importer::definition(cv::TypeIndex Index) {
  const cv::CVType Type = Types.getType(Index);
  llvm::StringRef Name, Unique;
  bool Forward = false;
  if (isClassKind(Type.kind())) {
    auto Record = deserialize<cv::ClassRecord>(Type);
    if (!Record)
      return std::nullopt;
    Forward = Record->isForwardRef();
    Name = Record->getName();
    Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
  } else if (Type.kind() == cv::LF_UNION) {
    auto Record = deserialize<cv::UnionRecord>(Type);
    if (!Record)
      return std::nullopt;
    Forward = Record->isForwardRef();
    Name = Record->getName();
    Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
  } else if (Type.kind() == cv::LF_ENUM) {
    auto Record = deserialize<cv::EnumRecord>(Type);
    if (!Record)
      return std::nullopt;
    Forward = Record->isForwardRef();
    Name = Record->getName();
    Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
  } else {
    return std::nullopt;
  }
  if (!Forward)
    return Index;
  if (!Unique.empty())
    if (auto Found = ByUniqueName.find(Unique); Found != ByUniqueName.end())
      return Found->second;
  if (auto Found = ByName.find(Name); Found != ByName.end())
    return Found->second;
  return std::nullopt;
}

CallingConvention Importer::convention(cv::CallingConvention Convention) const {
  switch (Convention) {
  case cv::CallingConvention::NearC:
  case cv::CallingConvention::FarC:
    // A 64-bit target has one C convention, its own.
    return Library.dataModel() == DataModel::ILP32
               ? CallingConvention::Cdecl
               : CallingConvention::Unspecified;
  case cv::CallingConvention::NearFast:
  case cv::CallingConvention::FarFast:
    return CallingConvention::Fastcall;
  case cv::CallingConvention::NearStdCall:
  case cv::CallingConvention::FarStdCall:
    return CallingConvention::Stdcall;
  case cv::CallingConvention::ThisCall:
    return CallingConvention::Thiscall;
  case cv::CallingConvention::NearVector:
    return CallingConvention::Vectorcall;
  default:
    return CallingConvention::Unspecified;
  }
}

uint64_t Importer::sizeOf(cv::TypeIndex Index) {
  if (Index.isNoneType())
    return 0;
  if (Index.isSimple()) {
    if (unsigned Bytes = simplePointerBytes(Index.getSimpleMode()))
      return Bytes;
    auto Kind = builtinOf(Index.getSimpleKind());
    return Kind ? builtinBytes(*Kind, Library.dataModel()) : 0;
  }
  if (auto Found = Sizes.find(Index.getIndex()); Found != Sizes.end())
    return Found->second;
  uint64_t Size = 0;
  const cv::CVType Type = Types.getType(Index);
  switch (Type.kind()) {
  case cv::LF_POINTER:
    if (auto Record = deserialize<cv::PointerRecord>(Type))
      Size = Record->getSize();
    break;
  case cv::LF_MODIFIER:
    if (auto Record = deserialize<cv::ModifierRecord>(Type))
      Size = sizeOf(Record->getModifiedType());
    break;
  case cv::LF_ARRAY:
    if (auto Record = deserialize<cv::ArrayRecord>(Type))
      Size = Record->getSize();
    break;
  case cv::LF_BITFIELD:
    if (auto Record = deserialize<cv::BitFieldRecord>(Type))
      Size = sizeOf(Record->getType());
    break;
  case cv::LF_STRUCTURE:
  case cv::LF_CLASS:
  case cv::LF_INTERFACE:
    if (auto Definition = definition(Index))
      if (auto Record =
              deserialize<cv::ClassRecord>(Types.getType(*Definition)))
        Size = Record->getSize();
    break;
  case cv::LF_UNION:
    if (auto Definition = definition(Index))
      if (auto Record =
              deserialize<cv::UnionRecord>(Types.getType(*Definition)))
        Size = Record->getSize();
    break;
  case cv::LF_ENUM:
    if (auto Record = deserialize<cv::EnumRecord>(Type))
      Size = sizeOf(Record->getUnderlyingType());
    break;
  default:
    break;
  }
  Sizes[Index.getIndex()] = Size;
  return Size;
}

TypeExprRef Importer::expr(cv::TypeIndex Index) {
  if (Index.isSimple() || Index.isNoneType())
    return exprUncached(Index);
  if (auto Found = Exprs.find(Index.getIndex()); Found != Exprs.end())
    return Found->second;
  TypeExprRef Result = exprUncached(Index);
  Exprs[Index.getIndex()] = Result;
  return Result;
}

TypeExprRef Importer::exprUncached(cv::TypeIndex Index) {
  if (Index.isNoneType())
    return makeBuiltin(Builtin::Void);
  if (Index.isSimple()) {
    auto Kind = builtinOf(Index.getSimpleKind());
    TypeExprRef Base = makeBuiltin(Kind ? *Kind : Builtin::UChar);
    if (unsigned Bytes = simplePointerBytes(Index.getSimpleMode()))
      return makePointer(std::move(Base), Bytes);
    return Base;
  }
  const cv::CVType Type = Types.getType(Index);
  switch (Type.kind()) {
  case cv::LF_POINTER: {
    auto Record = deserialize<cv::PointerRecord>(Type);
    if (!Record)
      break;
    const cv::PointerMode Mode = Record->getMode();
    TypeExprRef Pointer =
        makePointer(expr(Record->getReferentType()), Record->getSize(),
                    Mode == cv::PointerMode::LValueReference ||
                        Mode == cv::PointerMode::RValueReference);
    return qualified(std::move(Pointer), Record->isConst(),
                     Record->isVolatile());
  }
  case cv::LF_MODIFIER: {
    auto Record = deserialize<cv::ModifierRecord>(Type);
    if (!Record)
      break;
    const auto Options = static_cast<uint16_t>(Record->getModifiers());
    return qualified(
        expr(Record->getModifiedType()),
        Options & static_cast<uint16_t>(cv::ModifierOptions::Const),
        Options & static_cast<uint16_t>(cv::ModifierOptions::Volatile));
  }
  case cv::LF_ARRAY: {
    auto Record = deserialize<cv::ArrayRecord>(Type);
    if (!Record)
      break;
    const uint64_t Element = sizeOf(Record->getElementType());
    std::optional<uint64_t> Count;
    if (Element)
      Count = Record->getSize() / Element;
    return makeArray(expr(Record->getElementType()), Count);
  }
  case cv::LF_BITFIELD: {
    auto Record = deserialize<cv::BitFieldRecord>(Type);
    if (!Record)
      break;
    return expr(Record->getType());
  }
  case cv::LF_STRUCTURE:
  case cv::LF_CLASS:
  case cv::LF_INTERFACE:
  case cv::LF_UNION: {
    const bool Union = Type.kind() == cv::LF_UNION;
    llvm::StringRef Name;
    if (Union) {
      if (auto Record = deserialize<cv::UnionRecord>(Type))
        Name = Record->getName();
    } else if (auto Record = deserialize<cv::ClassRecord>(Type)) {
      Name = Record->getName();
    }
    if (isUnnamed(Name))
      if (auto Definition = definition(Index))
        return makeRecord(record(*Definition));
    return makeNamed(Name.str(), Union ? TypeKind::Union : TypeKind::Struct);
  }
  case cv::LF_ENUM: {
    auto Record = deserialize<cv::EnumRecord>(Type);
    if (!Record)
      break;
    if (isUnnamed(Record->getName()))
      if (auto Definition = definition(Index))
        return makeEnum(enumeration(*Definition));
    return makeNamed(Record->getName().str(), TypeKind::Enum);
  }
  case cv::LF_PROCEDURE: {
    auto Record = deserialize<cv::ProcedureRecord>(Type);
    if (!Record)
      break;
    return makeFunction(function(Record->getReturnType(), Record->getCallConv(),
                                 Record->getArgumentList()));
  }
  case cv::LF_MFUNCTION: {
    auto Record = deserialize<cv::MemberFunctionRecord>(Type);
    if (!Record)
      break;
    return makeFunction(function(Record->getReturnType(), Record->getCallConv(),
                                 Record->getArgumentList()));
  }
  default:
    break;
  }
  // A record kind local types do not model reads as bytes of its size.
  const uint64_t Size = sizeOf(Index);
  return Size ? makeArray(makeBuiltin(Builtin::UChar), Size)
              : makeBuiltin(Builtin::Void);
}

FieldCollector Importer::fields(cv::TypeIndex List) {
  FieldCollector Collector;
  std::optional<cv::TypeIndex> Next = List;
  // A long field list continues in another, which a well-formed stream never
  // loops back from; the bound guards a malformed one.
  for (unsigned Lists = 0; Next && !Next->isNoneType() && Lists < 4096;
       ++Lists) {
    const cv::CVType Type = Types.getType(*Next);
    Collector.Continuation.reset();
    if (Type.kind() != cv::LF_FIELDLIST)
      break;
    auto Record = deserialize<cv::FieldListRecord>(Type);
    if (!Record)
      break;
    if (auto Err = cv::visitMemberRecordStream(Record->Data, Collector)) {
      llvm::consumeError(std::move(Err));
      break;
    }
    Next = Collector.Continuation;
  }
  return Collector;
}

std::shared_ptr<const RecordDef> Importer::record(cv::TypeIndex Definition) {
  const cv::CVType Type = Types.getType(Definition);
  auto Result = std::make_shared<RecordDef>();
  cv::TypeIndex List;
  if (Type.kind() == cv::LF_UNION) {
    auto Record = deserialize<cv::UnionRecord>(Type);
    if (!Record)
      return Result;
    Result->Union = true;
    Result->Size = Record->getSize();
    List = Record->getFieldList();
  } else {
    auto Record = deserialize<cv::ClassRecord>(Type);
    if (!Record)
      return Result;
    Result->Size = Record->getSize();
    List = Record->getFieldList();
  }
  FieldCollector Collector = fields(List);
  std::vector<FlatMember> Flat;
  Flat.reserve(Collector.Fields.size());
  for (const FieldCollector::Data &Field : Collector.Fields) {
    FlatMember Member;
    Member.Name = Field.Name;
    Member.BitOffset = Field.Offset * 8;
    Member.StorageBitOffset = Member.BitOffset;
    if (!Field.Type.isSimple() &&
        Types.getType(Field.Type).kind() == cv::LF_BITFIELD) {
      if (auto Bits =
              deserialize<cv::BitFieldRecord>(Types.getType(Field.Type))) {
        Member.Type = expr(Bits->getType());
        Member.BitWidth = Bits->getBitSize();
        Member.BitOffset += Bits->getBitOffset();
        Member.StorageBits = sizeOf(Bits->getType()) * 8;
        Flat.push_back(std::move(Member));
        continue;
      }
    }
    Member.Type = expr(Field.Type);
    Member.StorageBits = sizeOf(Field.Type) * 8;
    Flat.push_back(std::move(Member));
  }
  Result->Members = nestFlatMembers(Flat, Result->Union);
  // The nesting must lay the members out exactly as MSVC listed them.
  const std::vector<FlatMember> Back = flattenMembers(*Result);
  bool Holds = Back.size() == Flat.size();
  for (size_t I = 0; Holds && I < Back.size(); ++I)
    Holds = Back[I].Name == Flat[I].Name &&
            Back[I].BitOffset == Flat[I].BitOffset &&
            Back[I].BitWidth == Flat[I].BitWidth;
  if (!Holds)
    ++Mismatches;
  return Result;
}

std::shared_ptr<const EnumDef> Importer::enumeration(cv::TypeIndex Definition) {
  auto Result = std::make_shared<EnumDef>();
  auto Record = deserialize<cv::EnumRecord>(Types.getType(Definition));
  if (!Record)
    return Result;
  const cv::TypeIndex Underlying = Record->getUnderlyingType();
  if (Underlying.isSimple())
    if (auto Kind = builtinOf(Underlying.getSimpleKind()))
      Result->Underlying = *Kind;
  Result->Values = fields(Record->getFieldList()).Enumerators;
  // CodeView may spell -1 as 0xFFFFFFFF; a value takes the underlying
  // type's width and, when that type is signed, its sign.
  const unsigned Bits =
      builtinBytes(Result->Underlying, Library.dataModel()) * 8;
  if (Bits && Bits < 64)
    for (Enumerator &Value : Result->Values) {
      const uint64_t Mask = (uint64_t(1) << Bits) - 1;
      uint64_t Truncated = Value.Value & Mask;
      if (builtinInfo(Result->Underlying).Signed &&
          (Truncated >> (Bits - 1)) & 1)
        Truncated |= ~Mask;
      Value.Value = Truncated;
    }
  return Result;
}

std::shared_ptr<const FunctionDef>
Importer::function(cv::TypeIndex Return, cv::CallingConvention Convention,
                   cv::TypeIndex Arguments) {
  auto Result = std::make_shared<FunctionDef>();
  Result->Convention = convention(Convention);
  Result->Return = expr(Return);
  if (Arguments.isSimple() || Arguments.isNoneType())
    return Result;
  auto List = deserialize<cv::ArgListRecord>(Types.getType(Arguments));
  if (!List)
    return Result;
  for (cv::TypeIndex Argument : List->getIndices()) {
    // CodeView ends a variadic list with an argument of no type.
    if (Argument.isNoneType()) {
      Result->Variadic = true;
      continue;
    }
    Result->Params.push_back({std::string(), expr(Argument)});
  }
  return Result;
}

PDBTypeImportStats Importer::run() {
  PDBTypeImportStats Stats;
  const uint32_t First = Tpi.TypeIndexBegin();
  const uint32_t Count = Tpi.getNumTypeRecords();
  // Where each union, structure and enumeration is defined.
  for (uint32_t I = 0; I < Count; ++I) {
    const cv::TypeIndex Index(First + I);
    const cv::CVType Type = Types.getType(Index);
    llvm::StringRef Name, Unique;
    bool Forward = true;
    if (isClassKind(Type.kind())) {
      if (auto Record = deserialize<cv::ClassRecord>(Type)) {
        Forward = Record->isForwardRef();
        Name = Record->getName();
        Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
      }
    } else if (Type.kind() == cv::LF_UNION) {
      if (auto Record = deserialize<cv::UnionRecord>(Type)) {
        Forward = Record->isForwardRef();
        Name = Record->getName();
        Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
      }
    } else if (Type.kind() == cv::LF_ENUM) {
      if (auto Record = deserialize<cv::EnumRecord>(Type)) {
        Forward = Record->isForwardRef();
        Name = Record->getName();
        Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
      }
    }
    if (Forward)
      continue;
    if (!Unique.empty())
      ByUniqueName.try_emplace(Unique, Index);
    ByName.try_emplace(Name, Index);
  }

  // Every named type, in the order of its first record, defined once.
  for (uint32_t I = 0; I < Count; ++I) {
    const cv::TypeIndex Index(First + I);
    const cv::CVType Type = Types.getType(Index);
    const bool Class = isClassKind(Type.kind());
    if (!Class && Type.kind() != cv::LF_UNION && Type.kind() != cv::LF_ENUM)
      continue;
    llvm::StringRef Name;
    if (Class) {
      if (auto Record = deserialize<cv::ClassRecord>(Type))
        Name = Record->getName();
    } else if (Type.kind() == cv::LF_UNION) {
      if (auto Record = deserialize<cv::UnionRecord>(Type))
        Name = Record->getName();
    } else if (auto Record = deserialize<cv::EnumRecord>(Type)) {
      Name = Record->getName();
    }
    if (isUnnamed(Name) || Library.find(Name))
      continue;
    LocalType Local;
    Local.Name = Name.str();
    Local.Origin = TypeOrigin::PDB;
    const std::optional<cv::TypeIndex> Definition = definition(Index);
    if (Type.kind() == cv::LF_ENUM) {
      Local.Kind = TypeKind::Enum;
      if (!Definition) {
        ++Stats.DeclaredOnly;
        Local.Enum = std::make_shared<EnumDef>();
      } else {
        Local.Enum = enumeration(*Definition);
        ++Stats.Enums;
      }
      Library.define(std::move(Local));
      continue;
    }
    Local.Kind = Class ? TypeKind::Struct : TypeKind::Union;
    if (!Definition) {
      auto Declared = std::make_shared<RecordDef>();
      Declared->Union = !Class;
      Declared->Defined = false;
      Local.Record = std::move(Declared);
      ++Stats.DeclaredOnly;
    } else {
      const unsigned Before = Mismatches;
      Local.Record = record(*Definition);
      if (Mismatches != Before)
        Stats.NestingMismatches.push_back(Local.Name);
      ++(Class ? Stats.Structs : Stats.Unions);
    }
    Library.define(std::move(Local));
  }
  return Stats;
}

/// The #pragma pack that put a member below its natural alignment, found
/// from the offsets the compiler chose; zero when every member is aligned.
uint32_t inferredPack(const TypeLibrary &Library, const RecordDef &Record,
                      uint64_t Base) {
  uint32_t Pack = 0;
  for (const Member &Field : Record.Members) {
    if (!Field.Type)
      continue;
    const uint64_t Bit = Base + Field.BitOffset;
    if (Field.isAnonymous() && Field.Type->Record) {
      const uint32_t Inner = inferredPack(Library, *Field.Type->Record, Bit);
      if (Inner && (!Pack || Inner < Pack))
        Pack = Inner;
      continue;
    }
    if (Field.isBitfield() || Bit % 8)
      continue;
    const uint64_t Offset = Bit / 8;
    const uint32_t Alignment = Library.alignOf(*Field.Type);
    if (Alignment <= 1 || Offset % Alignment == 0)
      continue;
    // The largest power of two that divides the offset.
    const auto Allowed = static_cast<uint32_t>(Offset & (~Offset + 1));
    if (!Pack || Allowed < Pack)
      Pack = Allowed;
  }
  return Pack;
}

} // namespace

llvm::Expected<PDBTypeImportStats> importPDBTypes(llvm::pdb::PDBFile &File,
                                                  TypeLibrary &Into) {
  if (!File.hasPDBTpiStream())
    return PDBTypeImportStats();
  auto Tpi = File.getPDBTpiStream();
  if (!Tpi)
    return Tpi.takeError();
  PDBTypeImportStats Stats = Importer(*Tpi, Into).run();
  // Packing needs every type's alignment, so it waits for the whole stream.
  for (const LocalType *Type : Into.types()) {
    if (!Type->Record || !Type->Record->Defined)
      continue;
    const uint32_t Pack = inferredPack(Into, *Type->Record, 0);
    if (!Pack)
      continue;
    LocalType Packed = *Type;
    auto Record = std::make_shared<RecordDef>(*Type->Record);
    Record->Pack = Pack;
    Packed.Record = std::move(Record);
    Into.define(std::move(Packed));
  }
  return Stats;
}

llvm::Expected<std::unique_ptr<TypeLibrary>>
importPDBTypes(llvm::StringRef Path, PDBTypeImportStats *Stats) {
  std::unique_ptr<llvm::pdb::IPDBSession> Session;
  if (auto Err = llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native,
                                           Path, Session))
    return std::move(Err);
  auto &File = static_cast<llvm::pdb::NativeSession &>(*Session).getPDBFile();
  DataModel Model = DataModel::LLP64;
  if (File.hasPDBDbiStream()) {
    auto Dbi = File.getPDBDbiStream();
    if (!Dbi)
      return Dbi.takeError();
    Model = dataModelOf(Dbi->getMachineType());
  }
  auto Library = std::make_unique<TypeLibrary>(Model);
  auto Read = importPDBTypes(File, *Library);
  if (!Read)
    return Read.takeError();
  if (Stats)
    *Stats = *Read;
  return std::move(Library);
}

} // namespace neverd::types
