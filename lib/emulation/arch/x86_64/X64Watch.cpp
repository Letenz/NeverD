//===- X64Watch.cpp - Direct execution between observed ranges ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/ExecutionDiagnostics.h"
#include "CheckedX64Backend.h"

#include "llvm/ADT/STLExtras.h"

#include <set>

namespace neverd::emulation {
namespace {
bool knownFlow(unsigned ID) {
  switch (ID) {
#define NEVERD_CHECKED_X64_INSTRUCTION(Name) case X86_INS_##Name:
#include "CheckedX64Instructions.def"
#undef NEVERD_CHECKED_X64_INSTRUCTION
    return true;
  default:
    return false;
  }
}
} // namespace

void CheckedX64Backend::setExecutionWatches(
    const std::vector<ExecutionWatch> &Watches) {
  if (!llvm::equal(ExecutionWatches, Watches, [](const auto &A, const auto &B) {
        return A.Address == B.Address && A.Size == B.Size;
      })) {
    RegionStops.clear();
    SavedRegions.clear();
  }
  CheckedBackend::setExecutionWatches(Watches);
  FetchWatchPages.clear();
  for (const auto &W : Watches) {
    uint64_t Begin = W.Address & ~(x64::PageSize - 1);
    uint64_t Last = (W.Address + W.Size - 1) | (x64::PageSize - 1);
    if (!FetchWatchPages.empty()) {
      auto &P = FetchWatchPages.back();
      if (Begin - P.Address <= P.Size) {
        Begin = P.Address;
        Last = std::max(Last, P.Address + P.Size - 1);
        FetchWatchPages.pop_back();
      }
    }
    // The complete virtual namespace cannot fit in one uint64_t size.
    if (!Begin && Last == UINT64_MAX) {
      FetchWatchPages.push_back({0, UINT64_MAX - (x64::PageSize - 1)});
      Begin = UINT64_MAX & ~(x64::PageSize - 1);
    }
    FetchWatchPages.push_back({Begin, Last - Begin + 1});
  }
}

llvm::Error CheckedX64Backend::stepDirectInstruction(uint64_t Root) {
  const uint64_t PC = CPU.reg(X64Register::PC);
  auto Decoding = decodeInstruction(PC);
  if (!Decoding)
    return Decoding.takeError();
  if (*Decoding) {
    const auto &I = decodedInstruction();
    if (I.id == X86_INS_PUSHF || I.id == X86_INS_PUSHFQ ||
        I.id == X86_INS_POPF || I.id == X86_INS_POPFQ) {
      // A synthetic TF would become guest data through PUSHF, or change when
      // POPF takes effect. Stop at the linear successor without setting TF.
      if (!Machine->supportsExecutionStops() || I.size > UINT64_MAX - PC)
        return diagnostic::error(diagnostic::DirectExecutionUnsupported);
      const uint64_t Next = PC + I.size;
      return Machine->runTo(CPU, Root, {Deadline, &StopRequested}, {Next});
    }
  }
  // The guest's own single-step exception must not be consumed as a private
  // monitor trap. Native free execution delivers it with its actual context.
  if (CPU.reg(X64Register::FLAGS) & x64::TrapFlag)
    return Machine->run(CPU, Root, {Deadline, &StopRequested});
  return Machine->step(CPU, Root, {Deadline, &StopRequested});
}

llvm::Expected<std::vector<uint64_t>>
CheckedX64Backend::regionStops(uint64_t PC) {
  if (auto I = RegionStops.find(PC); I != RegionStops.end())
    return *I->second;
  const uint64_t Page = *RegionPage;
  if (RegionStops.empty()) {
    RegionDecoded.reset();
    if (auto E = Memory->read(Page, RegionBytes, executionPermissions(Execute)))
      return std::move(E);
  }
  for (bool FollowBranches = true;; FollowBranches = false) {
    std::vector<uint64_t> Work{PC}, Stops;
    std::set<uint64_t> Seen;
    auto Stop = [&](uint64_t At) {
      if (!llvm::is_contained(Stops, At))
        Stops.push_back(At);
    };
    while (!Work.empty() && Stops.size() <= x64::ExecutionStopCount) {
      const uint64_t At = Work.back();
      Work.pop_back();
      if ((At & ~(x64::PageSize - 1)) != Page || !Seen.insert(At).second)
        continue;
      if (executionWatched(At)) {
        Stop(At);
        continue;
      }
      auto Decoding = decodeInstruction(At);
      if (!Decoding)
        return Decoding.takeError();
      if (!*Decoding) {
        Stop(At);
        continue;
      }
      const auto &I = decodedInstruction();
      if (!knownFlow(I.id) || I.size > x64::PageSize - (At - Page)) {
        Stop(At);
        continue;
      }
      RegionDecoded.set(At - Page, At - Page + I.size);
      const auto &X = I.detail->x86;
      // Segment writes have architectural interrupt/debug shadows. Leave them
      // to the ordinary processor boundary instead of planning across them.
      if (llvm::any_of(llvm::ArrayRef(X.operands, X.op_count),
                       [](const auto &O) {
                         return O.type == X86_OP_REG && O.reg == X86_REG_SS;
                       })) {
        Stop(At);
        continue;
      }
      const bool Jump = cs_insn_group(Decoder, &I, CS_GRP_JUMP);
      const bool Call = cs_insn_group(Decoder, &I, CS_GRP_CALL);
      if (Jump || Call) {
        if (!FollowBranches || X.op_count != 1 ||
            X.operands[0].type != X86_OP_IMM) {
          Stop(At);
          continue;
        }
        Work.push_back(uint64_t(X.operands[0].imm));
        if (I.id != X86_INS_JMP)
          Work.push_back(At + I.size);
        continue;
      }
      if (cs_insn_group(Decoder, &I, CS_GRP_RET) ||
          cs_insn_group(Decoder, &I, CS_GRP_IRET) ||
          cs_insn_group(Decoder, &I, CS_GRP_INT) ||
          cs_insn_group(Decoder, &I, CS_GRP_BRANCH_RELATIVE)) {
        Stop(At);
        continue;
      }
      Work.push_back(At + I.size);
    }
    if (Stops.size() > x64::ExecutionStopCount) {
      // Keep the leading basic block when the full graph has more frontiers
      // than the transport can arm. Stop at its branch instead of stepping
      // every preceding instruction, including flag-sensitive operations.
      continue;
    }
    auto Plan = std::make_shared<const std::vector<uint64_t>>(Stops);
    for (uint64_t At : Seen)
      RegionStops.emplace(At, Plan);
    return Stops;
  }
}

void CheckedX64Backend::leaveWatchedRegion() {
  if (RegionPage && !RegionStops.empty()) {
    if (SavedRegions.size() == x64::WatchRegionCacheEntries)
      SavedRegions.erase(SavedRegions.begin());
    SavedRegions.insert_or_assign(
        *RegionPage, SavedRegion{RegionBytes, std::move(RegionDecoded),
                                 std::move(RegionStops)});
  }
  RegionPage.reset();
  RegionPhysical.reset();
  RegionStops.clear();
  RegionDecoded = llvm::BitVector(unsigned(x64::PageSize));
  RegionDirty = false;
}

llvm::Error
CheckedX64Backend::runWatchedRegion(std::optional<uint64_t> &OpenPage) {
  const uint64_t Page = CPU.reg(X64Register::PC) & ~(x64::PageSize - 1);
  if (RegionPage != Page) {
    leaveWatchedRegion();
    RegionPage = Page;
    if (auto E = Memory->read(Page, RegionBytes, executionPermissions(Execute)))
      return E;
    if (auto I = SavedRegions.find(Page); I != SavedRegions.end()) {
      // Inactive code can change through any physical alias. Revalidate every
      // byte used to decode the plan. An active page instead has a write
      // guard in every native projection, and revalidates after that write.
      if (llvm::all_of(I->second.Decoded.set_bits(), [&](unsigned B) {
            return RegionBytes[B] == I->second.Bytes[B];
          })) {
        RegionStops = std::move(I->second.Stops);
        RegionDecoded = std::move(I->second.Decoded);
      }
      SavedRegions.erase(I);
    }
    const auto M = Memory->mappings().find(Page);
    if (M == Memory->mappings().end() || M->second.IO)
      return diagnostic::error(diagnostic::DirectExecutionUnsupported);
    RegionPhysical = M->second.Physical;
  }
  if (RegionDirty) {
    std::array<uint8_t, x64::PageSize> Actual;
    if (auto E = Memory->read(Page, Actual, executionPermissions(Execute)))
      return E;
    if (!llvm::all_of(RegionDecoded.set_bits(), [&](unsigned B) {
          return RegionBytes[B] == Actual[B];
        })) {
      RegionStops.clear();
      RegionDecoded.reset();
    }
    RegionBytes = Actual;
    RegionDirty = false;
  }
  auto Stops = regionStops(CPU.reg(X64Register::PC));
  if (!Stops)
    return Stops.takeError();
  if (llvm::is_contained(*Stops, CPU.reg(X64Register::PC)))
    return stepWatchedInstruction();

  // A branch out of this region must fault before another page executes,
  // including pages without public watches. Data permissions stay intact.
  std::vector<ExecutionWatch> Closed;
  if (Page)
    Closed.push_back({0, Page});
  if (Page != (UINT64_MAX & ~(x64::PageSize - 1))) {
    const uint64_t Next = Page + x64::PageSize;
    Closed.push_back({Next, UINT64_MAX - Next + 1});
  }
  ++WatchEpoch;
  auto Root = buildX64PageTables(
      *Memory, UserMode, Machine->requiresExceptionMonitor(), Closed,
      WatchEpoch, true, RegionPhysical, &WatchTables);
  ++WatchEpoch;
  if (!Root)
    return Root.takeError();
  OpenPage = Page;
  for (;;) {
    if (auto E = Machine->runTo(CPU, *Root, {Deadline, &StopRequested}, *Stops))
      return E;
    const uint64_t PC = CPU.reg(X64Register::PC);
    if (!llvm::is_contained(*Stops, PC))
      return diagnostic::error(diagnostic::DirectExecutionUnsupported);
    if (executionWatched(PC) && Hooks.Instruction)
      Hooks.Instruction(PC, 0);
    if (StopRequested || FirstFault)
      return llvm::Error::success();
    // The frontier itself is one watched/resumed or indirect instruction.
    // Keep the same projection and code guard across its native step.
    if (auto E = stepDirectInstruction(*Root))
      return E;
    if ((CPU.reg(X64Register::PC) & ~(x64::PageSize - 1)) != Page)
      return llvm::Error::success();
    Stops = regionStops(CPU.reg(X64Register::PC));
    if (!Stops)
      return Stops.takeError();
    // Reaching a frontier immediately calls for another exact step, without
    // arming an execution breakpoint at the current instruction forever.
    while (llvm::is_contained(*Stops, CPU.reg(X64Register::PC))) {
      const uint64_t Next = CPU.reg(X64Register::PC);
      if (executionWatched(Next) && Hooks.Instruction)
        Hooks.Instruction(Next, 0);
      if (StopRequested || FirstFault)
        return llvm::Error::success();
      if (auto E = stepDirectInstruction(*Root))
        return E;
      if ((CPU.reg(X64Register::PC) & ~(x64::PageSize - 1)) != Page)
        return llvm::Error::success();
      Stops = regionStops(CPU.reg(X64Register::PC));
      if (!Stops)
        return Stops.takeError();
    }
  }
}
} // namespace neverd::emulation
