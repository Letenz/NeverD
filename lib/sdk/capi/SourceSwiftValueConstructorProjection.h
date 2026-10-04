#ifndef NEVERD_SDK_CAPI_SOURCESWIFTVALUECONSTRUCTORPROJECTION_H
#define NEVERD_SDK_CAPI_SOURCESWIFTVALUECONSTRUCTORPROJECTION_H

#include "ObjCNativeCurrentFunction.h"
#include "ObjCNativeDependencies.h"
#include "ObjCSourceBindings.h"
#include "SourceExpressionIdentity.h"

#include "neverd/ir/high/MedToHigh.h"

namespace neverd::sdk {
namespace swift_value_constructor_detail {
// This replay accepts only the listed ordinary declarations/address owners.
// A different lifetime, virtual-target or frame receipt needs its own proof.
inline bool ordinaryHint(SourceCallTypeHint H) {
  using K = SourceCallTypeHint::Kind;
  switch (H.CallKind) {
  case K::Native:
  case K::ObjCRuntimeCall:
  case K::ObjCSuper2:
  case K::RuntimeClass:
  case K::RuntimeSelector:
  case K::RuntimeIvarOffset:
  case K::RuntimeProfileCounterStorage:
  case K::RuntimeSwiftTypeMetadataAddress:
    break;
  default:
    return false;
  }
  if (H.SwiftOpaqueValue || H.SwiftConsumedInput || H.SwiftWitnessFrame ||
      H.Virtual || H.NativeSwiftReceiver || H.ByValueCopy)
    return false;
  H.CallKind = K::Native;
  H.SwiftValueConstructor.reset();
  H.ReturnedArgument.reset();
  H.RuntimeObjCResultType.reset();
  H.Selector.clear();
  H.OwnerClass.clear();
  H.SelectorReferenceAddress = 0;
  H.Receiver.reset();
  H.SwiftTypeMetadata.reset();
  H.ByteCount = 0;
  return objc_binding_detail::plainNativeBinding(H);
}
inline bool sameHint(const SourceCallTypeHint &A, const SourceCallTypeHint &B) {
  return ordinaryHint(A) && ordinaryHint(B) && A.CallKind == B.CallKind &&
         A.TargetAddress == B.TargetAddress && A.TargetName == B.TargetName &&
         A.Selector == B.Selector && A.OwnerClass == B.OwnerClass &&
         A.SelectorReferenceAddress == B.SelectorReferenceAddress &&
         A.Receiver == B.Receiver && A.ByteCount == B.ByteCount &&
         A.ReturnedArgument == B.ReturnedArgument &&
         A.RuntimeObjCResultType == B.RuntimeObjCResultType &&
         A.SwiftTypeMetadata == B.SwiftTypeMetadata &&
         A.SwiftValueConstructor == B.SwiftValueConstructor &&
         equalSourceABIs(A.Signature, B.Signature);
}
inline bool sameExpression(const ExprPtr &Expected, const ExprPtr &Actual,
                           size_t &Budget, unsigned Depth = 0) {
  if (!Expected || !Actual || !Budget || Depth >= 64)
    return false;
  --Budget;
  HighExpr E = *Expected, A = *Actual;
  if (bool(E.SourceCallHint) != bool(A.SourceCallHint))
    return false;
  if (E.SourceCallHint) {
    if (E.Kind != ExprKind::Call ||
        !sameHint(*E.SourceCallHint, *A.SourceCallHint))
      return false;
    A.SourceCallHint = E.SourceCallHint;
  }
  E.Operands.clear();
  A.Operands.clear();
  E.IndirectTarget.reset();
  A.IndirectTarget.reset();
  if (!sameSourceExpressionIdentity(E, A, Budget) ||
      Expected->Operands.size() != Actual->Operands.size() ||
      bool(Expected->IndirectTarget) != bool(Actual->IndirectTarget))
    return false;
  for (size_t I = 0; I < Expected->Operands.size(); ++I)
    if (!sameExpression(Expected->Operands[I], Actual->Operands[I], Budget,
                        Depth + 1))
      return false;
  return !Expected->IndirectTarget ||
         sameExpression(Expected->IndirectTarget, Actual->IndirectTarget,
                        Budget, Depth + 1);
}
inline bool sameBody(const HighFunc &E, const HighFunc &A) {
  size_t Budget = 100000;
  return sameCompleteSourceBody(
      E, A,
      [&](const ExprPtr &X, const ExprPtr &Y) {
        return sameExpression(X, Y, Budget);
      },
      Budget);
}
inline std::optional<std::set<SourceCallOccurrenceKey>>
occurrences(const HighFunc &F) {
  std::set<SourceCallOccurrenceKey> Sites;
  size_t Budget = 100000;
  bool Invalid = false;
  // The shared bounded structural walk below also rejects exception clauses.
  const auto Walk = [&](const ExprPtr &E, const ExprPtr &) {
    std::vector<ExprPtr> Pending{E};
    while (!Pending.empty() && Budget) {
      --Budget;
      auto V = Pending.back();
      Pending.pop_back();
      if (!V)
        continue;
      if (V->SourceCallHint && V->SourceCallHint->SwiftValueConstructor) {
        const auto &R = *V->SourceCallHint->SwiftValueConstructor;
        Invalid |= V->Kind != ExprKind::Call || V->IsIndirectCall ||
                   V->IndirectTarget || R.FunctionEntry != F.Entry ||
                   !Sites.insert(R.Site).second;
      }
      V->forEachChildExpr([&](const ExprPtr &C) {
        if (Pending.size() >= Budget)
          Budget = 0;
        else
          Pending.push_back(C);
      });
    }
    return Budget != 0;
  };
  if (!sameStructuredSourceBody(F, F, Walk, Budget) || Invalid)
    return std::nullopt;
  return Sites;
}
} // namespace swift_value_constructor_detail

/// Independent bounded re-lifting/inference followed by complete structural
/// replay. Declarations describe carriers; they cannot bypass this body proof.
class SourceSwiftValueConstructorProjectionValidator {
  const BinaryImage &Image;
  const PipelineResult &Result;

public:
  SourceSwiftValueConstructorProjectionValidator(const BinaryImage &I,
                                                 const PipelineResult &R)
      : Image(I), Result(R) {}
  bool valid(const HighFunc &Function) const {
    using namespace swift_value_constructor_detail;
    const auto Published = occurrences(Function);
    if (!Published)
      return false;
    const LowFunc *Low = nullptr;
    const MedFunc *Med = nullptr;
    for (const auto &F : Result.LowFuncs)
      if (F.Entry == Function.Entry)
        Low = &F;
    for (const auto &F : Result.MedFuncs)
      if (F.Entry == Function.Entry)
        Med = &F;
    bool Relevant = bool(
        swiftMangledFixedRecordConstructorDeclaration(Image, Function.Entry));
    Relevant |=
        (Function.SourceTypeHint &&
         requiresSwiftFixedRecordConstructorProof(*Function.SourceTypeHint)) ||
        (Med && Med->SourceTypeHint &&
         requiresSwiftFixedRecordConstructorProof(*Med->SourceTypeHint));
    bool Marked = false;
    if (Med)
      for (const auto &B : Med->Blocks)
        for (const auto &O : B.Ops)
          if (O.SourceCallHint) {
            Marked |= bool(O.SourceCallHint->SwiftValueConstructor);
            Relevant |= requiresSwiftFixedRecordConstructorProof(
                O.SourceCallHint->Signature);
          }
    if (Low)
      for (const auto &B : Low->Blocks)
        for (const auto &O : B.Ops)
          if (const auto S = sourceCallOccurrenceKey(O); S && S->StaticTarget)
            Relevant |= bool(swiftMangledFixedRecordConstructorDeclaration(
                Image, *S->StaticTarget));
    if (!Relevant)
      return !Marked && Published->empty();
    const auto Current =
        native_source_detail::currentFunction(Result, Function.Entry);
    const auto Contracts = nativeSourceCalleeContracts(Image, Result);
    if (!Result.Success || Result.SourceImage != &Image || !Current ||
        !validateSwiftValueConstructorBindings(Image, Low, *Med, &Contracts))
      return false;
    // Rebuild the bounded current dependency group, without seeding any ABI
    // from saved MedIR, cached signatures or the candidate source itself.
    std::map<va_t, const LowFunc *> Bodies;
    for (const auto &F : Result.LowFuncs)
      if (!Bodies.emplace(F.Entry, &F).second)
        return false;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    std::vector<va_t> Pending{Function.Entry};
    while (!Pending.empty()) {
      auto Entry = Pending.back();
      Pending.pop_back();
      if (!Options.OnlyFunctionEntries.insert(Entry).second)
        continue;
      auto I = Bodies.find(Entry);
      if (I == Bodies.end() || !I->second->hasCompleteLiftCoverage() ||
          I->second->DecodedInstructionCount > 1024 ||
          Options.OnlyFunctionEntries.size() > 32)
        return false;
      for (const auto &B : I->second->Blocks)
        for (const auto &O : B.Ops)
          if (const auto S = sourceCallOccurrenceKey(O);
              S && S->StaticTarget && Bodies.count(*S->StaticTarget))
            Pending.push_back(*S->StaticTarget);
    }
    Options.MaxFunctions = Options.OnlyFunctionEntries.size();
    llvm::LLVMContext Context;
    PipelineResult Fresh;
    bool Complete = false;
    for (unsigned Round = 0; Round != 16; ++Round) {
      Fresh = Pipeline().run(Image, Context, Options);
      if (!Fresh.Success)
        return false;
      std::map<va_t, std::string> Diagnostics;
      if (!inferObjCNativeDependencies(Image, Fresh, Options, Diagnostics,
                                       Options.OnlyFunctionEntries)) {
        Complete = true;
        break;
      }
    }
    const auto Rebuilt =
        native_source_detail::currentFunction(Fresh, Function.Entry);
    if (!Complete || !Rebuilt)
      return false;
    const auto FreshContracts = nativeSourceCalleeContracts(Image, Fresh);
    if (!validateSwiftValueConstructorBindings(
            Image, Rebuilt->Low, *Rebuilt->Med, &FreshContracts) ||
        occurrences(*Rebuilt->High) != Published)
      return false;
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    std::map<va_t, std::string> Names;
    for (const auto &F : Result.MedFuncs)
      Names.emplace(F.Entry, F.Name);
    Converter.setFuncNames(&Names);
    Converter.setJumpTables(Current->Low->JumpTables);
    const auto MedReplay = Converter.convert(*Current->Med, Image.Arch);
    if (!sameBody(*Rebuilt->High, *Current->High) ||
        !sameBody(*Rebuilt->High, MedReplay))
      return false;
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &F : Fresh.HighFuncs)
      Functions.emplace(F.Entry, &F);
    auto Bound =
        bindObjCSourceReferences(*Rebuilt->High, Image, nullptr, &Functions);
    return Bound.Limitation.empty() && sameBody(Bound.Function, Function);
  }
};
} // namespace neverd::sdk
#endif
