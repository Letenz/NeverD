//===- VariadicStackArguments.h - Stack slots a call stores -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The stack arguments a call stores.  The pass that sizes a System V variadic
/// callee's overflow parameters (sizeVariadicOverflowFromStores) and the
/// conversion of each call to it read them the same way, so every call passes
/// exactly the values its callee's parameters take.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_VARIADICSTACKARGUMENTS_H
#define NEVERD_IR_HIGH_VARIADICSTACKARGUMENTS_H

#include "neverd/Common.h"
#include "neverd/ir/med/MedIR.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd::detail {

class HighEntryStackOffsets;

/// The value a stack slot holds: the bytes from \p Byte of \p Value, which a
/// store wider than a slot writes across several.
struct StackSlotValue {
  MedVar Value;
  uint16_t Byte = 0;
};

/// The register at \p RegOff as it enters \p Block of \p Func: the block's
/// phi of it, else the last definition along the first predecessor path that
/// has one, else at function entry the copy that names its incoming value.
bool reachingRegisterAtBlockEntry(const MedFunc &Func, const MedBlock &Block,
                                  uint64_t RegOff, MedVar &Out);

/// The values the call at \p CallIdx in \p Block stores to the pointer-wide
/// slots from the stack pointer it is made with up, in slot order as far as
/// they run unbroken.  A slot holds the latest store at its start since the
/// block's previous call.
std::vector<StackSlotValue>
storedStackArguments(const MedFunc &Func, const MedBlock &Block, size_t CallIdx,
                     Arch TheArch, HighEntryStackOffsets &Offsets);

} // namespace neverd::detail

#endif // NEVERD_IR_HIGH_VARIADICSTACKARGUMENTS_H
