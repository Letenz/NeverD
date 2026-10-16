//===- RegistrationRoots.h - x86 SSA runtime definitions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_REGISTRATIONROOTS_H
#define NEVERD_IR_MED_X86_REGISTRATIONROOTS_H

#include "neverd/ir/med/MedIR.h"

#include <map>
#include <optional>

namespace neverd::x86_registration {

/// Bind a registration no-return receipt to the current decoded call. This
/// does not grant a memory-effect or restored-frame projection.
bool hasNoReturnCallEffect(const LowFunc &Low, const LowBlock &Block,
                           size_t OpIndex,
                           const LowInstructionBoundary &Boundary);

/// Classification and source-proved runtime coordinates for SSA live-ins.
/// Ordinary module entries retain their independent calling convention.
class RegistrationRoots {
public:
  RegistrationRoots(const LowFunc &Low, Arch Architecture, BinaryFormat Format);

  bool hasSEHFrame() const { return HasSEHFrame; }
  bool hasFrame() const { return HasSEHFrame || HasCxxFrame; }
  /// Remove ordinary fallthrough only at an exact, checked no-return source
  /// call. Keep exceptional edges and continuation blocks for SSA rooting.
  void disconnectNoReturnFallthroughs(MedFunc &Func) const;
  /// Separate a callback's private EBP definition from a parent continuation
  /// reached exclusively through checked SavedESP transfers. Run after runtime
  /// live-in definitions are materialized and before SSA renaming.
  void seedSEHParentFrames(MedFunc &Func) const;
  /// An isolated, exact EH3/EH4 table entry retains its personality frame
  /// even when state propagation is incomplete. True denotes a callback's
  /// private stack; false denotes an except body resuming the parent frame.
  std::optional<bool> sehEntry(const MedBlock &Block) const;
  bool isCxxHandler(const MedBlock &Block) const;
  std::optional<std::pair<va_t, int32_t>>
  restoredStack(const MedBlock &Block) const;
  void initialize(MedOp &Op, bool RuntimeRoot, bool Callback,
                  std::optional<std::pair<va_t, int32_t>> RestoredSP) const;

private:
  void disconnectAdjacentSEHCalls(MedFunc &Func) const;
  void removeUnreachedBlocks(MedFunc &Func) const;
  const LowFunc &Low;
  bool HasSEHFrame = false;
  bool HasCxxFrame = false;
  bool Direct = false;
  bool Realigned = false;
  std::map<va_t, std::optional<std::pair<va_t, int32_t>>> RestoredStacks;
};

} // namespace neverd::x86_registration

#endif // NEVERD_IR_MED_X86_REGISTRATIONROOTS_H
