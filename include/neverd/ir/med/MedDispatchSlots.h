//===- MedDispatchSlots.h - Selectors of a reloaded dispatch ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The selector of a jump-table dispatch whose target travels through a frame
/// slot.  -O0 code spills the target each `goto *table[i]` site loads to one
/// slot that a shared dispatch block reloads, so no selector dominates the
/// dispatch: each predecessor's is the scaled index of its table load, or the
/// entry its constant load reads.  The LLVM and C routes consume the same
/// selectors; each supplies how it looks up definitions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDDISPATCHSLOTS_H
#define NEVERD_IR_MED_MEDDISPATCHSLOTS_H

#include "neverd/ir/med/MedIR.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

struct BinaryImage;
struct JumpTable;

/// A frame slot: the anchoring value's (Id, SSAVer) and a byte offset.
using MedSlotKey = std::pair<std::pair<int, int>, int64_t>;

using MedDefLookup = std::function<const MedOp *(const MedVar &)>;

/// What the dispatch analysis reads of the function a route lowers.
struct MedDispatchContext {
  const MedFunc *Func = nullptr;
  const BinaryImage *Img = nullptr;
  /// Address width in bytes.
  uint16_t AddressBytes = 8;
  /// The operation defining a value anywhere in the function.
  MedDefLookup DefOf;
  /// The frame slot an address value names.
  std::function<std::optional<MedSlotKey>(const MedVar &)> SlotKeyOf;
  /// Whether a value is a certified zero model base (an i386 GOT base in
  /// relocatable-object coordinates).
  std::function<bool(const MedVar &)> ModelZero;
};

/// The frame slot address \p V names: a register anchors it, and constant
/// offsets, copies and extensions accumulate onto that anchor.  \p ThroughRegs
/// also follows the top-level register's definition (escape detection).
std::optional<MedSlotKey> medAddressSlotKey(const MedVar &V,
                                            const MedDefLookup &DefOf,
                                            int Depth = 0,
                                            bool ThroughRegs = false);

/// The scaled index of the table load \p Target flows from within \p Blk,
/// forwarding through a spill of the target to a slot of the same block.
std::optional<MedVar> findTableSwitchIndex(const MedDispatchContext &C,
                                           const MedBlock &Blk,
                                           const MedVar &Target);

/// The slot the target of dispatch \p Br reloads, for `t = load [slot]; br t`.
std::optional<MedSlotKey> dispatchReloadSlot(const MedDispatchContext &C,
                                             const MedBlock &Blk,
                                             const MedOp &Br);

/// The scaled index of the table-loaded target \p Pred stores to \p Slot.
std::optional<MedVar> predecessorSwitchIndex(const MedDispatchContext &C,
                                             const MedBlock &Pred,
                                             const MedSlotKey &Slot);

/// The case value of the entry of \p JT that the load \p Val flows from reads
/// at a constant address.  Nothing unless exactly one base model puts the
/// address on an entry; a variable index never folds.
std::optional<MedVar> constTableLoadIndex(const MedDispatchContext &C,
                                          const MedVar &Val,
                                          const MedDefLookup &DefOf,
                                          const JumpTable &JT,
                                          bool RequireAuthenticatedLoad);

/// The case value of the constant table entry \p Pred stores to \p Slot.
std::optional<MedVar> predecessorConstDispatchIndex(const MedDispatchContext &C,
                                                    const MedBlock &Pred,
                                                    const MedSlotKey &Slot,
                                                    const JumpTable &JT);

/// The data-segment table base of the load \p Pred stores to \p Slot, to keep
/// two tables from sharing one dispatch.  Nothing for a base no constant in a
/// data segment names (an ARM PC-relative base).
std::optional<uint64_t> predecessorTableBase(const MedDispatchContext &C,
                                             const MedBlock &Pred,
                                             const MedSlotKey &Slot);

/// The selector of each predecessor of a shared dispatch.
struct MedSharedDispatchSelectors {
  std::vector<std::pair<int, MedVar>> PerPred;
  /// The widest selector, in bytes.
  unsigned MaxBytes = 0;
};

/// Every predecessor's selector of the shared dispatch \p Br in \p Blk, which
/// reloads the target each predecessor stores.  Nothing when any selector is
/// unknown or two predecessors index different tables.
std::optional<MedSharedDispatchSelectors>
sharedDispatchSelectors(const MedDispatchContext &C, const MedBlock &Blk,
                        const MedOp &Br, const JumpTable &JT);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDDISPATCHSLOTS_H
