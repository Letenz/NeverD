#ifndef NEVERD_SDK_CAPI_OBJCSOURCEINPUTS_H
#define NEVERD_SDK_CAPI_OBJCSOURCEINPUTS_H

#include "ObjCSourceBindings.h"

#include "neverd/ir/high/HighSourceInputs.h"

#include <limits>

namespace neverd::sdk {

/// Project a runtime offset address through a callee's proven entry-only read.
/// The original native ABI and callee stay intact. Ordinary source binding
/// resolves the new load from live runtime metadata, never from image bits.
inline HighFunc
snapshotObjCEntryInputs(const HighFunc &Function, const BinaryImage &Image,
                        const std::map<va_t, const HighFunc *> &Functions) {
  if (Function.Body.size() > 256 || Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions)
    return Function;
  int NextId = 0;
  size_t Budget = 4096;
  const auto Scan = [&](auto &&Self, const ExprPtr &Value,
                        unsigned Depth) -> bool {
    if (!Value || !Budget || Depth > 32)
      return false;
    --Budget;
    if (Value->Kind == ExprKind::Var || Value->Kind == ExprKind::Phi)
      NextId = std::max(NextId, Value->Var.Id);
    for (const auto &Output : Value->IntrinsicOutputs)
      NextId = std::max(NextId, Output.Id);
    for (const auto &Operand : Value->Operands)
      if (!Self(Self, Operand, Depth + 1))
        return false;
    return true;
  };
  for (const auto &Statement : Function.Body) {
    if (!Statement.Body.empty() || !Statement.ElseBody.empty() ||
        !Statement.DefaultBody.empty() || !Statement.Cases.empty() ||
        !Statement.EHClauseBodies.empty() ||
        (Statement.Kind != StmtKind::Assign &&
         Statement.Kind != StmtKind::Store &&
         Statement.Kind != StmtKind::Call &&
         Statement.Kind != StmtKind::ExprStmt &&
         Statement.Kind != StmtKind::Return && Statement.Kind != StmtKind::Nop))
      return Function;
    bool Valid = true;
    forEachExpr(Statement,
                [&](const ExprPtr &Value) { Valid &= Scan(Scan, Value, 0); });
    if (!Valid)
      return Function;
  }
  const auto Plain = [](const HighExpr &Value) {
    return Value.IntrinsicId == Intrinsic::None &&
           Value.IntrinsicOutputs.empty() &&
           Value.MemoryOrdering == NdMemoryOrdering::None &&
           Value.MemoryAddressSpace == NdMemoryAddressSpace::Default;
  };
  HighFunc Result = Function;
  Result.Body.clear();
  for (auto Statement : Function.Body) {
    ExprPtr *Root = Statement.Kind == StmtKind::Assign ||
                            Statement.Kind == StmtKind::ExprStmt
                        ? &Statement.Val
                    : Statement.Kind == StmtKind::Call   ? &Statement.CallExpr
                    : Statement.Kind == StmtKind::Return ? &Statement.RetVal
                                                         : nullptr;
    auto Call = Root ? *Root : nullptr;
    std::vector<ExprPtr> Casts;
    while (Call && Call->Kind == ExprKind::Cast && Plain(*Call) &&
           Call->Operands.size() == 1 && Call->Type &&
           (Call->Type->Kind == NdTypeKind::Int ||
            Call->Type->Kind == NdTypeKind::Ptr)) {
      Casts.push_back(Call);
      Call = Call->Operands[0];
    }
    if (!Call || Call->Kind != ExprKind::Call || !Plain(*Call) ||
        Call->IsIndirectCall || !Call->SourceCallHint ||
        Call->SourceCallHint->CallKind != SourceCallTypeHint::Kind::Native ||
        Call->CallAddr != Call->SourceCallHint->TargetAddress ||
        Statement.MemoryOrdering != NdMemoryOrdering::None ||
        Statement.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !objcSourceCallBound(*Call, Image, Functions) ||
        !std::all_of(Call->Operands.begin(), Call->Operands.end(),
                     [&](const ExprPtr &Argument) {
                       return Argument && Plain(*Argument) &&
                              Argument->Operands.empty() &&
                              (Argument->Kind == ExprKind::Const ||
                               Argument->Kind == ExprKind::Var);
                     })) {
      Result.Body.push_back(std::move(Statement));
      continue;
    }
    const auto &Callee = *Functions.at(Call->SourceCallHint->TargetAddress);
    if (Callee.Params.size() != Callee.SourceTypeHint->Parameters.size() ||
        !std::equal(Callee.Params.begin(), Callee.Params.end(),
                    Callee.SourceTypeHint->Parameters.begin(),
                    [](const HighParam &Parameter, const auto &Binding) {
                      return equalSourceTypes(Parameter.Type, Binding.Type) &&
                             Parameter.Name == Binding.Name;
                    })) {
      Result.Body.push_back(std::move(Statement));
      continue;
    }
    auto Copy = std::make_shared<HighExpr>(*Call);
    bool Changed = false;
    for (unsigned I = 0; I < Call->Operands.size(); ++I) {
      const auto &Argument = Call->Operands[I];
      if (Argument->Kind != ExprKind::Const || !Argument->Type ||
          Argument->Type->Size != 8 ||
          (Argument->Type->Kind != NdTypeKind::Int &&
           Argument->Type->Kind != NdTypeKind::Ptr) ||
          (Argument->ConstProvenance != ConstantAddressProvenance::Address &&
           Argument->ConstProvenance !=
               ConstantAddressProvenance::DataAddress) ||
          (Argument->AddressOwnerVA != InvalidVA &&
           Argument->AddressOwnerVA != Argument->ConstVal))
        continue;
      const auto Found = Image.ObjCSourceReferences.find(Argument->ConstVal);
      if (Found == Image.ObjCSourceReferences.end() ||
          Found->second.TheKind != ObjCSourceReference::Kind::IvarOffset ||
          Found->second.Address != Argument->ConstVal)
        continue;
      const auto Type = entryScalarLoadInput(Callee, I);
      if (!Type || (Type->Size != Found->second.Size &&
                    !(Type->Size == 4 && Found->second.Size == 8)))
        continue;
      std::string Name;
      do {
        if (NextId == std::numeric_limits<int>::max())
          return Function;
        Name = "t" + std::to_string(++NextId);
      } while (
          std::any_of(Function.Params.begin(), Function.Params.end(),
                      [&](const HighParam &P) { return P.Name == Name; }) ||
          std::any_of(Function.Locals.begin(), Function.Locals.end(),
                      [&](const HighLocal &L) { return L.Name == Name; }));
      MedVar Variable;
      Variable.Kind = MedVar::Temp;
      Variable.TheArch = Image.Arch;
      Variable.Id = NextId;
      Variable.Size = Type->Size;
      auto Local = HighExpr::makeVar(Variable, Type);
      HighStmt Snapshot;
      Snapshot.Kind = StmtKind::Assign;
      Snapshot.Addr = Statement.Addr;
      Snapshot.Dst = Local;
      Snapshot.Val = HighExpr::makeLoad(Argument, Type);
      Result.Body.push_back(std::move(Snapshot));
      Statement.Addr = 0;
      auto Address = std::make_shared<HighExpr>();
      Address->Kind = ExprKind::Addr;
      Address->Type = NdType::makePtr(Type);
      Address->Operands = {Local};
      Copy->Operands[I] = std::move(Address);
      Changed = true;
    }
    if (Changed) {
      for (auto It = Casts.rbegin(); It != Casts.rend(); ++It) {
        auto Wrapper = std::make_shared<HighExpr>(**It);
        Wrapper->Operands = {Copy};
        Copy = std::move(Wrapper);
      }
      *Root = std::move(Copy);
    }
    Result.Body.push_back(std::move(Statement));
  }
  return Result;
}
} // namespace neverd::sdk
#endif
