//===- NeverDCAPISwitches.cpp - C API: switch jump tables -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The jump tables whole-program analysis recovered, as a listing names and
/// lays them out: the dispatch, the table, how each slot stores its target
/// and the case each target serves.
///
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/ir/low/LowIR.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <optional>

using namespace neverd;
using namespace neverd::sdk;

namespace {

/// The most functions one page covers.
constexpr int MaxSwitchFunctionsPerPage = 4096;

/// How every slot of a table stores its target.
enum class EntryForm { Absolute, TableRelative, ImageRelative };

const char *entryFormName(EntryForm Form) {
  switch (Form) {
  case EntryForm::Absolute:
    return "absolute";
  case EntryForm::TableRelative:
    return "table_relative";
  case EntryForm::ImageRelative:
    return "image_relative";
  }
  return "";
}

/// The value physical slot \p Slot of the table stores, as the dispatch reads
/// it: at the table's address plus the slot times its stride, in storage the
/// table owns.
std::optional<uint64_t> slotValue(const BinaryImage &Img,
                                  const JumpTable &Table, uint64_t Slot) {
  if (!Table.HasBaseAddr || Table.EntrySize == 0 || Table.EntrySize > 8 ||
      Table.EntryStride < Table.EntrySize ||
      Slot > (InvalidVA - Table.BaseAddr) / Table.EntryStride)
    return std::nullopt;
  const va_t Address = Table.BaseAddr + Slot * Table.EntryStride;
  if (!Table.ownsStorageAddress(Address))
    return std::nullopt;
  const Segment *Seg = Img.getSegmentFor(Address);
  if (!Seg || Address - Seg->VA > Seg->Data.size() ||
      Seg->Data.size() - (Address - Seg->VA) < Table.EntrySize)
    return std::nullopt;
  uint64_t Value = 0;
  for (unsigned I = 0; I < Table.EntrySize; ++I)
    Value |= uint64_t(Seg->Data[Address - Seg->VA + I]) << (8 * I);
  if (Table.IsSigned && Table.EntrySize < 8)
    Value =
        static_cast<uint64_t>(llvm::SignExtend64(Value, Table.EntrySize * 8));
  return Value;
}

/// The form the table's encoding names, when every slot that dispatches to
/// a target stores exactly that target in it.
std::optional<EntryForm> entryForm(const BinaryImage &Img,
                                   const JumpTable &Table) {
  if (!Table.HasDispatchSlotMap ||
      Table.SlotIndices.size() != Table.Targets.size() ||
      Table.Targets.empty() || Table.TwoTableSelect || Table.TwoLevelIndex ||
      Table.MutatedUnsafe || Table.PreScaledIndex)
    return std::nullopt;
  EntryForm Form = EntryForm::Absolute;
  uint64_t Anchor = 0;
  if (Table.IsPEImageRelativeRVA) {
    Form = EntryForm::ImageRelative;
    Anchor = Img.Base;
  } else if (Table.HasTargetBase) {
    // A compact table scales its entries from another anchor.
    return std::nullopt;
  } else if (Table.IsRelative) {
    Form = EntryForm::TableRelative;
    Anchor = Table.BaseAddr;
  }
  for (size_t K = 0; K < Table.Targets.size(); ++K) {
    const auto Value = slotValue(Img, Table, Table.SlotIndices[K]);
    if (!Value || Anchor + *Value != Table.Targets[K])
      return std::nullopt;
  }
  return Form;
}

llvm::json::Value switchJson(const BinaryImage &Img, va_t Function,
                             const JumpTable &Table) {
  const auto address = [](bool Known, va_t Address) -> llvm::json::Value {
    return Known ? llvm::json::Value(vaHex(Address)) : nullptr;
  };
  llvm::json::Object Object;
  Object["function"] = vaHex(Function);
  Object["jump"] = vaHex(Table.InsnAddr);
  Object["load"] =
      address(Table.TableLoadAddr != InvalidVA, Table.TableLoadAddr);
  Object["table"] = address(Table.HasBaseAddr, Table.BaseAddr);
  Object["entry_size"] = static_cast<int64_t>(Table.EntrySize);
  Object["stride"] = static_cast<int64_t>(Table.EntryStride);
  llvm::json::Array Storage;
  for (const JumpTableStorageRange &Run : Table.StorageRanges)
    Storage.push_back(llvm::json::Array{
        vaHex(Run.BaseAddr), static_cast<int64_t>(Run.EntrySize),
        static_cast<int64_t>(Run.EntryStride),
        static_cast<int64_t>(Run.PhysicalSlotCount)});
  Object["storage"] = std::move(Storage);
  const auto Form = entryForm(Img, Table);
  Object["form"] = Form ? llvm::json::Value(entryFormName(*Form)) : nullptr;
  const bool Slots = Table.HasDispatchSlotMap &&
                     Table.SlotIndices.size() == Table.Targets.size();
  llvm::json::Array Targets;
  for (size_t K = 0; K < Table.Targets.size(); ++K)
    Targets.push_back(llvm::json::Array{
        vaHex(Table.Targets[K]), Table.caseLabel(K),
        Slots ? llvm::json::Value(static_cast<int64_t>(Table.SlotIndices[K]))
              : nullptr});
  Object["targets"] = std::move(Targets);
  return llvm::json::Value(std::move(Object));
}

} // namespace

const char *neverd_switches_json(neverd_session_t Sess, neverd_va_t FirstEntry,
                                 int MaxFunctions) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  // A pipeline restricted to some functions arbitrated their tables alone.
  if (!S->PipeRan || !S->PipeResult.Success ||
      !S->OnlyFunctionEntries.empty()) {
    S->setError("switch tables are published after whole-program analysis");
    return nullptr;
  }
  const size_t Limit = static_cast<size_t>(
      std::clamp(MaxFunctions, 1, MaxSwitchFunctionsPerPage));
  std::vector<const LowFunc *> Functions;
  Functions.reserve(S->PipeResult.LowFuncs.size());
  for (const LowFunc &F : S->PipeResult.LowFuncs)
    if (F.Entry >= FirstEntry)
      Functions.push_back(&F);
  std::sort(
      Functions.begin(), Functions.end(),
      [](const LowFunc *A, const LowFunc *B) { return A->Entry < B->Entry; });
  llvm::json::Array Switches;
  for (size_t I = 0; I < std::min(Limit, Functions.size()); ++I)
    for (const JumpTable &Table : Functions[I]->JumpTables)
      Switches.push_back(switchJson(S->Img, Functions[I]->Entry, Table));
  llvm::json::Object Result;
  Result["switches"] = std::move(Switches);
  Result["next_entry"] = Functions.size() > Limit
                             ? llvm::json::Value(vaHex(Functions[Limit]->Entry))
                             : nullptr;
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}
