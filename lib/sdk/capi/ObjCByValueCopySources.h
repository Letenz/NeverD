#ifndef NEVERD_SDK_CAPI_OBJCBYVALUECOPYSOURCES_H
#define NEVERD_SDK_CAPI_OBJCBYVALUECOPYSOURCES_H

#include "ObjCNativeDependencies.h"
#include "ObjCNativeSwiftReceiverSources.h"

namespace neverd::sdk {
namespace objc_value_copy_detail {
// These ordinary declaration owners may accompany a private copy. Other
// lifetime, virtual or storage receipts need their own publication proof.
inline bool ordinaryHint(SourceCallTypeHint H) {
  using K = SourceCallTypeHint::Kind;
  switch (H.CallKind) {
  case K::Native:
  case K::ObjCMessage:
  case K::ObjCRuntimeCall:
  case K::DarwinRuntimeCall:
  case K::RuntimeSelector:
  case K::RuntimeClass:
  case K::RuntimeIvarOffset:
  case K::RuntimeProfileCounterStorage:
  case K::DarwinRuntimeGlobalAddress:
    break;
  default:
    return false;
  }
  if (H.SwiftOpaqueValue || H.SwiftConsumedInput || H.SwiftWitnessFrame ||
      H.SwiftValueConstructor || H.Virtual || H.NativeSwiftReceiver)
    return false;
  H.CallKind = K::Native;
  H.ReturnedArgument.reset();
  H.RuntimeObjCResultType.reset();
  H.Selector.clear();
  H.OwnerClass.clear();
  H.SelectorReferenceAddress = 0;
  H.Receiver.reset();
  H.ByValueCopy.reset();
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
         A.ByValueCopy == B.ByValueCopy &&
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
inline bool sameBody(const HighFunc &Expected, const HighFunc &Actual) {
  size_t Budget = 100000;
  return sameCompleteSourceBody(
      Expected, Actual,
      [&](const ExprPtr &E, const ExprPtr &A) {
        return sameExpression(E, A, Budget);
      },
      Budget);
}
} // namespace objc_value_copy_detail

inline bool objCByValueCopySourceCallBound(
    const HighExpr &Expression, const BinaryImage &Image,
    const PipelineResult &Result, const HighFunc &Function,
    const std::map<va_t, const HighFunc *> &Functions) {
  using native_swift_receiver_detail::BodyProof;
  if (!Expression.SourceCallHint || !Expression.SourceCallHint->ByValueCopy ||
      Expression.SourceCallHint->NativeSwiftReceiver || !Result.Success ||
      Result.SourceImage != &Image || !Function.SourceTypeHint)
    return false;
  const auto Current =
      native_source_detail::currentFunction(Result, Function.Entry);
  const auto Declaration = objcMethodSourceTypeHint(Image, Function.Entry);
  if (!Current || !Declaration || !Current->Med->SourceTypeHint ||
      !equalSourceABIs(*Declaration, *Function.SourceTypeHint) ||
      !equalSourceABIs(*Declaration, *Current->Med->SourceTypeHint) ||
      !validateObjCByValueCopyBindings(Image, Current->Low, *Current->Med))
    return false;

  // Rebuild from current machine/declarations, never from saved argument
  // values or cached signatures. Include bounded native dependencies so the
  // authoritative pipeline can rediscover intervening call and frame effects.
  std::map<va_t, const LowFunc *> Bodies;
  for (const auto &F : Result.LowFuncs)
    if (!Bodies.emplace(F.Entry, &F).second)
      return false;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.SourceTypeHints.emplace(Function.Entry, *Declaration);
  std::vector<va_t> Pending{Function.Entry};
  while (!Pending.empty()) {
    const auto Entry = Pending.back();
    Pending.pop_back();
    if (!Options.OnlyFunctionEntries.insert(Entry).second)
      continue;
    const auto I = Bodies.find(Entry);
    if (I == Bodies.end() || !I->second->hasCompleteLiftCoverage() ||
        I->second->DecodedInstructionCount > 1024 ||
        Options.OnlyFunctionEntries.size() > 32)
      return false;
    for (const auto &B : I->second->Blocks)
      for (const auto &Op : B.Ops)
        if (const auto S = sourceCallOccurrenceKey(Op);
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
  if (!Complete || !Rebuilt ||
      !validateObjCByValueCopyBindings(Image, Rebuilt->Low, *Rebuilt->Med))
    return false;
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Image);
  std::map<va_t, std::string> Names;
  for (const auto &F : Result.MedFuncs)
    Names.emplace(F.Entry, F.Name);
  Converter.setFuncNames(&Names);
  Converter.setJumpTables(Current->Low->JumpTables);
  const auto MedReplay = Converter.convert(*Current->Med, Image.Arch);
  if (!objc_value_copy_detail::sameBody(*Rebuilt->High, *Current->High) ||
      !objc_value_copy_detail::sameBody(*Rebuilt->High, MedReplay))
    return false;
  const auto Bound =
      bindObjCSourceReferences(*Rebuilt->High, Image, nullptr, &Functions);
  if (!Bound.Limitation.empty() ||
      !objc_value_copy_detail::sameBody(Bound.Function, Function))
    return false;
  BodyProof Expected{Image, Bound.Function}, Actual{Image, Function};
  const auto ExpectedCalls = Expected.calls(true), Calls = Actual.calls(true);
  if (!ExpectedCalls || !Calls || Calls->empty() ||
      Calls->size() != ExpectedCalls->size())
    return false;
  const auto Call = Calls->find(Expression.SourceCallHint->ByValueCopy->Site);
  if (Call == Calls->end() || Call->second != &Expression)
    return false;
  return objcSourceCallBound(Expression, Image, Functions, nullptr, nullptr,
                             &Function, nullptr, nullptr, false, true);
}
} // namespace neverd::sdk
#endif
