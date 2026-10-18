//===- SymConcrete.cpp - Exact concrete execution through SymExec ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymConcrete.h"

#include <iterator>
#include <limits>
#include <map>
#include <set>

namespace neverd::symbolic {

struct SymExecConcreteShadow::Impl {
  std::unique_ptr<SymContext> Context;
  std::unique_ptr<SymState> State;
  std::unique_ptr<SymExec> Executor;
  std::set<uint64_t> SeededBytes;
  std::map<uint64_t, uint64_t> SeededMemory;
  bool Trusted = false;
  bool Started = false;

  void rebuild(llvm::endianness ByteOrder) {
    Executor.reset();
    State.reset();
    Context = std::make_unique<SymContext>();
    State = std::make_unique<SymState>(*Context, ByteOrder);
    Executor = std::make_unique<SymExec>(*Context, *State);
    SeededBytes.clear();
    SeededMemory.clear();
    Trusted = true;
    Started = false;
  }

  std::optional<llvm::APInt> concreteValue(const NdVar &V) {
    SymRef Expression = Executor->operandValue(V);
    if (!Expression.isValid())
      return std::nullopt;
    return Context->asConst(Expression);
  }

  bool fail() {
    Trusted = false;
    return false;
  }
};

SymExecConcreteShadow::SymExecConcreteShadow() : P(std::make_unique<Impl>()) {}

SymExecConcreteShadow::~SymExecConcreteShadow() = default;

bool SymExecConcreteShadow::reset(llvm::endianness ByteOrder) {
  P->rebuild(ByteOrder);
  return true;
}

bool SymExecConcreteShadow::setRegister(uint64_t Offset, uint16_t Bytes,
                                        uint64_t Value) {
  if (!P->Trusted || P->Started || Bytes == 0 || Bytes > sizeof(uint64_t) ||
      (Bytes < sizeof(uint64_t) && Value >> (unsigned(Bytes) * 8) != 0) ||
      Offset > std::numeric_limits<uint64_t>::max() - (Bytes - 1))
    return P->fail();

  for (uint16_t I = 0; I < Bytes; ++I)
    if (P->SeededBytes.count(Offset + I) != 0)
      return P->fail();
  for (uint16_t I = 0; I < Bytes; ++I)
    P->SeededBytes.insert(Offset + I);

  P->State->write(SymSpace::Register, Offset,
                  P->Context->mkConst(uint32_t(Bytes) * 8, Value));
  return true;
}

bool SymExecConcreteShadow::setMemory(uint64_t Address,
                                      llvm::ArrayRef<uint8_t> Bytes) {
  if (!P->Trusted || P->Started || Bytes.empty() ||
      Bytes.size() - 1 > std::numeric_limits<uint64_t>::max() - Address)
    return P->fail();

  const uint64_t Last = Address + Bytes.size() - 1;
  const auto Next = P->SeededMemory.lower_bound(Address);
  if ((Next != P->SeededMemory.end() && Next->first <= Last) ||
      (Next != P->SeededMemory.begin() && std::prev(Next)->second >= Address))
    return P->fail();

  P->SeededMemory.emplace_hint(Next, Address, Last);
  for (size_t I = 0; I < Bytes.size(); ++I)
    P->State->write(SymSpace::Memory, Address + I,
                    P->Context->mkConst(8, Bytes[I]));
  return true;
}

bool SymExecConcreteShadow::step(const LowOp &Op) {
  if (!P->Trusted)
    return false;
  P->Started = true;

  // Widening data does not widen the linear-address interface.
  if (const auto Memory = lowMemoryOperands(Op);
      Memory.Address && Memory.Address->Size > sizeof(uint64_t))
    return P->fail();

  // A concrete run must know every value the operation consumes, even when
  // symbolic simplification could prove that one of them does not affect the
  // output.  This is what keeps a missing seed from becoming an implicit zero.
  for (uint8_t I = 0; I < Op.NumInputs; ++I)
    if (!P->concreteValue(Op.Inputs[I]))
      return P->fail();

  const StepResult Result = P->Executor->step(Op);
  if (Result != StepResult::Continue || P->Executor->unmodelledCount() != 0 ||
      P->Executor->opaqueOperationCount() != 0 ||
      P->Executor->callHavocCount() != 0 ||
      P->Executor->memoryHavocCount() != 0)
    return P->fail();

  if (Op.Output.Size != 0 && !P->concreteValue(Op.Output))
    return P->fail();
  return true;
}

std::optional<uint64_t> SymExecConcreteShadow::value(const NdVar &V) {
  if (!P->Trusted)
    return std::nullopt;
  P->Started = true;
  const std::optional<llvm::APInt> Result = P->concreteValue(V);
  // Operation data can be wider than the scalar control-decision interface.
  // Even a wide value with zero high bits does not fit that interface's width.
  if (!Result || Result->getBitWidth() > 64) {
    P->fail();
    return std::nullopt;
  }
  return Result->getZExtValue();
}

} // namespace neverd::symbolic
