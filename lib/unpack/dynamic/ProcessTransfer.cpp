//===- ProcessTransfer.cpp - Transfers into generated code ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessTransfer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>

namespace neverd::unpack {
using namespace emulation;

llvm::Error TransferObserver::snapshot(ProcessView &Process,
                                       std::vector<uint8_t> &Bytes,
                                       std::vector<uint8_t> *Access) {
  const uint64_t Size = Extent;
  Bytes.assign(Size, 0);
  if (Access)
    Access->assign(Size / value::PageSize, 0);
  auto Mappings = Process.mappings();
  if (!Mappings)
    return Mappings.takeError();
  // Unmapped image pages have no bytes; they stay zero and without access.
  for (const auto &M : *Mappings) {
    if (M.Device || M.Address >= Base + Size || M.Address + M.Size <= Base)
      continue;
    const uint64_t Begin = std::max(M.Address, Base) - Base;
    const uint64_t End = std::min(M.Address + M.Size, Base + Size) - Base;
    if (auto E = Process.read(Base + Begin, llvm::MutableArrayRef(Bytes).slice(
                                                Begin, End - Begin)))
      return E;
    if (Access)
      for (uint64_t Page = Begin / value::PageSize;
           Page < llvm::alignTo(End, value::PageSize) / value::PageSize; ++Page)
        (*Access)[Page] |= M.Permissions & GuestAccessPermissions;
  }
  return llvm::Error::success();
}

void TransferObserver::refreshWatches() {
  Watches.clear();
  auto Add = [&](uint64_t Begin, uint64_t End) {
    const uint64_t Address = Base + Begin;
    if (!Watches.empty() &&
        Address - Watches.back().Address <= Watches.back().Size)
      Watches.back().Size =
          std::max(Watches.back().Size, Base + End - Watches.back().Address);
    else
      Watches.push_back({Address, End - Begin});
  };
  for (uint64_t Page = 0; Page < Visited.size(); ++Page) {
    const uint64_t Begin = Page * value::PageSize;
    const uint64_t End = Begin + value::PageSize;
    if (!Visited[Page]) {
      // A visited predecessor can fetch operand bytes from this page. Its
      // write watch includes this prefix; resuming() refreshes those bytes.
      // Expand only a changed prefix, keeping unchanged stub pages executable
      // in direct runs rather than single-stepping their entire contents.
      if (Page && Visited[Page - 1])
        for (uint64_t At = Begin;
             At < std::min(End, Begin + Traits.InstructionWindow - 1); ++At)
          if (generation(llvm::ArrayRef(Current).slice(At, 1), At) != Running)
            Add(At - std::min(At, Traits.InstructionWindow - 1), At + 1);
      Add(Begin, End);
      continue;
    }
    // A page can mix old stub bytes and newly generated code. Conservatively
    // watch every possible instruction start whose bytes may belong to a
    // different generation. watched() decides using the actual decoded size.
    for (uint64_t At = Begin; At < End; ++At)
      if (generation(llvm::ArrayRef(Current).slice(At, 1), At) != Running)
        Add(At - std::min(At, Traits.InstructionWindow - 1), At + 1);
  }
}

std::vector<MemoryWriteWatch> TransferObserver::writeWatches() const {
  std::vector<MemoryWriteWatch> Result;
  for (uint64_t Page = 0; Page < Visited.size(); ++Page) {
    if (!Visited[Page])
      continue;
    const uint64_t Offset = Page * value::PageSize, Address = Base + Offset;
    const uint64_t Size = std::min(
        value::PageSize + Traits.InstructionWindow - 1, Extent - Offset);
    if (!Result.empty() &&
        Address - Result.back().Address <= Result.back().Size)
      Result.back().Size = Address + Size - Result.back().Address;
    else
      Result.push_back({Address, Size});
  }
  return Result;
}

llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
TransferObserver::resuming(ProcessView &Process) {
  if (llvm::none_of(Visited, [](bool Seen) { return Seen; }))
    return std::nullopt;
  auto Mappings = Process.mappings();
  if (!Mappings)
    return Mappings.takeError();
  bool Changed = false;
  std::array<uint8_t, value::PageSize> Bytes;
  auto Mapped = [&](uint64_t Offset) {
    return llvm::any_of(*Mappings, [&](const auto &M) {
      return !M.Device && Base + Offset >= M.Address &&
             Base + Offset - M.Address < M.Size;
    });
  };
  auto Refresh = [&](uint64_t Offset, uint64_t Size) -> llvm::Error {
    auto Read = llvm::MutableArrayRef(Bytes).take_front(Size);
    if (Mapped(Offset)) {
      if (auto E = Process.read(Base + Offset, Read))
        return E;
    } else
      std::fill(Read.begin(), Read.end(), 0);
    auto Old = llvm::MutableArrayRef(Current).slice(Offset, Size);
    if (!std::equal(Read.begin(), Read.end(), Old.begin())) {
      llvm::copy(Read, Old.begin());
      Changed = true;
    }
    return llvm::Error::success();
  };
  for (uint64_t Page = 0; Page < Visited.size(); ++Page) {
    if (!Visited[Page])
      continue;
    const uint64_t Offset = Page * value::PageSize;
    if (!Mapped(Offset)) {
      Visited[Page] = false;
      Changed = true;
      continue;
    }
    if (auto E = Refresh(Offset, value::PageSize))
      return std::move(E);
    if (Page + 1 < Visited.size() && !Visited[Page + 1])
      if (auto E =
              Refresh(Offset + value::PageSize, Traits.InstructionWindow - 1))
        return std::move(E);
  }
  if (!Changed)
    return std::nullopt;
  refreshWatches();
  return Watches;
}

uint64_t TransferObserver::generation(llvm::ArrayRef<uint8_t> Bytes,
                                      uint64_t Offset) const {
  // Bytes belong to the generation after the last image they differ from;
  // bytes the loader mapped and nobody changed are generation zero.
  for (uint64_t N = Images.size(); N; --N)
    if (!std::equal(Bytes.begin(), Bytes.end(), Images[N - 1].begin() + Offset))
      return N;
  return 0;
}

llvm::Expected<std::vector<ExecutionWatch>>
TransferObserver::started(ProcessView &Process) {
  const auto Modules = Process.modules();
  const auto Main =
      llvm::find_if(Modules, [](const ProcessModuleView &M) { return M.Main; });
  if (Main == Modules.end())
    return failure(text::MainImage);
  if (Main->Size != Extent)
    return failure(text::ImageChanged);
  Base = Main->Base;
  if (auto E = snapshot(Process, Images.emplace_back()))
    return std::move(E);
  auto SP = Process.readRegister(Traits.StackPointer);
  if (!SP)
    return SP.takeError();
  InitialSP = (*SP)[0];
  EnteredProgram = Process.programInvocation();
  Visited.assign(Extent / value::PageSize, false);
  Current = Images.front();
  refreshWatches();
  return Watches;
}

llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
TransferObserver::invoking(ProcessView &Process) {
  if (!EnteredProgram && Process.programInvocation()) {
    auto SP = Process.readRegister(Traits.StackPointer);
    if (!SP)
      return SP.takeError();
    InitialSP = (*SP)[0];
    EnteredProgram = true;
  }
  // OS-owned callbacks may generate the next invocation's code in the same
  // page. A page executed by one invocation says nothing about the next one.
  Running = 0;
  Visited.assign(Visited.size(), false);
  refreshWatches();
  return Watches;
}

llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
TransferObserver::watched(ProcessView &Process, uint64_t PC) {
  if (PC < Base || PC - Base >= Extent)
    return failure(text::WatchOutside);
  const uint64_t Offset = PC - Base;
  auto Size = Process.instructionSize(PC);
  if (!Size)
    return Size.takeError();
  if (!*Size || *Size > Traits.InstructionWindow || *Size > Extent - Offset)
    return failure(text::InstructionExtent);
  std::vector<uint8_t> Instruction(*Size);
  if (auto E = Process.read(PC, Instruction))
    return std::move(E);
  const uint64_t Generation = generation(Instruction, Offset);
  if (Generation <= Running) {
    // Older code resumed, or more code of the running generation.
    if (Generation < Running)
      Visited.assign(Visited.size(), false);
    Running = Generation;
    const uint64_t Page = Offset / value::PageSize;
    if (!Visited[Page]) {
      auto Bytes = llvm::MutableArrayRef(Current).slice(Page * value::PageSize,
                                                        value::PageSize);
      if (auto E = Process.read(Base + Page * value::PageSize, Bytes))
        return std::move(E);
      Visited[Page] = true;
      refreshWatches();
    }
    return Watches;
  }
  auto SP = Process.readRegister(Traits.StackPointer);
  if (!SP)
    return SP.takeError();
  if (Seen.size() == defaults::MaxTransfers)
    return failure(text::TransferLimit);
  Seen.push_back(
      {Offset, (*SP)[0] == InitialSP, Generation, Process.programInvocation()});
  Capture Observed{};
  Observed.Base = Base;
  Observed.EntryRVA = Offset;
  Observed.Source = EntrySource::Transfer;
  if (auto E = snapshot(Process, Observed.Memory, &Observed.PageAccess))
    return std::move(E);
  const bool Accepted =
      Wanted ? Seen.size() == Wanted
             : Seen.back().StackBalanced && Seen.back().ProgramInvocation;
  if (!Accepted) {
    Images.push_back(std::move(Observed.Memory));
    Current = Images.back();
    Running = Images.size() - 1;
    Visited.assign(Visited.size(), false);
    Visited[Offset / value::PageSize] = true;
    refreshWatches();
    return Watches;
  }
  Observed.Baseline = std::move(Images.front());
  Observed.Transfers = Seen;
  if (Process.programInvocation()) {
    Observed.Initializers = Process.completedInitializers();
    auto ThreadLocal = Process.threadLocalMemory();
    if (!ThreadLocal)
      return ThreadLocal.takeError();
    Observed.ThreadLocal = std::move(*ThreadLocal);
  }
  // Several identities may share one address. Keep a named one, in a stable
  // order, so the rebuilt directory does not depend on enumeration order.
  for (auto &Export : Process.exports()) {
    ExportBinding Candidate{std::move(Export.Module), std::move(Export.Name),
                            Export.Ordinal};
    auto [Slot, Inserted] =
        Observed.Exports.try_emplace(Export.Address, Candidate);
    if (!Inserted && Candidate < Slot->second)
      Slot->second = std::move(Candidate);
  }
  Captured = std::move(Observed);
  return std::nullopt;
}
} // namespace neverd::unpack
