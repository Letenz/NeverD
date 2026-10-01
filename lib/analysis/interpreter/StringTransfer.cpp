//===- StringTransfer.cpp - Bounded repeated memory transfers -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "StringTransfer.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/Support/Errc.h"

#include <algorithm>

namespace neverd::analysis::detail {
namespace {

bool value(const NdVar &V, uint16_t Size) {
  return V.Size == Size &&
         (V.isConst() ||
          ((V.isReg() || V.isTemp()) && V.Offset <= InvalidVA - (Size - 1)));
}

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
}

} // namespace

std::optional<StringTransferShape> stringTransferShape(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 5 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      !Op.Output.isTemp() || !value(Op.Output, 8) || !value(Op.Inputs[4], 1))
    return std::nullopt;
  StringTransferShape Shape{};
  switch (static_cast<Intrinsic>(Op.Inputs[0].Offset)) {
  case Intrinsic::Movsb:
    Shape = {1, 3, false};
    break;
  case Intrinsic::Movsw:
    Shape = {2, 3, false};
    break;
  case Intrinsic::Movsd:
    Shape = {4, 3, false};
    break;
  case Intrinsic::Movsq:
    Shape = {8, 3, false};
    break;
  case Intrinsic::Stosb:
    Shape = {1, 2, true};
    break;
  case Intrinsic::Stosw:
    Shape = {2, 2, true};
    break;
  case Intrinsic::Stosd:
    Shape = {4, 2, true};
    break;
  case Intrinsic::Stosq:
    Shape = {8, 2, true};
    break;
  default:
    return std::nullopt;
  }
  if (!value(Op.Inputs[1], 8) || !value(Op.Inputs[2], 8) ||
      !value(Op.Inputs[3], Shape.Fill ? Shape.ElementBytes : 8))
    return std::nullopt;
  return Shape;
}

llvm::Expected<std::vector<LowOp>>
lowerStringTransfer(const LowOp &Op, uint64_t Count, bool Backward,
                    llvm::ArrayRef<LowOp> InstructionOps,
                    uint64_t TemporaryLimit, uint64_t MaxOperations) {
  const auto Shape = stringTransferShape(Op);
  if (!Shape)
    return invalid("unsupported repeated memory transfer shape");
  std::vector<LowOp> Result;
  const uint64_t PerElement = Shape->Fill ? 2 : 4;
  if (!MaxOperations || Count > (MaxOperations - 1) / PerElement ||
      Count > (Result.max_size() - 1) / PerElement)
    return invalid("repeated memory transfer exceeds operation budget");

  uint64_t Next = 0;
  const auto Scan = [&](const NdVar &V) {
    if (!V.isTemp() || !V.Size)
      return true;
    if (V.Offset >= TemporaryLimit || V.Size > TemporaryLimit - V.Offset)
      return false;
    Next = std::max(Next, V.Offset + V.Size);
    return true;
  };
  if (!Scan(Op.Output))
    return invalid("repeated memory transfer temporary is out of range");
  for (const auto &Original : InstructionOps) {
    if (Original.NumInputs > 6 || !Scan(Original.Output))
      return invalid("invalid repeated memory transfer instruction operands");
    for (unsigned I = 0; I < Original.NumInputs; ++I)
      if (!Scan(Original.Inputs[I]))
        return invalid("repeated memory transfer temporary is out of range");
  }
  for (unsigned I = 0; I < Op.NumInputs; ++I)
    if (!Scan(Op.Inputs[I]))
      return invalid("repeated memory transfer temporary is out of range");
  if (Count && (Next > TemporaryLimit || TemporaryLimit - Next < 24))
    return invalid("repeated memory transfer has no disjoint scratch range");

  Result.reserve(static_cast<size_t>(Count * PerElement + 1));
  const auto Emit = [&](NdOp Code, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
    LowOp New;
    New.Opcode = Code;
    New.Addr = Op.Addr;
    New.Seq = Op.Seq;
    New.Output = Output;
    for (const auto &Input : Inputs)
      New.addInput(Input);
    Result.push_back(New);
  };
  const NdVar SourceAddress = NdVar::tmp(Next, 8);
  const NdVar DestinationAddress = NdVar::tmp(Next + 8, 8);
  const NdVar Element = NdVar::tmp(Next + 16, Shape->ElementBytes);
  for (uint64_t I = 0; I < Count; ++I) {
    const uint64_t Distance = I * Shape->ElementBytes;
    const NdVar Delta =
        NdVar::scalar(Backward ? uint64_t{0} - Distance : Distance, 8);
    if (!Shape->Fill) {
      Emit(NdOp::INT_ADD, SourceAddress, {Op.Inputs[1], Delta});
      Emit(NdOp::LOAD, Element, {SourceAddress});
    }
    Emit(NdOp::INT_ADD, DestinationAddress,
         {Op.Inputs[Shape->Fill ? 1 : 2], Delta});
    Emit(NdOp::STORE, {},
         {DestinationAddress, Shape->Fill ? Op.Inputs[3] : Element});
  }
  Emit(NdOp::COPY, Op.Output, {NdVar::scalar(0, 8)});
  return Result;
}

} // namespace neverd::analysis::detail
