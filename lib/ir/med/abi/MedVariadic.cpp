//===- MedVariadic.cpp - Variadic (...) function detection ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Variadic-function prologue detection for the calling-convention recovery
/// framework.  detectVariadic asks the recognizer of the target's convention
/// whether the function has a variadic prologue, then finds the overflow
/// area base.  Each recognizer lives in its own file (MedVariadicDetail.h):
///   - x86-64: the va_start GP/FP-offset word + register save area;
///   - i386: the stack va_list home-and-reload;
///   - AArch64: the AAPCS64 dual GP+FP save area, or, where every variadic
///     argument is on the stack (Apple arm64), the home-slot round-trip and
///     -O2 register-walk overflow shapes;
///   - ARM32: the save area abutting entry SP.
///
/// detectVariadic is declared in MedCallingConv.cpp and called from detectCc;
/// see that file for the dispatch order.
///
//===----------------------------------------------------------------------===//

#include "MedVariadicDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedCallConvention.h"

#include <functional>

namespace neverd {
namespace med_variadic_detail {

std::optional<int64_t> entrySpDelta(const VariadicValueIndex &Index,
                                    uint64_t SpOff, const MedVar &Root,
                                    int Depth) {
  using Key = VariadicValueIndex::Key;
  const auto &Definitions = Index.Definitions;
  const auto &AmbiguousDefinitions = Index.AmbiguousDefinitions;
  const auto &Phis = Index.Phis;
  const auto &AmbiguousPhis = Index.AmbiguousPhis;

  std::set<Key> Active;
  std::function<std::optional<int64_t>(const MedVar &, int)> Eval =
      [&](const MedVar &V, int Remaining) -> std::optional<int64_t> {
    // A bare constant is an absolute value, not evidence of an entry-SP
    // relative address.  Constants participate only as the offset operand of
    // an ADD/SUB whose other side is already proven entry-SP-derived below.
    // In particular, i386 PIC's call-next/pop get-PC idiom stores and reloads
    // a constant return address; treating that literal as entry_sp+constant
    // misclassifies an ordinary function as variadic and drops its cdecl args.
    if (Remaining <= 0 || V.isConst())
      return std::nullopt;
    if (V.Kind != MedVar::Reg && V.Kind != MedVar::Temp)
      return std::nullopt;
    if (V.Kind == MedVar::Reg && V.RegOff == SpOff && V.SSAVer == 0)
      return int64_t{0};

    const Key K = VariadicValueIndex::keyOf(V);
    if (!Active.insert(K).second || AmbiguousDefinitions.count(K) ||
        AmbiguousPhis.count(K))
      return std::nullopt;
    struct PopActive {
      std::set<Key> &Set;
      Key Value;
      ~PopActive() { Set.erase(Value); }
    } Guard{Active, K};

    if (auto It = Phis.find(K); It != Phis.end()) {
      std::optional<int64_t> Merged;
      for (const auto &[Pred, Arg] : It->second->Args) {
        (void)Pred;
        auto Offset = Eval(Arg, Remaining - 1);
        if (!Offset || (Merged && *Merged != *Offset))
          return std::nullopt;
        Merged = Offset;
      }
      return Merged;
    }
    auto It = Definitions.find(K);
    if (It == Definitions.end())
      return std::nullopt;
    const MedOp &Op = *It->second;
    auto constOf = [](const MedVar &Value) -> std::optional<int64_t> {
      if (!Value.isConst() ||
          (Value.Provenance != ConstantAddressProvenance::Unknown &&
           Value.Provenance != ConstantAddressProvenance::Scalar))
        return std::nullopt;
      return static_cast<int64_t>(Value.ConstVal);
    };
    if (Op.NumInputs < 1)
      return std::nullopt;
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return Eval(Op.Inputs[0], Remaining - 1);
    case NdOp::SUBBYTES:
      return Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
                     Op.Inputs[1].ConstVal == 0
                 ? Eval(Op.Inputs[0], Remaining - 1)
                 : std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      if (Op.NumInputs < 2)
        return std::nullopt;
      if (auto C = constOf(Op.Inputs[1]))
        if (auto Base = Eval(Op.Inputs[0], Remaining - 1))
          return Op.Opcode == NdOp::INT_ADD ? *Base + *C : *Base - *C;
      if (Op.Opcode == NdOp::INT_ADD)
        if (auto C = constOf(Op.Inputs[0]))
          if (auto Base = Eval(Op.Inputs[1], Remaining - 1))
            return *Base + *C;
      return std::nullopt;
    default:
      return std::nullopt;
    }
  };
  return Eval(Root, Depth > 0 ? 64 - Depth : 64);
}

// Whether \p V is the x86-64 SysV va_start word ((fp_offset << 32) | gp_offset)
// with each field a legal save-area offset.  Distinctive enough, paired with a
// parameter-register spill, to mark a variadic prologue.
bool isX64VaStartWord(uint64_t V) {
  const uint64_t Gp = V & 0xFFFFFFFFu;
  const uint64_t Fp = V >> 32;
  return (V >> 48) == 0 && Gp <= limits::kX64VaGpOffsetMax &&
         (Gp % limits::kX64VaGpOffsetStep) == 0 &&
         Fp >= limits::kX64VaFpOffsetMin && Fp <= limits::kX64VaFpOffsetMax &&
         ((Fp - limits::kX64VaFpOffsetMin) % limits::kX64VaFpOffsetStep) == 0;
}

// Count distinct live-in (SSA version 0) parameter registers from \p Regs that
// the function spills to the stack — the variadic prologue's register save
// area.
int countParamRegSpills(const MedFunc &Func, llvm::ArrayRef<uint64_t> Regs) {
  std::set<uint64_t> Seen;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.Inputs[1].Kind == MedVar::Reg && Op.Inputs[1].SSAVer == 0)
        for (uint64_t R : Regs)
          if (Op.Inputs[1].RegOff == R) {
            Seen.insert(R);
            break;
          }
  return static_cast<int>(Seen.size());
}

} // namespace med_variadic_detail

using namespace med_variadic_detail;

//===----------------------------------------------------------------------===//
// Variadic (...) function detection
//===----------------------------------------------------------------------===//

// Detect a variadic function and its overflow-area base, setting
// Func.IsVariadic and Func.VariadicOverflowBase.  The register save area
// round-trips through the ordinary register parameters (its spill/reload
// forwards), but the va_arg overflow reads land above the synthetic frame and
// are spilled there by the emitter; the caller-side count is finalized once all
// call sites are known.
void detectVariadic(MedFunc &Func, const TargetRegInfo &TRI, Arch TargetArch,
                    BinaryFormat Fmt) {
  if (Func.Blocks.empty())
    return;
  VariadicScan S(Func, TRI);
  const CallArgumentConvention *Convention =
      callArgumentConvention(TargetArch, Fmt);
  bool Marked = false;
  std::optional<int> FirstVariadicRegister;
  switch (TargetArch) {
  case Arch::X64:
    Marked = hasX64VariadicPrologue(S);
    break;
  case Arch::X86:
    Marked = hasI386VariadicPrologue(S);
    break;
  case Arch::AArch64:
    if (Convention && Convention->VariadicArgumentsOnStack) {
      Marked = hasDarwinVariadicPrologue(S);
    } else {
      Marked = hasAAPCS64VariadicPrologue(S);
      if (Marked)
        FirstVariadicRegister = aapcs64FirstVariadicRegister(S);
    }
    break;
  case Arch::ARM:
    Marked = hasAAPCS32VariadicPrologue(S);
    break;
  default:
    break;
  }
  if (!Marked)
    return;
  // The overflow area base: the smallest non-negative entry-SP offset stored as
  // a pointer value (the va_list overflow/__stack pointer).  x86-64 places it
  // one slot past the return address (+8); AArch64 at the entry SP (+0).  The
  // register-save-area pointer is entry-SP-negative and does not match.
  std::optional<int64_t> Base;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (auto D = entrySpDelta(S.values(), S.SpOff, Op.Inputs[1], 0))
          if (*D >= 0 && *D <= limits::kVariadicOverflowBaseMax)
            if (!Base || *D < *Base)
              Base = *D;

  // Without such a pointer the base is past the return address the call
  // pushed, if any.
  Func.IsVariadic = true;
  Func.VariadicOverflowBase =
      Base.value_or(TRI.CallPushesReturnAddress ? TRI.PointerSize : 0);
  Func.VariadicFirstRegister = FirstVariadicRegister.value_or(-1);
}

} // namespace neverd
