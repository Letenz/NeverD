#ifndef NEVERD_SDK_CAPI_SOURCESWIFTCONSUMEDINPUTPROJECTION_H
#define NEVERD_SDK_CAPI_SOURCESWIFTCONSUMEDINPUTPROJECTION_H

#include "ObjCNativeCurrentFunction.h"
#include "ObjCSourceBindings.h"
#include "SourceExpressionIdentity.h"

#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/Swift/SwiftConsumedInputEffects.h"
#include "neverd/pipeline/NativeSourceHints.h"

namespace neverd::sdk {
namespace swift_consumed_input_detail {
inline bool sameExpression(const ExprPtr &Expected, const ExprPtr &Actual,
                           const BinaryImage &Image, size_t &Budget,
                           unsigned Depth = 0) {
  if (!Expected || !Actual || !Budget || Depth >= 64)
    return false;
  --Budget;
  HighExpr E = *Expected, A = *Actual;
  if (bool(E.SourceCallHint) != bool(A.SourceCallHint))
    return false;
  if (E.SourceCallHint && E.SourceCallHint != A.SourceCallHint) {
    const auto &X = *E.SourceCallHint, &Y = *A.SourceCallHint;
    using Kind = SourceCallTypeHint::Kind;
    const bool Runtime = X.CallKind == Kind::SwiftRuntimeCall &&
                         X.SwiftConsumedInput && Y.SwiftConsumedInput;
    const bool Address = E.Operands.empty() && A.Operands.empty() &&
                         (X.CallKind == Kind::DarwinRuntimeGlobalAddress ||
                          X.CallKind == Kind::RuntimeProfileCounterStorage);
    if ((!Runtime && !Address) || E.Kind != ExprKind::Call ||
        X.CallKind != Y.CallKind || X.TargetAddress != Y.TargetAddress ||
        X.TargetName != Y.TargetName || X.ByteCount != Y.ByteCount ||
        X.OwnerClass != Y.OwnerClass || X.WeakImport != Y.WeakImport ||
        X.DoesNotReturn != Y.DoesNotReturn ||
        X.ReturnedArgument != Y.ReturnedArgument ||
        X.SwiftConsumedInput != Y.SwiftConsumedInput ||
        X.requiresUniqueSourceOccurrence() !=
            Y.requiresUniqueSourceOccurrence() ||
        !equalSourceABIs(X.Signature, Y.Signature) ||
        !objcSourceCallBound(E, Image, {}) ||
        !objcSourceCallBound(A, Image, {}))
      return false;
    A.SourceCallHint = E.SourceCallHint;
  }
  E.Operands.clear();
  A.Operands.clear();
  if (!sameSourceExpressionIdentity(E, A, Budget) ||
      Expected->Operands.size() != Actual->Operands.size())
    return false;
  for (size_t I = 0; I < Expected->Operands.size(); ++I)
    if (!sameExpression(Expected->Operands[I], Actual->Operands[I], Image,
                        Budget, Depth + 1))
      return false;
  return true;
}

inline bool sameBody(const HighFunc &Expected, const HighFunc &Actual,
                     const BinaryImage &Image) {
  if (!Expected.SourceTypeHint || !Actual.SourceTypeHint ||
      !equalSourceABIs(*Expected.SourceTypeHint, *Actual.SourceTypeHint) ||
      !equalSourceTypes(Expected.ReturnType, Actual.ReturnType) ||
      Expected.Params.size() != Actual.Params.size() || Actual.DoesNotReturn ||
      Actual.StructuredExceptionRegions || Actual.UnstructuredExceptionRegions)
    return false;
  for (size_t I = 0; I < Expected.Params.size(); ++I)
    if (!equalSourceTypes(Expected.Params[I].Type, Actual.Params[I].Type))
      return false;
  size_t Budget = 100000;
  return sameStraightLineSourceBody(
      Expected, Actual,
      [&](const ExprPtr &E, const ExprPtr &A) {
        return sameExpression(E, A, Image, Budget);
      },
      Budget);
}
} // namespace swift_consumed_input_detail

/// A bounded source-only replay for the first consumed-input consumer. Use
/// the canonical pipeline, including its frame proof, instead of reconstructing
/// its instruction, SSA, stack, initialization or lifetime transfer rules here.
/// Broader structured or multi-call consumers remain unsupported.
class SourceSwiftConsumedInputProjectionValidator {
  const BinaryImage &Image;
  const PipelineResult &Result;
  std::set<va_t> Candidates;

public:
  SourceSwiftConsumedInputProjectionValidator(const BinaryImage &Image,
                                              const PipelineResult &Result)
      : Image(Image), Result(Result) {
    for (const auto &F : Result.LowFuncs)
      for (const auto &B : F.Blocks)
        for (const auto &Op : B.Ops)
          if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
              Op.Inputs[0].isConst() &&
              isSwiftConsumedInputCallTarget(Image, Op.Inputs[0].Offset))
            Candidates.insert(F.Entry);
    for (const auto &F : Result.MedFuncs)
      for (const auto &B : F.Blocks)
        for (const auto &Op : B.Ops)
          if (Op.SourceCallHint && Op.SourceCallHint->SwiftConsumedInput)
            Candidates.insert(F.Entry);
  }

  bool valid(const HighFunc &Function) const {
    size_t Budget = 100000;
    bool Marked = false;
    std::vector<ExprPtr> Pending;
    walkStmts(Function.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) { Pending.push_back(E); });
    });
    while (!Pending.empty()) {
      if (!Budget--)
        return false;
      auto E = Pending.back();
      Pending.pop_back();
      if (!E)
        continue;
      Marked |= E->SourceCallHint && E->SourceCallHint->SwiftConsumedInput;
      E->forEachChildExpr(
          [&](const ExprPtr &Child) { Pending.push_back(Child); });
    }
    if (!Candidates.count(Function.Entry))
      return !Marked;
    if (!Result.Success || Result.SourceImage != &Image)
      return false;
    const auto Current =
        native_source_detail::currentFunction(Result, Function.Entry);
    if (!Current ||
        !validateSwiftConsumedInputBindings(Image, Current->Low, *Current->Med))
      return false;
    const auto Hints = buildSwiftConsumedInputCallHints(Image, *Current->Low);
    if (Hints.empty())
      return !Marked; // An ordinary generic call has no new input permission.
    if (Hints.size() != 1 || Current->Low->Blocks.size() != 1 ||
        Current->Low->DecodedInstructionCount > 256)
      return false;
    unsigned Calls = 0;
    for (const auto &Op : Current->Low->Blocks.front().Ops)
      Calls += Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
    if (Calls != 1)
      return false;

    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.MaxFunctions = 1;
    Options.OnlyFunctionEntries = {Function.Entry};
    auto Fresh = Pipeline().run(Image, Context, Options);
    if (!Fresh.Success || Fresh.LowFuncs.size() != 1 ||
        Fresh.MedFuncs.size() != 1 || Fresh.HighFuncs.size() != 1 ||
        Fresh.FunctionAudits.size() != 1)
      return false;
    std::string Error;
    const auto Entry = inferNativeSourceTypeHint(
        Image, Fresh.MedFuncs.front(), Fresh.HighFuncs.front(),
        Fresh.FunctionAudits.front(), Error, &Fresh.LowFuncs.front());
    if (!Entry || !equalSourceABIs(*Entry, *Current->Med->SourceTypeHint))
      return false;
    Options.SourceTypeHints.emplace(Function.Entry, *Entry);
    Fresh = Pipeline().run(Image, Context, Options);
    const auto Rebuilt =
        native_source_detail::currentFunction(Fresh, Function.Entry);
    if (!Fresh.Success || !Rebuilt)
      return false;

    // Compare both saved representations against fresh decoding. Replaying
    // only the saved MedIR would let an edited operand validate itself.
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    std::map<va_t, std::string> Names;
    for (const auto &F : Result.MedFuncs)
      Names.emplace(F.Entry, F.Name);
    Converter.setFuncNames(&Names);
    Converter.setJumpTables(Current->Low->JumpTables);
    const auto MedReplay = Converter.convert(*Current->Med, Image.Arch);
    using namespace swift_consumed_input_detail;
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
