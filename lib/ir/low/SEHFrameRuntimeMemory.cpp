//===- SEHFrameRuntimeMemory.cpp - Checked Windows CRT memory effects -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Applies authenticated CRT effects while protecting live frame storage.

#include "SEHFrameRuntimeMemory.h"

#include "X86/SEHFrameCRTControl.h"
#include "X86/SEHFrameFormat.h"

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::seh_frame {

bool imageStoreIsDisjoint(const LowSEHRuntimeMemory &Memory, va_t Address,
                          uint16_t Bytes) {
  if (!Bytes || Address > InvalidVA - Bytes)
    return false;
  return llvm::any_of(Memory.writableImageRanges(), [&](const auto &Range) {
    return Address >= Range.Begin && Address + Bytes <= Range.End;
  });
}

std::optional<State> transferRuntimeMemory(const LowOp &Op, const State &Caller,
                                           const TargetRegInfo &TRI,
                                           const LowSEHRuntimeMemory &Memory,
                                           const std::set<int64_t> &ReturnSlots,
                                           size_t &Remaining) {
  if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
      Op.NumInputs != 1 || Op.Inputs[0].Size != 8 ||
      TRI.Win64ParamRegs.size() < 4 || Caller.EscapedFrame)
    return std::nullopt;
  const bool Indirect = Op.Opcode == NdOp::INDIR_CALL;
  const auto Target = Caller.value(Op.Inputs[0]);
  if (!Target || (Target->Identity != Value::Kind::Constant &&
                  !(Indirect && Target->Identity == Value::Kind::ImportTarget)))
    return std::nullopt;
  const auto Binding = Memory.calls().find({Target->Bits, Indirect});
  const auto SP = Caller.get(NdVar::reg(TRI.StackPointer, 8));
  int64_t CallSlot = 0, HomeEnd = 0;
  if (Binding == Memory.calls().end() ||
      (Target->Identity != Value::Kind::ImportTarget &&
       Caller.InvalidatedImportSlots.count(Binding->second.IATAddress)) ||
      !SP || llvm::SubOverflow(*SP, int64_t{8}, CallSlot) ||
      llvm::AddOverflow(*SP, int64_t{32}, HomeEnd) ||
      llvm::any_of(ReturnSlots, [&](int64_t Slot) {
        return Slot > INT64_MAX - 8 || (CallSlot < Slot + 8 && Slot < HomeEnd);
      }))
    return std::nullopt;
  const auto Argument = [&](unsigned I) {
    return NdVar::reg(TRI.Win64ParamRegs[I], 8);
  };
  std::optional<Value> Returned;
  std::optional<int64_t> ControlOutput;
  if (Binding->second.Routine == LowSEHRuntimeCall::Kind::CRTStream) {
    const auto Index = Caller.constant(Argument(0));
    if (!Index || *Index >= 3)
      return std::nullopt;
    Returned = Value{Value::Kind::CRTStream, *Index};
  } else if (Binding->second.Routine == LowSEHRuntimeCall::Kind::CRTVfprintf) {
    const auto Stream = Caller.value(Argument(1));
    const auto Format = Caller.value(Argument(2));
    const auto Locale = Caller.constant(Argument(3));
    const auto VAList = Caller.Spills.find(HomeEnd);
    if (!Stream || Stream->Identity != Value::Kind::CRTStream ||
        Stream->Bits >= 3 || !Format || Locale != 0 ||
        VAList == Caller.Spills.end() ||
        VAList->second.Identity != Value::Kind::Frame)
      return std::nullopt;
    if (Format->Identity == Value::Kind::Constant) {
      if (!Memory.scalarFormats().count(Format->Bits))
        return std::nullopt;
    } else if (Format->Identity != Value::Kind::Frame ||
               !checkedX64FrameFormat(Caller, int64_t(Format->Bits), HomeEnd,
                                      ReturnSlots, Remaining))
      return std::nullopt;
  } else if (Binding->second.Routine == LowSEHRuntimeCall::Kind::CRTControlFP) {
    ControlOutput =
        checkedX64ControlFPOutput(Caller, TRI, HomeEnd, ReturnSlots);
    if (!ControlOutput)
      return std::nullopt;
  } else
    return std::nullopt;

  State Result;
  Result.Spills = Caller.Spills;
  Result.FrameBytes = Caller.FrameBytes;
  Result.InvalidatedImportSlots = Caller.InvalidatedImportSlots;
  // CALL writes its own PC below SP, and the runtime may allocate private
  // storage farther below it. No saved-byte identity there survives a call.
  std::erase_if(Result.Spills,
                [&](const auto &Cell) { return Cell.first < *SP; });
  std::erase_if(Result.FrameBytes,
                [&](const auto &Byte) { return Byte.first < *SP; });
  Result.invalidateMemory(*SP, HomeEnd);
  if (ControlOutput)
    Result.invalidateMemory(*ControlOutput, *ControlOutput + 4);
  for (const auto &[K, Fact] : Caller.Values) {
    const auto &[Space, Offset, Size] = K;
    if (Space == VnodeSpace::REG &&
        TRI.isCallPreserved(Offset, Size, BinaryFormat::COFF))
      Result.Values.emplace(K, Fact);
  }
  Result.Values[key(NdVar::reg(TRI.StackPointer, 8))] = Value::frame(*SP);
  if (Returned)
    Result.Values[key(NdVar::reg(TRI.IntReturnReg, 8))] = *Returned;
  return Result;
}

} // namespace neverd::seh_frame
