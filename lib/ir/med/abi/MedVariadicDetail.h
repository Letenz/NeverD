//===- MedVariadicDetail.h - Variadic prologue recognizers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared by detectVariadic (MedVariadic.cpp) and the
/// per-convention variadic prologue recognizers: MedVariadicX86.cpp (x86-64
/// va_start word, i386 stack va_list), MedVariadicAArch64.cpp (AAPCS64 save
/// area), MedVariadicDarwin.cpp (Apple arm64 stack va_list) and
/// MedVariadicARM.cpp (AAPCS save area abutting the entry SP).  Not public.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDVARIADICDETAIL_H
#define NEVERD_IR_MED_MEDVARIADICDETAIL_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {
namespace med_variadic_detail {

/// The minimum number of saved parameter registers that distinguishes a
/// variadic register save area from an ordinary function spilling a few of
/// its own arguments.
constexpr int kMinSaveAreaRegs = 4;

// Definitions stay unchanged for one detectVariadic invocation. Index them
// once; recursive proof state still belongs to each individual root query.
struct VariadicValueIndex {
  using Key = std::tuple<uint8_t, int, int, uint64_t, uint16_t>;
  using WalkKey = std::tuple<uint8_t, int, int>;

  static Key keyOf(const MedVar &Value) {
    return {static_cast<uint8_t>(Value.Kind), Value.Id, Value.SSAVer,
            Value.Kind == MedVar::Reg ? Value.RegOff : 0, Value.Size};
  }

  // Darwin's existing COPY/OR walk uses a deliberately different identity
  // from the exact entry-SP proof. Keep its first COPY and ordered ORs.
  static WalkKey walkKeyOf(const MedVar &Value) {
    return {static_cast<uint8_t>(Value.Kind), Value.Id, Value.SSAVer};
  }

  std::map<Key, const MedOp *> Definitions;
  std::set<Key> AmbiguousDefinitions;
  std::map<Key, const PhiNode *> Phis;
  std::set<Key> AmbiguousPhis;
  std::map<WalkKey, const MedOp *> FirstCopies;
  std::map<WalkKey, llvm::SmallVector<const MedOp *, 1>> OrDefinitions;

  explicit VariadicValueIndex(const MedFunc &Func) {
    for (const auto &Block : Func.Blocks) {
      for (const auto &Op : Block.Ops) {
        if (!Op.Output.isConst()) {
          const Key K = keyOf(Op.Output);
          auto [It, Inserted] = Definitions.emplace(K, &Op);
          if (!Inserted && It->second != &Op)
            AmbiguousDefinitions.insert(K);
        }
        const WalkKey K = walkKeyOf(Op.Output);
        if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1)
          FirstCopies.emplace(K, &Op);
        if (Op.Opcode == NdOp::INT_OR && Op.NumInputs >= 2 &&
            Op.Inputs[1].isConst())
          OrDefinitions[K].push_back(&Op);
      }
      for (const auto &Phi : Block.Phis) {
        const Key K = keyOf(Phi.Output);
        auto [It, Inserted] = Phis.emplace(K, &Phi);
        if (!Inserted && It->second != &Phi)
          AmbiguousPhis.insert(K);
      }
    }
  }
};

// Byte offset of \p V relative to the entry stack pointer. Every SSA/PHI

/// Byte offset of \p Root relative to the entry stack pointer.  Every
/// SSA/PHI definition must be unique and every PHI arm must prove the same
/// offset.
std::optional<int64_t> entrySpDelta(const VariadicValueIndex &Index,
                                    uint64_t SpOff, const MedVar &Root,
                                    int Depth);

/// Whether \p V is the x86-64 SysV va_start word ((fp_offset << 32) |
/// gp_offset) with each field a legal save-area offset.
bool isX64VaStartWord(uint64_t V);

/// Distinct live-in (SSA version 0) parameter registers from \p Regs that the
/// function spills to the stack: the variadic prologue's register save area.
int countParamRegSpills(const MedFunc &Func, llvm::ArrayRef<uint64_t> Regs);

/// One function's variadic prologue recognition.
class VariadicScan {
public:
  VariadicScan(const MedFunc &Func, const TargetRegInfo &TRI)
      : Func(Func), TRI(TRI), SpOff(TRI.StackPointer) {}

  const MedFunc &Func;
  const TargetRegInfo &TRI;
  const uint64_t SpOff;

  /// The function's definitions, indexed on first use.
  const VariadicValueIndex &values() {
    if (!Values)
      Values.emplace(Func);
    return *Values;
  }

private:
  std::optional<VariadicValueIndex> Values;
};

/// Whether the function has the variadic prologue of each convention.
bool hasX64VariadicPrologue(VariadicScan &S);
std::optional<int> aapcs64FirstVariadicRegister(VariadicScan &S);
bool hasI386VariadicPrologue(VariadicScan &S);
bool hasAAPCS64VariadicPrologue(VariadicScan &S);
bool hasDarwinVariadicPrologue(VariadicScan &S);
bool hasAAPCS32VariadicPrologue(VariadicScan &S);

} // namespace med_variadic_detail
} // namespace neverd

#endif // NEVERD_IR_MED_MEDVARIADICDETAIL_H
