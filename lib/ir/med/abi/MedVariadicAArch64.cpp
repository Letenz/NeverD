//===- MedVariadicAArch64.cpp - AAPCS64 variadic prologue -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The AAPCS64 variadic prologue recognizer: the register save area that
/// spills most of both the GP and FP argument registers at entry.
///
//===----------------------------------------------------------------------===//

#include "MedVariadicDetail.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <optional>
#include <set>

namespace neverd {
namespace med_variadic_detail {

/// Most of both argument register files spilled at entry.
bool hasAAPCS64VariadicPrologue(VariadicScan &S) {
  const MedFunc &Func = S.Func;
  const TargetRegInfo &TRI = S.TRI;
  // AArch64 saves both the GP (x0-x7) and FP (q0-q7) argument registers to a
  // contiguous save area; spilling most of both register files at entry is
  // the variadic prologue's signature.  This is the AAPCS64 (Linux/ELF)
  // layout ONLY: Apple/Darwin arm64 passes EVERY variadic argument on the
  // stack and emits NO register save area (its va_start homes a single
  // overflow pointer, detected by MedVariadicDarwin.cpp).  On Mach-O this
  // save-area test would FALSE-POSITIVE on an ordinary -O0 function that merely
  // spills >=4 GP and
  // >=4 FP *named* parameters to its frame (e.g. a non-variadic
  // f(int,double,int,double,int,double,long,double)); the misclassification
  // skips detectXMMParams and silently drops every FP argument, so
  // detectVariadic uses this test only where variadic arguments travel in
  // registers; Darwin variadics use the home-slot test (MedVariadicDarwin.cpp).
  return countParamRegSpills(Func, TRI.IntParamRegs) >= kMinSaveAreaRegs &&
         countParamRegSpills(Func, TRI.FPParamRegs) >= kMinSaveAreaRegs;
}

/// The integer argument register the AAPCS64 register save area starts at:
/// va_start's prologue spills each argument register from the first unnamed
/// one through x7, still holding its entry value, to consecutive doublewords
/// below __gr_top.  A named parameter's own slot (-O0) lies elsewhere.
std::optional<int> aapcs64FirstVariadicRegister(VariadicScan &S) {
  const llvm::ArrayRef<uint64_t> Regs = S.TRI.IntParamRegs;
  // Each register's spill slots, as offsets from the entry stack pointer.
  std::map<size_t, std::set<int64_t>> Slots;
  for (const auto &Blk : S.Func.Blocks)
    for (const auto &Op : Blk.Ops) {
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      const MedVar &Value = Op.Inputs[1];
      if (Value.Kind != MedVar::Reg || Value.SSAVer != 0 ||
          Value.Size != S.TRI.PointerSize)
        continue;
      const auto *It = llvm::find(Regs, Value.RegOff);
      if (It == Regs.end())
        continue;
      if (auto Offset = entrySpDelta(S.values(), S.SpOff, Op.Inputs[0], 0))
        Slots[static_cast<size_t>(It - Regs.begin())].insert(*Offset);
    }
  // Walk down from x7 while each register's slot sits just below the next's.
  size_t First = Regs.size();
  std::set<int64_t> Above;
  while (First > 0) {
    const auto It = Slots.find(First - 1);
    if (It == Slots.end())
      break;
    std::set<int64_t> Chained;
    for (int64_t Offset : It->second)
      if (First == Regs.size() || Above.count(Offset + S.TRI.PointerSize))
        Chained.insert(Offset);
    if (Chained.empty())
      break;
    Above = std::move(Chained);
    --First;
  }
  return static_cast<int>(First);
}

} // namespace med_variadic_detail
} // namespace neverd
