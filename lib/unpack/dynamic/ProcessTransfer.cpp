//===- ProcessTransfer.cpp - Transfers into generated code ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessTransfer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <tuple>

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

std::vector<ExecutionWatch> TransferObserver::watches() const {
  std::vector<ExecutionWatch> Result;
  for (uint64_t Page = 0; Page < Executed.size(); ++Page) {
    if (Executed[Page])
      continue;
    const uint64_t Address = Base + Page * value::PageSize;
    if (!Result.empty() &&
        Result.back().Address + Result.back().Size == Address)
      Result.back().Size += value::PageSize;
    else
      Result.push_back({Address, value::PageSize});
  }
  return Result;
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
  Executed.assign(Extent / value::PageSize, false);
  return watches();
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
  Executed.assign(Executed.size(), false);
  return watches();
}

llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
TransferObserver::watched(ProcessView &Process, uint64_t PC) {
  if (PC < Base || PC - Base >= Extent)
    return failure(text::WatchOutside);
  const uint64_t Offset = PC - Base;
  // Compare the longest possible instruction. The page that holds its first
  // byte is committed; a following page need not be.
  const uint64_t Window = std::min(Traits.InstructionWindow, Extent - Offset);
  const uint64_t InPage =
      std::min(Window, value::PageSize - Offset % value::PageSize);
  std::vector<uint8_t> Current(Window);
  if (auto E =
          Process.read(PC, llvm::MutableArrayRef(Current).take_front(InPage)))
    return std::move(E);
  uint64_t Compared = InPage;
  if (InPage < Window) {
    if (auto E = Process.read(
            PC + InPage, llvm::MutableArrayRef(Current).drop_front(InPage)))
      llvm::consumeError(std::move(E));
    else
      Compared = Window;
  }
  const uint64_t Generation =
      generation(llvm::ArrayRef(Current).take_front(Compared), Offset);
  if (Generation <= Running) {
    // Older code resumed, or more code of the running generation.
    if (Generation < Running)
      Executed.assign(Executed.size(), false);
    Running = Generation;
    Executed[Offset / value::PageSize] = true;
    return watches();
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
    Running = Images.size() - 1;
    Executed.assign(Executed.size(), false);
    Executed[Offset / value::PageSize] = true;
    return watches();
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
  const auto Key = [](const ExportBinding &B) {
    return std::tuple(B.Name.empty(), B.Module, B.Name, B.Ordinal);
  };
  for (auto &Export : Process.exports()) {
    ExportBinding Candidate{std::move(Export.Module), std::move(Export.Name),
                            Export.Ordinal};
    auto [Slot, Inserted] =
        Observed.Exports.try_emplace(Export.Address, Candidate);
    if (!Inserted && Key(Candidate) < Key(Slot->second))
      Slot->second = std::move(Candidate);
  }
  Captured = std::move(Observed);
  return std::nullopt;
}
} // namespace neverd::unpack
