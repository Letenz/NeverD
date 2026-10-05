#ifndef NEVERD_SDK_CAPI_SOURCESWIFTOPAQUEVALUEPROJECTION_H
#define NEVERD_SDK_CAPI_SOURCESWIFTOPAQUEVALUEPROJECTION_H

#include "../../loader/SourceUnwind.h"
#include "ObjCNativeCurrentFunction.h"
#include "ObjCSourceBindings.h"
#include "SourceExpressionIdentity.h"
#include "SourceReceiptOccurrences.h"

#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/Swift/SwiftOpaqueValueEffects.h"
#include "neverd/pipeline/NativeSourceHints.h"

namespace neverd::sdk {
namespace swift_opaque_value_detail {
// This first consumer accepts only ordinary copy/destroy and equality calls,
// plus independently bound address expressions. Other annotation owners must
// have their own integration; an unexamined annotation cannot ride this replay.
inline bool plainHint(const SourceCallTypeHint &H) {
  return !H.WeakImport && !H.DoesNotReturn && !H.ReturnedArgument &&
         !H.RuntimeObjCResultType && H.Selector.empty() &&
         H.OwnerClass.empty() && !H.SelectorReferenceAddress &&
         H.BorrowedByteInputs.empty() && H.CanonicalBooleanInputs.empty() &&
         H.SwiftStringInputs.empty() && H.SwiftStaticStringInputs.empty() &&
         !H.Format && !H.NilTerminated && !H.SwiftTypeMetadata && !H.Receiver &&
         !H.NativeSwiftReceiver && !H.ByValueCopy && !H.SelectorResultUse &&
         !H.SelectorResultTypeUse && !H.SelectorArgumentTypeUse &&
         !H.SelectorForwardingUse && !H.SelectorArgumentStorageUse &&
         !H.ObjCIndirectResultStorage && !H.ByteCount &&
         !H.ImmutablePointerSlot && !H.AddressedFunctionABI &&
         !H.SwiftWitnessUndefDescriptor && !H.FunctionParameterCall &&
         !H.ImmutableNativeCall && !H.ValueWitness && !H.SwiftWitnessFrame &&
         !H.SwiftConsumedInput && !H.SwiftValueConstructor && !H.Virtual;
}
inline bool sameExpression(const ExprPtr &Expected, const ExprPtr &Actual,
                           const BinaryImage &Image, size_t &Budget,
                           unsigned Depth = 0) {
  if (!Expected || !Actual || !Budget || Depth >= 64)
    return false;
  --Budget;
  HighExpr E = *Expected, A = *Actual;
  if (bool(E.SourceCallHint) != bool(A.SourceCallHint))
    return false;
  if (E.SourceCallHint) {
    const auto &X = *E.SourceCallHint, &Y = *A.SourceCallHint;
    using Kind = SourceCallTypeHint::Kind;
    const bool Value = X.SwiftOpaqueValue && Y.SwiftOpaqueValue &&
                       (X.CallKind == Kind::Native ||
                        X.CallKind == Kind::SwiftBooleanProjection);
    const bool Address = E.Operands.empty() && A.Operands.empty() &&
                         (X.CallKind == Kind::DarwinRuntimeGlobalAddress ||
                          X.CallKind == Kind::RuntimeProfileCounterStorage) &&
                         objcSourceCallBound(E, Image, {}) &&
                         objcSourceCallBound(A, Image, {});
    if ((!Value && !Address) || !plainHint(X) || !plainHint(Y) ||
        E.Kind != ExprKind::Call || X.CallKind != Y.CallKind ||
        X.TargetAddress != Y.TargetAddress || X.TargetName != Y.TargetName ||
        X.SwiftOpaqueValue != Y.SwiftOpaqueValue ||
        bool(X.BooleanResult) != bool(Y.BooleanResult) ||
        (X.BooleanResult &&
         (X.BooleanResult->FunctionEntry != Y.BooleanResult->FunctionEntry ||
          X.BooleanResult->Site != Y.BooleanResult->Site)) ||
        !equalSourceABIs(X.Signature, Y.Signature))
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
    if (!sameExpression(Expected->Operands[I], Actual->Operands[I], Image,
                        Budget, Depth + 1))
      return false;
  return !Expected->IndirectTarget ||
         sameExpression(Expected->IndirectTarget, Actual->IndirectTarget, Image,
                        Budget, Depth + 1);
}
inline bool sameBody(const HighFunc &E, const HighFunc &A,
                     const BinaryImage &Image) {
  size_t Budget = 100000;
  return sameCompleteSourceBody(
      E, A,
      [&](const ExprPtr &X, const ExprPtr &Y) {
        return sameExpression(X, Y, Image, Budget);
      },
      Budget);
}
inline std::optional<std::set<SourceCallOccurrenceKey>>
occurrences(const HighFunc &F) {
  return ordinarySourceReceiptOccurrences(
      F, &SourceCallTypeHint::SwiftOpaqueValue);
}
} // namespace swift_opaque_value_detail

/// Full bounded canonical replay, including structured branches and backedges.
/// This compares complete bodies; it never flattens a lifetime into a call
/// list.
class SourceSwiftOpaqueValueProjectionValidator {
  const BinaryImage &Image;
  const PipelineResult &Result;
  NativeSourceCalleeContracts Contracts;
  std::map<va_t, const LowFunc *> Bodies;
  std::map<va_t, SourceFunctionTypeHint> ABIs;

public:
  SourceSwiftOpaqueValueProjectionValidator(const BinaryImage &I,
                                            const PipelineResult &R)
      : Image(I), Result(R), Contracts(nativeSourceCalleeContracts(I, R)) {
    for (const auto &[Entry, C] : Contracts.CurrentCallees)
      if (C.Low && C.Signature) {
        Bodies.emplace(Entry, C.Low);
        ABIs.emplace(Entry, *C.Signature);
      }
  }
  bool valid(const HighFunc &Function) const {
    using namespace swift_opaque_value_detail;
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
    if (!Low || !Med)
      return Published->empty();
    const auto Hints =
        buildSwiftOpaqueValueCallHints(Image, *Low, ABIs, Bodies);
    bool Marked = false;
    for (const auto &B : Med->Blocks)
      for (const auto &O : B.Ops)
        Marked |= O.SourceCallHint && O.SourceCallHint->SwiftOpaqueValue;
    if (Hints.empty())
      return !Marked && Published->empty();
    const auto Current =
        native_source_detail::currentFunction(Result, Function.Entry);
    if (!Current || Low->DecodedInstructionCount > 256 || Hints.size() > 16 ||
        !validateSwiftOpaqueValueBindings(Image, Low, *Med, &Contracts))
      return false;
    std::set<SourceCallOccurrenceKey> Required;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Function.Entry};
    for (const auto &[Address, H] : Hints) {
      Required.insert(H.SwiftOpaqueValue->Site);
      if (H.CallKind == SourceCallTypeHint::Kind::Native)
        Options.OnlyFunctionEntries.insert(H.TargetAddress);
    }
    if (*Published != Required || Options.OnlyFunctionEntries.size() > 5)
      return false;
    Options.MaxFunctions = Options.OnlyFunctionEntries.size();
    llvm::LLVMContext Context;
    auto Fresh = Pipeline().run(Image, Context, Options);
    if (!Fresh.Success)
      return false;
    // Infer each helper independently from fresh lifting, without the caller's
    // stored hints or an opaque-value permission. Complete machine proofs still
    // certify its dynamic witness operation in the loader.
    for (auto Entry : Options.OnlyFunctionEntries) {
      if (Entry == Function.Entry)
        continue;
      const LowFunc *L = nullptr;
      const MedFunc *M = nullptr;
      const HighFunc *H = nullptr;
      const PipelineFunctionAudit *A = nullptr;
      for (const auto &F : Fresh.LowFuncs)
        if (F.Entry == Entry)
          L = &F;
      for (const auto &F : Fresh.MedFuncs)
        if (F.Entry == Entry)
          M = &F;
      for (const auto &F : Fresh.HighFuncs)
        if (F.Entry == Entry)
          H = &F;
      for (const auto &F : Fresh.FunctionAudits)
        if (F.Entry == Entry)
          A = &F;
      if (!L || !M || !H || !A)
        return false;
      std::string Error;
      auto Hint = inferNativeSourceTypeHint(Image, *M, *H, *A, Error, L);
      // Display names have no ABI authority. Preserve the current callee's
      // spelling only after independent inference supplies every carrier.
      if (Hint && ABIs.count(Entry) &&
          Hint->Parameters.size() == ABIs.at(Entry).Parameters.size())
        for (size_t I = 0; I < Hint->Parameters.size(); ++I)
          Hint->Parameters[I].Name = ABIs.at(Entry).Parameters[I].Name;
      if (!Hint || !ABIs.count(Entry) ||
          !equalSourceABIs(*Hint, ABIs.at(Entry)))
        return false;
      Options.SourceTypeHints.emplace(Entry, *Hint);
    }
    Fresh = Pipeline().run(Image, Context, Options);
    auto FreshContracts = nativeSourceCalleeContracts(Image, Fresh);
    const LowFunc *L = nullptr;
    const MedFunc *M = nullptr;
    const HighFunc *H = nullptr;
    const PipelineFunctionAudit *A = nullptr;
    for (const auto &F : Fresh.LowFuncs)
      if (F.Entry == Function.Entry)
        L = &F;
    for (const auto &F : Fresh.MedFuncs)
      if (F.Entry == Function.Entry)
        M = &F;
    for (const auto &F : Fresh.HighFuncs)
      if (F.Entry == Function.Entry)
        H = &F;
    for (const auto &F : Fresh.FunctionAudits)
      if (F.Entry == Function.Entry)
        A = &F;
    if (!Fresh.Success || !L || !M || !H || !A)
      return false;
    std::string Error;
    auto Entry = inferNativeSourceTypeHint(
        Image, *M, *H, *A, Error, L,
        Current->Med->SourceTypeHint->ReturnType->Kind == NdTypeKind::Struct,
        &FreshContracts);
    if (!Entry || !equalSourceABIs(*Entry, *Current->Med->SourceTypeHint))
      return false;
    Options.SourceTypeHints.emplace(Function.Entry, *Entry);
    Fresh = Pipeline().run(Image, Context, Options);
    const auto Rebuilt =
        native_source_detail::currentFunction(Fresh, Function.Entry);
    if (!Fresh.Success || !Rebuilt)
      return false;
    const auto ExpectedSites = occurrences(*Rebuilt->High);
    if (!ExpectedSites || *ExpectedSites != Required)
      return false;
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    std::map<va_t, std::string> Names;
    for (const auto &F : Result.MedFuncs)
      Names.emplace(F.Entry, F.Name);
    Converter.setFuncNames(&Names);
    Converter.setJumpTables(Current->Low->JumpTables);
    const auto MedReplay = Converter.convert(*Current->Med, Image.Arch);
    if (!sameBody(*Rebuilt->High, *Current->High, Image) ||
        !sameBody(*Rebuilt->High, MedReplay, Image))
      return false;
    const auto Bound = bindObjCSourceReferences(*Rebuilt->High, Image);
    return Bound.Limitation.empty() &&
           sameBody(Bound.Function, Function, Image);
  }
};
} // namespace neverd::sdk
#endif
