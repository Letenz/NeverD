//===- RegistrationStateSolver.h - x86 EH state solver --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONSTATESOLVER_H
#define NEVERD_IR_LOW_X86_REGISTRATIONSTATESOLVER_H

#include "RegistrationFrame.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"

#include <compare>
#include <map>
#include <set>
#include <tuple>

namespace neverd::registration_state {

struct CxxCatchContext {
  uint32_t TryIndex = 0;
  uint32_t CatchIndex = 0;
  /// The CRT captures SavedESP before entering a catch and restores that
  /// snapshot on return or guard unwind, even if the catch rewrites the slot.
  std::optional<int32_t> SavedStackOffset;
  std::optional<CallbackFrameAddress> SavedCallback;
  std::optional<va_t> SuspendedCallback;
  std::optional<int32_t> SuspendedStackOffset;
  std::map<int32_t, FrameValue> SuspendedCells;
  std::set<int32_t> SuspendedInitializedBytes;
  size_t cellCount() const {
    return 1 + SuspendedCells.size() + SuspendedInitializedBytes.size();
  }
  FrameValue savedStack() const {
    if (SavedCallback)
      return FrameValue::callbackFrame(SavedCallback->Entry,
                                       SavedCallback->Offset);
    return SavedStackOffset ? FrameValue::frame(*SavedStackOffset)
                            : FrameValue{{}, {}, false, true};
  }
  auto operator<=>(const CxxCatchContext &) const = default;
};

/// SEH filters and finally handlers own distinct runtime invocations even
/// when their decoded bodies share an epilogue. Searching and unwinding also
/// have different dispatch rules. The default key retains the existing C++
/// catch-stack domain and ordinary parent flow.
struct SEHActivation {
  std::optional<va_t> Entry;
  bool SearchFilter = false;
  va_t OrdinaryCall = 0;
  auto operator<=>(const SEHActivation &) const = default;
};

struct Domain {
  SEHActivation Activation;
  std::set<int32_t> Levels;
  FrameState Frame;
  FrameState RuntimeObject;
  std::set<int32_t> InitializedFrameBytes;
  bool Reached = false;
  bool Unknown = false;
  bool Parent = false;
  bool Callback = false;
  bool OtherCallback = false;
  using CatchStack = std::vector<CxxCatchContext>;
  std::set<CatchStack> CxxCatchStacks;
  bool Uninstalled = false;
  bool Installed = false;
  bool CanDispatch = false;
  std::optional<int32_t> OrdinaryCallStack;
  struct ParentStackRestore {
    va_t Address = InvalidVA;
    int Seq = -1;
    int32_t StackOffset = 0;
    bool operator==(const ParentStackRestore &) const = default;
  };
  std::optional<ParentStackRestore> StackRestore;
  size_t catchCellCount() const {
    size_t Count = 0;
    for (const auto &Stack : CxxCatchStacks)
      for (const auto &Context : Stack)
        Count += Context.cellCount();
    return Count;
  }
};

struct BlockFacts {
  bool Invalid = false;
  bool InstalledAtExit = false;
  bool CxxContinuationAtExit = false;
  bool NoReturnAtExit = false;
  bool InvocationAtExit = false;
  bool CallbackReturnAtExit = false;
};

struct ActivationState {
  Domain Incoming;
  BlockFacts Facts;
};

int32_t cxxMinimumTryLevel(const Domain &State, const CxxExceptionInfo &Cxx);
size_t retainedCxxCatchDepth(const Domain::CatchStack &Stack,
                             const CxxExceptionInfo &Cxx, uint32_t TryIndex);

/// One bounded fixed-point analysis. Ordinary transfers and exceptional roots
/// share the same frame lattice, source occurrences and cumulative work limit.
class RegistrationStateSolver {
public:
  RegistrationStateSolver(
      const LowFunc &Function, va_t SecurityCookieVA, va_t CookieCheckVA,
      const std::vector<RegistrationCalleeFrameContract> *Callees,
      const std::vector<RegistrationCleanupFrameContract> *Cleanups,
      const std::vector<RegistrationCalleeStackContract> *Stacks,
      const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
      const std::vector<RegistrationSEHControlContract> *SEHControls);
  RegistrationStateAnalysis run();

private:
  bool initialize();
  bool initializeOrder();
  bool validateRealignedLayout();
  bool realignedMemoryIsDisjoint(const FrameValue &Address,
                                 uint16_t Width) const;
  bool registrationInstallationReady(const FrameState &Frame) const;
  bool callbackCanReturn(const Domain &State) const;
  bool callbackReturnInstruction(size_t I, const LowOp &Op) const;
  std::optional<int32_t> parentStackOffset(const Domain &State) const;
  void preserveCxxFrameCells(Domain &Root, const Domain &Source);
  bool initializeContracts();
  std::optional<RegistrationRuntimeThrow>
  runtimeThrowArguments(const Domain &State,
                        const RegistrationCalleeFrameContract &Callee,
                        std::vector<RegistrationObjectExtent> &Reads);
  void initializeCookies();
  void collectOccurrences();
  void seedEntries();
  struct CallTransfer {
    FrameValue StackPointer;
    bool DoesNotReturn;
    va_t EndAddress;
  };
  std::optional<CallTransfer> transferCall(size_t I, Domain &After,
                                           const LowOp &Op,
                                           const BlockFacts &Facts);
  std::optional<FrameValue> transferLocalUnwind(size_t I, Domain &After,
                                                const LowOp &Op,
                                                BlockFacts &Facts);
  bool transferOrdinaryFinally(size_t I, Domain &After, const LowOp &Op,
                               BlockFacts &Facts,
                               std::optional<FrameValue> &ReturnedSP);
  bool enterOrdinaryFinally(size_t I, const Domain &Before, const LowOp &Op,
                            BlockFacts &Facts);
  std::optional<va_t> sehCallContinuation(va_t Call);
  bool validateSEHInvocationBlock(size_t I);
  bool transferSEHExit(size_t I, Domain &After, const LowOp &Op,
                       BlockFacts &Facts);
  void recordSEHStackRestore(const LowOp &Op, Domain &After,
                             const FrameTransfer &Transfer,
                             const FrameValue &Value);
  bool runLocalFinally(va_t Target, FrameState &Parent,
                       bool OrdinaryInvocation = false);
  void
  recordCatchReturn(size_t I, const Domain &After, const LowOp &Op,
                    const FrameTransfer &Transfer, const BlockFacts &Facts,
                    std::optional<RegistrationCxxContinuation> &CatchReturn);
  void resumeCxxCatch(const Domain &After,
                      const RegistrationCxxContinuation &Return);
  bool recordRuntimeMemory(size_t I, Domain &After, const LowOp &Op,
                           const FrameTransfer &Transfer,
                           const FrameTransfer &RuntimeTransfer,
                           const BlockFacts &Facts);
  bool runtimeObjectIsLive(const Domain &State,
                           std::pair<uint32_t, uint32_t> Identity);
  bool transferBlock(size_t I, const Domain &Before, BlockFacts &Facts);
  void dispatchBlock(size_t I, const Domain &Before, const BlockFacts &Facts);
  std::vector<RegistrationCxxSearch> cxxSearches(const Domain &State,
                                                 bool Invalid);
  bool enterCxxCatch(Domain &Root, const Domain &Source, uint32_t TryIndex,
                     uint32_t CatchIndex);
  RegistrationStateAnalysis finish();

  bool charge(size_t Amount);
  bool validLevel(int32_t Level) const { return AllLevels.count(Level) != 0; }
  bool validObjects(llvm::ArrayRef<RegistrationObjectExtent> Ranges);
  bool validImageRanges(llvm::ArrayRef<ExceptionAddressRange> Ranges);
  /// Source object borrows and runtime construction cannot own registration,
  /// saved-stack, exception-pointer, realigned-anchor or cookie storage.
  bool frameObjectIsDisjointFromAdministration(int64_t Begin,
                                               int64_t End) const;
  bool projectFrameObject(const Domain &State, int32_t Object,
                          std::optional<int32_t> SP,
                          llvm::ArrayRef<RegistrationObjectExtent> Extents,
                          std::vector<RegistrationObjectExtent> &Projected,
                          bool Reads);
  std::optional<int32_t> cookieSlot(int32_t Displacement) const;
  bool cookiesReady(const FrameState &Frame) const;
  void recordChainAccess(const LowOp &Op, const LowBlock &Block,
                         RegistrationChainAccess::Kind Kind);
  void merge(size_t Target, const Domain &Source);
  void dispatch(
      va_t Address, int32_t Level, const Domain &Source, bool Unknown = false,
      bool Callback = false, bool SearchFilter = false,
      std::optional<std::pair<uint32_t, uint32_t>> CxxCatch = std::nullopt);

  const LowFunc &Function;
  const ExceptionFunction &EH;
  const RegistrationChainInfo &Chain;
  va_t SecurityCookieVA;
  va_t CookieCheckVA;
  const std::vector<RegistrationCalleeFrameContract> *Callees;
  const std::vector<RegistrationCleanupFrameContract> *Cleanups;
  const std::vector<RegistrationCalleeStackContract> *Stacks;
  const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds;
  const std::vector<RegistrationSEHControlContract> *SEHControls;
  const bool KnownCxx;
  const bool EH4;
  const bool CheckCalls;
  const bool CheckRuntimeObjects;
  const bool CheckCleanups;
  RegistrationStateAnalysis Result;

  std::map<int, size_t> Index;
  std::map<va_t, size_t> Entries;
  std::vector<bool> InvalidBlocks;
  std::map<va_t, RegistrationTryLevelStore> Stores;
  std::set<int32_t> AllLevels;
  std::set<va_t> FinallyTargets;
  std::set<va_t> SEHParentEntries;
  std::vector<std::map<SEHActivation, ActivationState>> States;
  std::vector<size_t> Order;
  std::set<std::tuple<size_t, size_t, SEHActivation>> Work;
  size_t WorkUsed = 0;
  bool Exhausted = false;
  bool ProvenInstallation = false;
  bool CompleteChainOperations = true;
  std::map<std::pair<va_t, int>, RegistrationChainAccess> ChainAccesses;
  std::map<va_t, std::pair<int, LowInstructionBoundary>> Boundaries;
  std::set<std::pair<va_t, int>> ChainOccurrences;
  std::map<std::pair<va_t, int>, FrameValue> FrameValues;

  std::map<va_t, uint32_t> CalleeIndices;
  std::map<std::pair<va_t, bool>, uint32_t> StackPops;
  std::set<std::pair<va_t, bool>> LocalUnwindTargets;
  bool FailedLocalUnwind = false;
  std::map<va_t, RegistrationSEHControlContract> SEHControlIndex;
  using SEHExitKey = std::tuple<va_t, va_t, va_t, int>;
  std::map<SEHExitKey, RegistrationSEHInvocationExit> SEHExits;
  std::set<SEHExitKey> InvalidSEHExits;
  std::map<uint32_t, uint32_t> CleanupIndices;
  bool CompleteCalls;
  bool CompleteCleanups;
  bool CompleteCatchObjects;
  bool CompleteRuntimeObjects;
  bool CompleteCxxContinuations = true;
  std::map<std::pair<uint32_t, uint32_t>, RegistrationCxxCatchObject>
      CatchObjects;
  std::map<std::pair<va_t, int>, RegistrationCxxContinuation> CxxContinuations;
  std::set<std::pair<va_t, int>> InvalidCxxContinuations;
  std::map<std::pair<va_t, int>, RegistrationCallFrameEffect> CallEffects;
  std::set<std::pair<va_t, int>> InvalidCalls;
  std::map<std::pair<va_t, int>, RegistrationRuntimeObjectAccess>
      RuntimeAccesses;
  std::set<std::pair<va_t, int>> InvalidRuntimeAccesses;
  using CleanupKey = std::tuple<int, int32_t, uint32_t>;
  std::map<CleanupKey, RegistrationCleanupFrameEffect> CleanupEffects;
  std::set<CleanupKey> InvalidCleanups;

  bool ConsistentIncomingAccesses = true;
  bool CompleteImageReads = true;
  std::map<std::pair<va_t, int>, std::optional<RegistrationIncomingFrameAccess>>
      IncomingAccesses;
  std::set<std::pair<va_t, va_t>> ImageReads;

  bool CompleteCookies = false;
  std::optional<int32_t> EHCookieSlot;
  std::optional<int32_t> GSCookieSlot;
  std::map<va_t, int> CookieCheckOccurrences;
  std::map<std::pair<va_t, int>, RegistrationCookieCheck> CookieChecks;
  bool ReadsGSCookie = false;
};

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONSTATESOLVER_H
