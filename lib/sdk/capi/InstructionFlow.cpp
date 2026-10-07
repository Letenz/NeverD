//===- InstructionFlow.cpp - What each native instruction does -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "InstructionFlow.h"

#include "SessionImpl.h"

#include "neverd/Limits.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <atomic>
#include <map>

namespace neverd::sdk {
namespace {

/// Follows the lift's full-width values, each a register's value before the
/// instruction plus a constant, through copies and constant additions to
/// where the stack pointer ends.  A call keeps the stack pointer: the callee
/// pops the return address it pushes.
StackMove stackMove(const TargetRegInfo &Registers, llvm::ArrayRef<LowOp> Ops) {
  struct Value {
    uint64_t Register = 0;
    int64_t Offset = 0;
  };
  const uint16_t Width = Registers.PointerSize;
  std::map<std::pair<VnodeSpace, uint64_t>, Value> Values;
  // Register bytes written so far: their earlier values are gone.
  llvm::SmallVector<std::pair<uint64_t, uint64_t>, 8> Written;
  const auto Overlaps = [](uint64_t Offset, uint64_t Size, uint64_t Start,
                           uint64_t End) {
    return Offset < End && Start < Offset + Size;
  };
  const auto ValueOf = [&](const NdVar &V) -> std::optional<Value> {
    if (V.Size != Width)
      return std::nullopt;
    if (auto It = Values.find({V.Space, V.Offset}); It != Values.end())
      return It->second;
    if (!V.isReg() || llvm::any_of(Written, [&](const auto &Range) {
          return Overlaps(V.Offset, V.Size, Range.first, Range.second);
        }))
      return std::nullopt;
    return Value{V.Offset, 0};
  };
  bool Moved = false;
  for (const LowOp &Op : Ops) {
    const NdVar &Out = Op.Output;
    if (!Out.Size)
      continue;
    std::optional<Value> Result;
    if (Out.Size == Width && Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
      Result = ValueOf(Op.Inputs[0]);
    } else if (Out.Size == Width &&
               (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
               Op.NumInputs == 2) {
      for (unsigned I = 0; I < 2 && !Result; ++I) {
        // `constant - value` is not an offset from the value.
        if (Op.Opcode == NdOp::INT_SUB && I != 0)
          break;
        const NdVar &C = Op.Inputs[1 - I];
        if (!C.isConst() || C.Size == 0 || C.Size > 8)
          continue;
        const int64_t Constant = llvm::SignExtend64(C.Offset, C.Size * 8);
        const auto Base = ValueOf(Op.Inputs[I]);
        if (Base && Constant > -limits::kMaxFrameSize &&
            Constant < limits::kMaxFrameSize)
          Result = Value{Base->Register, Op.Opcode == NdOp::INT_ADD
                                             ? Base->Offset + Constant
                                             : Base->Offset - Constant};
      }
    }
    for (auto It = Values.begin(); It != Values.end();)
      It = It->first.first == Out.Space &&
                   Overlaps(Out.Offset, Out.Size, It->first.second,
                            It->first.second + Width)
               ? Values.erase(It)
               : std::next(It);
    if (Out.isReg()) {
      Written.push_back({Out.Offset, Out.Offset + Out.Size});
      Moved |= Overlaps(Out.Offset, Out.Size, Registers.StackPointer,
                        Registers.StackPointer + Width);
    }
    if (Result)
      Values[{Out.Space, Out.Offset}] = *Result;
  }
  StackMove Move;
  if (!Moved)
    return Move;
  const auto Final = Values.find({VnodeSpace::REG, Registers.StackPointer});
  if (Final == Values.end()) {
    Move.Unknown = true;
    return Move;
  }
  Move.Delta = Final->second.Offset;
  if (Final->second.Register != Registers.StackPointer)
    Move.Base = Final->second.Register;
  return Move;
}

} // namespace

/// The flow of \p DI, with its stack pointer move when \p StackRegisters is
/// given.
InstructionFlow summarizeInstructionFlow(const BinaryImage &Img, Decoder &Dec,
                                         const DecodedInsn &DI,
                                         const TargetRegInfo *StackRegisters) {
  InstructionFlow Flow;
  std::vector<LowOp> Ops;
  // Each row is lifted independently of its neighbours.
  Dec.resetX86FpuState();
  try {
    // The relocations inside the instruction make its relocated immediates
    // addresses, as they are to the pipeline.
    const InstructionRelocations Relocations = instructionRelocations(Img, DI);
    Dec.liftToLow(DI, Ops, Relocations.Addresses, Relocations.Scalars);
  } catch (const UnliftedInstruction &) {
    // Nothing is known about what the instruction does, so it states no
    // transfer, no reference and no stack pointer.
    Flow.Kind = UnliftedFlow;
    Flow.Stack.Unknown = true;
    return Flow;
  }
  if (StackRegisters)
    Flow.Stack = stackMove(*StackRegisters, Ops);
  const auto ConstantInput = [](const LowOp &Op) -> std::optional<va_t> {
    if (Op.NumInputs == 0 || !Op.Inputs[0].isConst())
      return std::nullopt;
    return static_cast<va_t>(Op.Inputs[0].Offset);
  };
  // Values the instruction's own ops compute from constants, such as a
  // RIP-relative address: the next instruction's address plus a
  // displacement.  A sum is an address when one of its terms is; a
  // segment offset alone, which the lifter marks scalar, is not.
  enum class Role : uint8_t { Unknown, Scalar, Address };
  struct Known {
    va_t Value;
    Role Kind;
  };
  struct Slot {
    VnodeSpace Space;
    uint64_t Offset;
    uint16_t Size;
    Known Value;
  };
  llvm::SmallVector<Slot, 8> Computed;
  // Addresses the instruction materializes as values: exact address
  // constants (a relocated immediate, a PC-relative address) that it stores
  // or puts in a register.  A number that only looks like an address is not
  // one; without a relocation an immediate stays a number.
  llvm::SmallVector<va_t, 2> Offsets;
  const auto ValueOf = [&](const NdVar &V) -> std::optional<Known> {
    if (V.isConst())
      return Known{static_cast<va_t>(V.Offset),
                   isExactAddressProvenance(V.Provenance) ? Role::Address
                   : V.Provenance == ConstantAddressProvenance::Scalar
                       ? Role::Scalar
                       : Role::Unknown};
    for (const Slot &S : Computed)
      if (S.Space == V.Space && S.Offset == V.Offset && S.Size == V.Size)
        return S.Value;
    return std::nullopt;
  };
  const auto Record = [&](const LowOp &Op) {
    const NdVar &Out = Op.Output;
    if (!Out.Size || Out.isConst())
      return;
    std::optional<Known> Result;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
      Result = ValueOf(Op.Inputs[0]);
    } else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2) {
      const auto Left = ValueOf(Op.Inputs[0]), Right = ValueOf(Op.Inputs[1]);
      if (Left && Right)
        Result =
            Known{Left->Value + Right->Value,
                  Left->Kind == Role::Address || Right->Kind == Role::Address
                      ? Role::Address
                  : Left->Kind == Role::Scalar || Right->Kind == Role::Scalar
                      ? Role::Scalar
                      : Role::Unknown};
    }
    llvm::erase_if(Computed, [&](const Slot &S) {
      return S.Space == Out.Space && S.Offset < Out.Offset + Out.Size &&
             Out.Offset < S.Offset + S.Size;
    });
    if (!Result)
      return;
    if (Out.Size < 8)
      Result->Value &= (uint64_t{1} << (Out.Size * 8)) - 1;
    Computed.push_back({Out.Space, Out.Offset, Out.Size, *Result});
  };
  for (const LowOp &Op : Ops) {
    switch (Op.Opcode) {
    case NdOp::CALL:
      if (auto Target = ConstantInput(Op)) {
        Flow.Kind = "call";
        Flow.Target = *Target;
      } else {
        Flow.Kind = "icall";
      }
      break;
    case NdOp::INDIR_CALL:
      Flow.Kind = "icall";
      if (auto Slot = ConstantInput(Op))
        Flow.Refs.push_back({*Slot, "read"});
      break;
    case NdOp::BRANCH:
    case NdOp::COND_BR:
      Flow.Kind = Op.Opcode == NdOp::BRANCH ? "jump" : "cjump";
      if (auto Target = ConstantInput(Op))
        Flow.Target = *Target;
      break;
    case NdOp::INDIR_BR:
      Flow.Kind = "ijump";
      if (auto Slot = ConstantInput(Op))
        Flow.Refs.push_back({*Slot, "read"});
      break;
    case NdOp::RETURN:
      Flow.Kind = "ret";
      break;
    case NdOp::LOAD:
    case NdOp::STORE: {
      const LowMemoryOperandView Memory = lowMemoryOperands(Op);
      if (!Memory.Complete || !Memory.Address)
        break;
      if (const auto Address = ValueOf(*Memory.Address);
          Address && Address->Kind != Role::Scalar)
        Flow.Refs.push_back(
            {Address->Value, Op.Opcode == NdOp::LOAD ? "read" : "write"});
      // A stored address, such as an argument pushed as `offset aText`.
      if (Op.Opcode == NdOp::STORE && Memory.StoredValue)
        if (const auto Stored = ValueOf(*Memory.StoredValue);
            Stored && Stored->Kind == Role::Address)
          Offsets.push_back(Stored->Value);
      break;
    }
    default:
      break;
    }
    Record(Op);
    // An address the instruction puts in a register, as `mov eax, offset X`.
    if (Op.Output.isReg())
      if (const auto Value = ValueOf(Op.Output);
          Value && Value->Kind == Role::Address)
        Offsets.push_back(Value->Value);
  }
  if (va_t Address = Dec.pcRelCodeRefTarget(DI); Address != InvalidVA)
    Offsets.push_back(Address);
  // The next instruction's address, which a call pushes and a system call
  // saves, is where execution returns, not an address the code refers to.
  const va_t Next = DI.Addr + DI.Size;
  for (const va_t Address : Offsets)
    if (Address != Next && Address != Flow.Target &&
        llvm::none_of(Flow.Refs, [&](const auto &Ref) {
          return Ref.first == Address && Ref.second == "offset";
        }))
      Flow.Refs.push_back({Address, "offset"});
  return Flow;
}

/// The pointer the loader stored in a relocated slot, as a code address when
/// the slot points to code.
std::optional<va_t> relocatedPointer(const BinaryImage &Img, va_t Slot) {
  const size_t Size = Img.getPointerSize();
  const uint8_t *Bytes = Size ? Img.readVA(Slot, Size) : nullptr;
  if (!Bytes)
    return std::nullopt;
  const va_t Target = readPtr(Bytes, Size == 8);
  return Img.Arch == Arch::ARM && Img.CodePtrRelocSlots.count(Slot)
             ? clearThumbBit(Target)
             : Target;
}

FunctionPage functionPage(const Session &S, va_t FirstEntry, int MaxFunctions) {
  std::vector<const FuncInfo *> Ordered;
  Ordered.reserve(S.Functions.size());
  for (const FuncInfo &F : S.Functions)
    if (F.Entry >= FirstEntry)
      Ordered.push_back(&F);
  std::sort(Ordered.begin(), Ordered.end(),
            [](const FuncInfo *Left, const FuncInfo *Right) {
              return Left->Entry < Right->Entry;
            });
  Ordered.erase(std::unique(Ordered.begin(), Ordered.end(),
                            [](const FuncInfo *Left, const FuncInfo *Right) {
                              return Left->Entry == Right->Entry;
                            }),
                Ordered.end());
  const size_t End = std::min(
      Ordered.size(), static_cast<size_t>(std::clamp(
                          MaxFunctions, 1, FunctionPage::MaxFunctionsPerPage)));
  FunctionPage Page;
  if (End < Ordered.size())
    Page.NextEntry = Ordered[End]->Entry;
  Ordered.resize(End);
  Page.Functions = std::move(Ordered);
  return Page;
}

bool decodeFunctions(
    const Session &S, const FunctionPage &Page,
    llvm::function_ref<void(size_t Index, Decoder &, const DecodedInsn &)>
        Visit) {
  // A function without a recorded size is decoded only up to its first
  // terminator; this bounds a corrupt or missing extent.
  constexpr uint64_t MaxUnsizedInstructions = 65536;
  std::atomic<bool> DecoderFailed{false};
  parallelForEach(Page.Functions.size(), [&](auto Claim, size_t Total) {
    Decoder Local;
    if (!Local.init(S.Img)) {
      DecoderFailed = true;
      return;
    }
    for (size_t Index = Claim(); Index < Total; Index = Claim()) {
      const FuncInfo &F = *Page.Functions[Index];
      const uint64_t Limit = F.Size > 0 ? F.Size : MaxUnsizedInstructions;
      (void)decodeNativeRange(S.Img, Local, F.Entry, F.Size, Limit,
                              [&](const DecodedInsn &DI, const uint8_t *, int) {
                                Visit(Index, Local, DI);
                                return F.Size > 0 ||
                                       !Local.isFunctionTerminator(DI);
                              });
    }
  });
  return !DecoderFailed;
}

} // namespace neverd::sdk
