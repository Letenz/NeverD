//===- NeverDCLIFunctions.cpp - Functions holding addresses ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDCLIFunctions.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <algorithm>

namespace neverd::cli {

FunctionLocator::FunctionLocator(neverd_session_t Sess) {
  const int Count = neverd_func_count(Sess);
  std::vector<std::pair<neverd_va_t, uint64_t>> Sizes;
  for (int I = 0; I < Count; ++I) {
    Function F;
    F.Entry = neverd_func_entry(Sess, I);
    const char *Name = neverd_func_name(Sess, I);
    F.Name = Name ? Name : "";
    neverd_free_string(Name);
    Functions.push_back(std::move(F));
    Sizes.push_back(
        {Functions.back().Entry,
         static_cast<uint64_t>(std::max(0, neverd_func_size(Sess, I)))});
  }
  // Segment ends bound functions without a recorded size.
  std::vector<std::pair<neverd_va_t, neverd_va_t>> Segments;
  if (const char *Json = neverd_segments_json(Sess)) {
    if (auto Parsed = llvm::json::parse(Json))
      if (const auto *Rows = Parsed->getAsArray())
        for (const auto &Row : *Rows)
          if (const auto *Object = Row.getAsObject()) {
            uint64_t Start = 0, Size = 0;
            const auto Va = Object->getString("va");
            const auto Bytes = Object->getString("size");
            if (Va && Bytes && !Va->getAsInteger(0, Start) &&
                !Bytes->getAsInteger(0, Size))
              Segments.push_back({Start, Start + Size});
          }
    neverd_free_string(Json);
  }
  std::vector<size_t> Order(Functions.size());
  for (size_t I = 0; I < Order.size(); ++I)
    Order[I] = I;
  std::stable_sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
    return Functions[A].Entry < Functions[B].Entry;
  });
  std::vector<Function> Sorted;
  std::vector<uint64_t> SortedSizes;
  for (size_t I : Order) {
    Sorted.push_back(std::move(Functions[I]));
    SortedSizes.push_back(Sizes[I].second);
  }
  Functions = std::move(Sorted);
  for (size_t I = 0; I < Functions.size(); ++I) {
    Function &F = Functions[I];
    neverd_va_t Bound = F.Entry + 1;
    for (const auto &[Start, End] : Segments)
      if (F.Entry >= Start && F.Entry < End)
        Bound = End;
    if (I + 1 < Functions.size())
      Bound = std::min(Bound, Functions[I + 1].Entry);
    F.End = SortedSizes[I] ? std::min(Bound, F.Entry + SortedSizes[I]) : Bound;
    if (F.End <= F.Entry)
      F.End = F.Entry + 1;
  }
}

std::string FunctionLocator::locate(neverd_va_t Address) const {
  auto It = std::upper_bound(
      Functions.begin(), Functions.end(), Address,
      [](neverd_va_t Value, const Function &F) { return Value < F.Entry; });
  if (It == Functions.begin())
    return {};
  --It;
  if (Address >= It->End)
    return {};
  if (Address == It->Entry)
    return It->Name;
  return It->Name + "+0x" + llvm::utohexstr(Address - It->Entry);
}

} // namespace neverd::cli
