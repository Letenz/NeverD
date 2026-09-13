#ifndef NEVERD_SDK_CAPI_OBJCSOURCEBINDINGS_H
#define NEVERD_SDK_CAPI_OBJCSOURCEBINDINGS_H

#include "../../loader/ObjC/ObjCRuntimeData.h"
#include "ObjCSourceProjection.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"

#include "llvm/ADT/StringExtras.h"

#include <optional>

namespace neverd::sdk {

struct ObjCSourceBindingResult {
  HighFunc Function;
  std::string Limitation;
  std::set<va_t> Dependencies;
  std::set<std::string> InstanceLayoutClasses;
  std::set<va_t> AssociationKeys;
};

namespace objc_binding_detail {

inline std::optional<SourceCallTypeHint>
associationKeyHint(const BinaryImage &Image, va_t Address) {
  if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
      !Address)
    return std::nullopt;
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section || !Segment || !Section->isReadable() || Section->isWritable() ||
      !Segment->isReadable() || Segment->isWritable() ||
      (Section->Type & llvm::MachO::SECTION_TYPE) !=
          llvm::MachO::S_CSTRING_LITERALS ||
      !Image.readVA(Address, 1))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeAssociationKey;
  Hint.TargetAddress = Address;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

inline bool isAssociationKeyConsumer(const HighExpr &Expression,
                                     const BinaryImage &Image) {
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      Expression.IntrinsicId != Intrinsic::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Expression.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  const auto &Hint = *Expression.SourceCallHint;
  if (Hint.CallKind != SourceCallTypeHint::Kind::ObjCRuntimeCall ||
      (Hint.TargetName != "objc_getAssociatedObject" &&
       Hint.TargetName != "objc_setAssociatedObject") ||
      Expression.Operands.size() != Hint.Signature.Parameters.size())
    return false;
  const auto Expected = objcRuntimeSourceCallHint(Image, Hint.TargetAddress);
  return Expected && Expected->TargetName == Hint.TargetName &&
         objc_projection_detail::sameHint(Expected->Signature, Hint.Signature);
}

struct ClassObjectIdentity {
  SourceCallTypeHint::Kind Kind;
  std::string Name;
};

inline std::map<va_t, ClassObjectIdentity>
classObjectIdentities(const BinaryImage &Image) {
  std::map<va_t, ClassObjectIdentity> Result;
  std::set<va_t> Conflicts;
  std::map<std::string, size_t> NameCounts;
  for (const auto &Class : Image.ObjCClasses)
    ++NameCounts[Class.Name];
  const objc::RuntimeData Data(Image);
  auto Publish = [&](va_t Address, ClassObjectIdentity Identity) {
    if (Conflicts.count(Address))
      return;
    auto [It, Inserted] = Result.emplace(Address, Identity);
    if (!Inserted && (It->second.Kind != Identity.Kind ||
                      It->second.Name != Identity.Name)) {
      Result.erase(It);
      Conflicts.insert(Address);
    }
  };
  for (const auto &Class : Image.ObjCClasses) {
    if (Class.Name.empty() || NameCounts[Class.Name] != 1)
      continue;
    const auto RO = Data.classRO(Class.Address);
    const auto Flags = RO ? Data.u32(*RO) : std::nullopt;
    const auto Name = Data.className(Class.Address);
    if (!Flags || (*Flags & 1) || !Name || *Name != Class.Name)
      continue;
    Publish(Class.Address,
            {SourceCallTypeHint::Kind::RuntimeClass, Class.Name});
    const auto Meta = Data.pointer(Class.Address);
    const auto MetaRO = Meta ? Data.classRO(*Meta) : std::nullopt;
    const auto MetaFlags = MetaRO ? Data.u32(*MetaRO) : std::nullopt;
    const auto MetaName = Meta ? Data.className(*Meta) : std::nullopt;
    if (MetaFlags && (*MetaFlags & 1) && MetaName && *MetaName == Class.Name)
      Publish(*Meta, {SourceCallTypeHint::Kind::RuntimeMetaclass, Class.Name});
  }
  return Result;
}

inline std::optional<uint64_t> constantAddress(const HighExpr &Expression,
                                               unsigned Depth = 0) {
  if (Depth > 32 || !Expression.Type || Expression.Type->Size != 8)
    return std::nullopt;
  if (Expression.Kind == ExprKind::Const)
    return Expression.ConstVal;
  if (Expression.Kind == ExprKind::Cast && Expression.Operands.size() == 1 &&
      Expression.Operands[0])
    return constantAddress(*Expression.Operands[0], Depth + 1);
  if (Expression.Kind != ExprKind::BinOp || Expression.Operands.size() != 2 ||
      !Expression.Operands[0] || !Expression.Operands[1])
    return std::nullopt;
  auto Left = constantAddress(*Expression.Operands[0], Depth + 1);
  auto Right = constantAddress(*Expression.Operands[1], Depth + 1);
  if (!Left || !Right)
    return std::nullopt;
  if (Expression.Op == NdOp::INT_ADD)
    return *Left + *Right;
  if (Expression.Op == NdOp::INT_SUB)
    return *Left - *Right;
  return std::nullopt;
}

inline SourceCallTypeHint::Kind runtimeKind(ObjCSourceReference::Kind Kind) {
  using K = SourceCallTypeHint::Kind;
  switch (Kind) {
  case ObjCSourceReference::Kind::Selector:
    return K::RuntimeSelector;
  case ObjCSourceReference::Kind::Class:
    return K::RuntimeClass;
  case ObjCSourceReference::Kind::Metaclass:
    return K::RuntimeMetaclass;
  case ObjCSourceReference::Kind::IvarOffset:
    return K::RuntimeIvarOffset;
  }
  return K::Native;
}

inline bool isRuntimeReference(SourceCallTypeHint::Kind Kind) {
  using K = SourceCallTypeHint::Kind;
  return Kind == K::RuntimeSelector || Kind == K::RuntimeClass ||
         Kind == K::RuntimeMetaclass || Kind == K::RuntimeIvarOffset;
}

} // namespace objc_binding_detail

/// Clone before attaching relocation bindings: other native exports keep the
/// original HighIR. A load from a proven runtime slot is a runtime query; the
/// address of that slot is never itself replaced with the loaded value.
inline ObjCSourceBindingResult
bindObjCSourceReferences(const HighFunc &Function, const BinaryImage &Image) {
  using namespace objc_binding_detail;
  ObjCSourceBindingResult Result{Function, {}, {}, {}, {}};
  const auto ClassObjects = classObjectIdentities(Image);
  std::map<const HighExpr *, ExprPtr> Copies;
  size_t Budget = 1000000;
  auto Fail = [&](const char *Message) {
    if (Result.Limitation.empty())
      Result.Limitation = Message;
  };
  std::function<ExprPtr(const ExprPtr &, unsigned)> Copy;
  Copy = [&](const ExprPtr &Original, unsigned Depth) -> ExprPtr {
    if (!Original)
      return nullptr;
    if (Depth > 200 || !Budget) {
      Fail("source reference projection exceeds its complexity budget");
      return HighExpr::makeUndef(Original->Type ? Original->Type->Size : 8);
    }
    --Budget;
    if (auto Found = Copies.find(Original.get()); Found != Copies.end())
      return Found->second;
    auto Expression = std::make_shared<HighExpr>(*Original);
    Copies.emplace(Original.get(), Expression);
    if (Original->Kind == ExprKind::Load && Original->Type &&
        Original->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Original->MemoryOrdering == NdMemoryOrdering::None &&
        Original->Operands.size() == 1 && Original->Operands[0]) {
      auto Address = constantAddress(*Original->Operands[0]);
      auto Found = Address ? Image.ObjCSourceReferences.find(*Address)
                           : Image.ObjCSourceReferences.end();
      if (Found != Image.ObjCSourceReferences.end()) {
        const auto &Reference = Found->second;
        const bool Ivar =
            Reference.TheKind == ObjCSourceReference::Kind::IvarOffset;
        if (Original->Type->Size == Reference.Size ||
            (Ivar && Reference.Size == 8 && Original->Type->Size == 4)) {
          auto Binding = std::make_shared<SourceCallTypeHint>();
          Binding->CallKind = runtimeKind(Reference.TheKind);
          Binding->TargetAddress = Reference.Address;
          Binding->TargetName = Reference.Name;
          Binding->OwnerClass = Reference.ClassName;
          if (Ivar)
            Result.InstanceLayoutClasses.insert(Reference.ClassName);
          auto &Hint = Binding->Signature;
          Hint.Architecture = Image.Arch;
          Hint.HasExplicitABI = true;
          Hint.ReturnType = Ivar ? NdType::makeInt(Original->Type->Size, false)
                                 : NdType::makePtr(NdType::makeVoid());
          Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                                 getTargetRegInfo(Image.Arch).IntReturnReg, 0,
                                 Hint.ReturnType->Size};
          Expression->Kind = ExprKind::Call;
          Expression->CallAddr = 0;
          Expression->CallTarget.clear();
          Expression->SourceCallHint = std::move(Binding);
          Expression->Operands.clear();
          return Expression;
        }
      }
    }
    if (Expression->Kind == ExprKind::Const && Expression->ConstVal &&
        Image.getSectionFor(Expression->ConstVal))
      Fail("method retains an image address without a relocatable source "
           "binding");
    if (Expression->Kind == ExprKind::Call && Expression->SourceCallHint &&
        Expression->SourceCallHint->CallKind ==
            SourceCallTypeHint::Kind::Native)
      Result.Dependencies.insert(Expression->SourceCallHint->TargetAddress);
    for (size_t Index = 0; Index < Expression->Operands.size(); ++Index) {
      auto &Operand = Expression->Operands[Index];
      // The associated-object API compares key identities and never reads
      // their bytes. Rebuild only this authenticated argument occurrence;
      // ordinary loads, returned addresses, and unrelated calls must retain
      // the unresolved image-data diagnostic. Equal original addresses share
      // one helper across methods, including addresses inside a string.
      if (Index == 1 && Operand &&
          isAssociationKeyConsumer(*Expression, Image)) {
        const auto Address = constantAddress(*Operand);
        auto Hint =
            Address ? associationKeyHint(Image, *Address) : std::nullopt;
        if (Hint) {
          auto Key = HighExpr::makeCall({}, 0, {});
          Key->Type = Operand->Type;
          Key->SourceCallHint =
              std::make_shared<SourceCallTypeHint>(std::move(*Hint));
          Result.AssociationKeys.insert(*Address);
          Operand = std::move(Key);
          continue;
        }
      }
      // A direct class-object address is already the receiver value. This is
      // distinct from the address of a classref slot, which requires a LOAD.
      // Keep this contextual rewrite out of Copies: the same Const node may
      // also occur as an ordinary integer elsewhere in the expression DAG.
      if (Index == 0 && Operand && Expression->Kind == ExprKind::Call &&
          Expression->SourceCallHint &&
          Expression->SourceCallHint->CallKind ==
              SourceCallTypeHint::Kind::ObjCMessage) {
        const auto &Signature = Expression->SourceCallHint->Signature;
        std::string Error;
        if (Signature.Parameters.size() >= 2 &&
            Signature.Parameters.size() == Expression->Operands.size() &&
            Signature.Parameters[0].Type &&
            Signature.Parameters[0].Type->Kind == NdTypeKind::Ptr &&
            validateSourceABI(Signature, Error)) {
          const auto Address = constantAddress(*Operand);
          const auto Object =
              Address ? ClassObjects.find(*Address) : ClassObjects.end();
          if (Object != ClassObjects.end()) {
            auto Binding = std::make_shared<SourceCallTypeHint>();
            Binding->CallKind = Object->second.Kind;
            Binding->TargetAddress = Object->first;
            Binding->TargetName = Object->second.Name;
            auto &Hint = Binding->Signature;
            Hint.Architecture = Image.Arch;
            Hint.HasExplicitABI = true;
            Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
            Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                                   getTargetRegInfo(Image.Arch).IntReturnReg, 0,
                                   8};
            auto Value = HighExpr::makeCall({}, 0, {});
            Value->Type = Operand->Type;
            Value->SourceCallHint = std::move(Binding);
            Operand = std::move(Value);
            continue;
          }
        }
      }
      Operand = Copy(Operand, Depth + 1);
    }
    return Expression;
  };
  std::function<void(std::vector<HighStmt> &, unsigned)> Walk;
  Walk = [&](std::vector<HighStmt> &Body, unsigned Depth) {
    if (Depth > 200) {
      Fail("source reference control flow exceeds its depth budget");
      return;
    }
    for (auto &Statement : Body) {
      forEachExpr(Statement, [&](ExprPtr &Expression) {
        Expression = Copy(Expression, 0);
      });
      Walk(Statement.Body, Depth + 1);
      Walk(Statement.ElseBody, Depth + 1);
      Walk(Statement.DefaultBody, Depth + 1);
      for (auto &Case : Statement.Cases)
        Walk(Case.Body, Depth + 1);
      for (auto &Clause : Statement.EHClauseBodies)
        Walk(Clause, Depth + 1);
    }
  };
  Walk(Result.Function.Body, 0);
  return Result;
}

inline bool
objcSourceCallBound(const HighExpr &Expression, const BinaryImage &Image,
                    const std::map<va_t, const HighFunc *> &Functions) {
  using namespace objc_binding_detail;
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      Expression.IntrinsicId != Intrinsic::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto &Binding = *Expression.SourceCallHint;
  const auto &Hint = Binding.Signature;
  std::string Reason;
  if (!validateSourceABI(Hint, Reason) || Hint.Architecture != Image.Arch ||
      Expression.Operands.size() != Hint.Parameters.size())
    return false;
  if (Binding.CallKind == SourceCallTypeHint::Kind::RuntimeAssociationKey) {
    const auto Expected = associationKeyHint(Image, Binding.TargetAddress);
    return Expected && Binding.TargetName.empty() && Binding.Selector.empty() &&
           Binding.OwnerClass.empty() && !Binding.SelectorReferenceAddress &&
           objc_projection_detail::sameHint(Expected->Signature, Hint);
  }
  if (isRuntimeReference(Binding.CallKind)) {
    auto Found = Image.ObjCSourceReferences.find(Binding.TargetAddress);
    if (Expression.Operands.empty() &&
        Found != Image.ObjCSourceReferences.end() &&
        Binding.CallKind == runtimeKind(Found->second.TheKind) &&
        Binding.TargetName == Found->second.Name &&
        Binding.OwnerClass == Found->second.ClassName)
      return true;
    if (!Expression.Operands.empty() || !Binding.OwnerClass.empty() ||
        (Binding.CallKind != SourceCallTypeHint::Kind::RuntimeClass &&
         Binding.CallKind != SourceCallTypeHint::Kind::RuntimeMetaclass))
      return false;
    const auto Objects = classObjectIdentities(Image);
    const auto Object = Objects.find(Binding.TargetAddress);
    return Object != Objects.end() && Object->second.Kind == Binding.CallKind &&
           Object->second.Name == Binding.TargetName &&
           Hint.ReturnType->Kind == NdTypeKind::Ptr;
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::Native) {
    auto Found = Functions.find(Binding.TargetAddress);
    return Found != Functions.end() && Found->second->SourceTypeHint &&
           objc_projection_detail::sameHint(Hint,
                                            *Found->second->SourceTypeHint);
  }
  if (Binding.CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall) {
    const auto Expected =
        objcRuntimeSourceCallHint(Image, Binding.TargetAddress);
    // HighIR retains the original veneer spelling in CallTarget. The source
    // emitter uses the canonical operation carried by this runtime binding.
    return Expected && Binding.TargetName == Expected->TargetName &&
           Binding.Selector.empty() && Binding.OwnerClass.empty() &&
           !Binding.SelectorReferenceAddress &&
           objc_projection_detail::sameHint(Hint, Expected->Signature);
  }
  return (Binding.CallKind == SourceCallTypeHint::Kind::ObjCMessage ||
          Binding.CallKind == SourceCallTypeHint::Kind::ObjCSuper2) &&
         Hint.Parameters.size() >= 2 && !Binding.Selector.empty();
}

inline std::string
renderObjCAssociationKeyHelpers(const std::set<va_t> &Keys,
                                std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (va_t Key : Keys) {
    const std::string Name = "neverd_objc_association_key_" +
                             llvm::utohexstr(Key, true) + "_address";
    SharedFunctions.insert(Name);
    Source += "\nuintptr_t " + Name +
              "(void) {\n"
              "  static unsigned char key;\n"
              "  return (uintptr_t)&key;\n}\n";
  }
  return Source;
}

} // namespace neverd::sdk
#endif
