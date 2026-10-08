//===- RegistrationFrame.h - x86 EH frame-value domain -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_REGISTRATIONFRAME_H
#define NEVERD_IR_LOW_REGISTRATIONFRAME_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

#include <array>
#include <map>
#include <optional>

namespace neverd::registration_state {

/// Values relative to the established EBP, in the PE32 address domain. Unknown
/// values retain possible frame provenance when paths or arithmetic disagree.
struct FrameValue {
  std::optional<int32_t> Offset;
  std::optional<uint32_t> Constant;
  bool PreviousChain = false;
  bool MayBeFrame = false;

  static FrameValue frame(int32_t Offset) { return {Offset, {}, false, true}; }
  static FrameValue constant(uint32_t Value) {
    return {{}, Value, false, false};
  }
  static FrameValue previousChain() { return {{}, {}, true, false}; }
  friend bool operator==(const FrameValue &, const FrameValue &) = default;
};

inline FrameValue join(FrameValue Left, const FrameValue &Right) {
  if (Left == Right)
    return Left;
  return {{}, {}, false, Left.MayBeFrame || Right.MayBeFrame};
}

struct FrameState {
  std::array<FrameValue, 8> Registers;
  std::map<int32_t, FrameValue> Cells;

  bool merge(const FrameState &Other) {
    bool Changed = false;
    for (size_t I = 0; I < Registers.size(); ++I) {
      const FrameValue Merged = join(Registers[I], Other.Registers[I]);
      Changed |= Merged != Registers[I];
      Registers[I] = Merged;
    }
    for (auto &[Offset, Value] : Cells) {
      const auto It = Other.Cells.find(Offset);
      const FrameValue Merged =
          join(Value, It == Other.Cells.end() ? FrameValue{} : It->second);
      Changed |= Merged != Value;
      Value = Merged;
    }
    for (const auto &[Offset, Value] : Other.Cells)
      if (!Cells.count(Offset)) {
        const FrameValue Merged = join({}, Value);
        if (Merged != FrameValue{}) {
          Cells.emplace(Offset, Merged);
          Changed = true;
        }
      }
    return Changed;
  }

  void store(int32_t Offset, uint16_t Width, const FrameValue &Value) {
    for (auto It = Cells.begin(); It != Cells.end();) {
      if (int64_t(It->first) < int64_t(Offset) + Width &&
          int64_t(Offset) < int64_t(It->first) + 4) {
        const bool FullyOverwritten =
            Offset <= It->first &&
            int64_t(It->first) + 4 <= int64_t(Offset) + Width;
        if (!FullyOverwritten && It->second.MayBeFrame) {
          It->second = {{}, {}, false, true};
          ++It;
        } else
          It = Cells.erase(It);
      } else
        ++It;
    }
    if (Width == 4 && Value != FrameValue{})
      Cells[Offset] = Value;
  }
};

/// One block transfer. Registers and frame cells survive instructions and CFG
/// edges; only the lifter's instruction-local temporaries are discarded.
class FrameTransfer {
public:
  FrameTransfer(FrameState &State, int32_t RegistrationOffset)
      : State(State), RegistrationOffset(RegistrationOffset) {}

  void beginInstruction(va_t Address) {
    if (Address != Instruction) {
      Temps.clear();
      Instruction = Address;
    }
  }

  FrameValue read(const NdVar &Value) const {
    if (Value.isConst())
      return FrameValue::constant(static_cast<uint32_t>(Value.Offset));
    if (Value.isReg() && Value.Offset < 8 * x86reg::GeneralRegStride) {
      const FrameValue &Register =
          State.Registers[Value.Offset / x86reg::GeneralRegStride];
      if (Value.Offset % x86reg::GeneralRegStride == 0 && Value.Size >= 4)
        return Register;
      return {{}, {}, false, Register.MayBeFrame};
    }
    if (Value.isTemp()) {
      auto It = Temps.find(Value.Offset);
      if (It != Temps.end())
        return It->second;
    }
    return {};
  }

  FrameValue evaluate(const LowOp &Op, bool Installed) const {
    FrameValue Result;
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      Result.MayBeFrame |= read(Op.Inputs[I]).MayBeFrame;
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT) &&
        Op.NumInputs == 1)
      return read(Op.Inputs[0]);
    if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
        Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0 &&
        Op.Output.Size == 4)
      return read(Op.Inputs[0]);
    if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1) {
      const FrameValue Address = read(Op.Inputs[0]);
      if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
          Address.Constant == uint32_t{0} && Op.Output.Size == 4)
        return Installed ? FrameValue::frame(RegistrationOffset)
                         : FrameValue::previousChain();
      if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Address.MayBeFrame) {
        if (Address.Offset && Op.Output.Size == 4)
          if (auto It = State.Cells.find(*Address.Offset);
              It != State.Cells.end())
            return It->second;
        for (const auto &[Offset, Value] : State.Cells)
          if (Value.MayBeFrame &&
              (!Address.Offset ||
               (int64_t(Offset) < int64_t(*Address.Offset) + Op.Output.Size &&
                int64_t(*Address.Offset) < int64_t(Offset) + 4)))
            return {{}, {}, false, true};
      }
      return {};
    }
    if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
        Op.NumInputs == 2) {
      const FrameValue Left = read(Op.Inputs[0]);
      const FrameValue Right = read(Op.Inputs[1]);
      if (Left.Offset && Right.Constant)
        return FrameValue::frame(static_cast<int32_t>(
            uint32_t(*Left.Offset) +
            (Op.Opcode == NdOp::INT_ADD ? *Right.Constant : -*Right.Constant)));
      if (Op.Opcode == NdOp::INT_ADD && Left.Constant && Right.Offset)
        return FrameValue::frame(
            static_cast<int32_t>(*Left.Constant + uint32_t(*Right.Offset)));
      if (Left.Constant && Right.Constant)
        return FrameValue::constant(Op.Opcode == NdOp::INT_ADD
                                        ? *Left.Constant + *Right.Constant
                                        : *Left.Constant - *Right.Constant);
    }
    return Result;
  }

  void write(const LowOp &Op, FrameValue Value) {
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
      State.Registers[x86reg::RAX / x86reg::GeneralRegStride] = {
          {}, {}, false, true};
      State.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {
          {}, {}, false, true};
      State.Registers[x86reg::RDX / x86reg::GeneralRegStride] = {
          {}, {}, false, true};
      // LowIR's call alone does not prove the callee's PE32 stack-pop ABI.
      // A later explicit SP restoration can recover the frame value.
      State.Registers[x86reg::RSP / x86reg::GeneralRegStride] = {
          {}, {}, false, true};
      Value = {{}, {}, false, true};
    }
    if (Op.Output.isTemp())
      Temps[Op.Output.Offset] = Value;
    else if (Op.Output.isReg() &&
             Op.Output.Offset < 8 * x86reg::GeneralRegStride) {
      FrameValue &Register =
          State.Registers[Op.Output.Offset / x86reg::GeneralRegStride];
      if (Op.Output.Offset % x86reg::GeneralRegStride == 0 &&
          Op.Output.Size >= 4)
        Register = Value;
      else
        Register = {{}, {}, false, Register.MayBeFrame || Value.MayBeFrame};
    }
  }

private:
  FrameState &State;
  int32_t RegistrationOffset;
  std::map<uint64_t, FrameValue> Temps;
  va_t Instruction = InvalidVA;
};

} // namespace neverd::registration_state

#endif
