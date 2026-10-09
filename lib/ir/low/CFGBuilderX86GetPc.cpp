//===- CFGBuilderX86GetPc.cpp - i386 get-PC thunk calls -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An i386 get-PC thunk is a function whose whole body is `mov r32, [esp];
/// ret`.  Position-independent 32-bit code has no PC-relative data addressing,
/// so it calls one to read its own return address, which GCC names
/// `__x86.get_pc_thunk.<reg>` (`__i686.get_pc_thunk.<reg>` before GCC 4.7).
/// The call has exactly the effect of `mov r32, <return address>`: the call
/// pushes the return address, the thunk copies it, and its return pops it,
/// leaving every other register, the flags and the stack pointer as they
/// were.  CFG construction lifts the call as that copy, so no calling
/// convention clobbers the caller's scratch registers across it or keeps the
/// register it loads.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/support/ISAEncoding.h"

#include <optional>

namespace neverd {

namespace {

/// The general-purpose register number a get-PC thunk at \p Addr loads.  GCC
/// pads the thunk's body with eight `nop` when it tunes for in-order Atom
/// (-mtune=bonnell), which changes nothing it does.
std::optional<unsigned> getPcThunkRegister(const BinaryImage &Img, va_t Addr) {
  size_t At = 0;
  for (const uint8_t *Byte = Img.readVA(Addr, 1);
       Byte && *Byte == x86::kNop && At < x86::kShortFunctionPadding;
       Byte = Img.readVA(Addr + At, 1))
    ++At;
  // mov r32, [esp]: 8B, then ModRM mod 00 rm 100 and the SIB byte of [esp].
  const uint8_t *Body = Img.readVA(Addr + At, x86::kGetPcThunkBodyLen);
  if (!Body || Body[0] != x86::kMovRegFromRmOp ||
      Body[x86::kGetPcThunkBodyLen - 1] != x86::kRetNear)
    return std::nullopt;
  const uint8_t ModRM = Body[1];
  const unsigned Reg = (ModRM >> x86::kModRMRegShift) & x86::kModRMFieldMask;
  if ((ModRM >> x86::kModRMModShift) != x86::kModMemory ||
      (ModRM & x86::kModRMFieldMask) != x86::kRmSIB ||
      Body[2] != x86::kSIBStackTop || x86reg::generalReg(Reg) == x86reg::RSP ||
      !Img.isCodeRange(Addr, At + x86::kGetPcThunkBodyLen))
    return std::nullopt;
  return Reg;
}

} // namespace

void CFGBuilder::rewriteGetPcThunkCall(const BinaryImage &Img, InsnRecord &Rec,
                                       va_t Next) {
  if (Img.Arch != Arch::X86)
    return;
  const va_t Target = *Rec.Immediate;
  const std::optional<unsigned> Reg = getPcThunkRegister(Img, Target);
  if (!Reg)
    return;
  constexpr uint16_t PtrSize = 4;
  LowOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = NdVar::reg(x86reg::generalReg(*Reg), PtrSize);
  Copy.addInput(NdVar::cst(Next, PtrSize));
  Copy.Addr = Rec.Addr;
  Copy.Seq = 0;
  Rec.Ops = {Copy};
  Rec.IsCall = false;
  Rec.Immediate.reset();
  CallTargets.erase(Target);
}

} // namespace neverd
