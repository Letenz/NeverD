//===- ComparisonSource.cpp - Snapshot-bound source pages -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps source projection separate from native correspondence and proofs.
//===----------------------------------------------------------------------===//

#include "ComparisonSource.h"

#include "SessionImpl.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>

namespace neverd::sdk {
ComparisonSourcePage
readComparisonSource(Session &S, const diff::Program &P, uint32_t Index,
                     const ComparisonSourceOptions &Options,
                     const diff::ComparisonControl &Control) {
  ComparisonSourcePage Result;
  const auto Stopped = [&] {
    if (auto State = Control.stop()) {
      Result.State = *State;
      Result.Availability = "unavailable";
      Result.Reason = "source inspection stopped";
      Result.Document.clear();
      return true;
    }
    return false;
  };
  const auto &F = P.Functions.at(Index);
  if (Stopped())
    return Result;
  if (!F.hasBody()) {
    Result.Reason = "no native body is present in this comparison snapshot";
    return Result;
  }
  if (Options.MaxNativeInstructions &&
      P.instructionCount(F) > Options.MaxNativeInstructions) {
    Result.State = diff::ComparisonState::ResourceLimited;
    Result.Reason =
        "function exceeds the requested source instruction allowance";
    return Result;
  }
  try {
    // Select exactly the requested entry, including when an earlier SSA or
    // source request prepared another function. This changes only retained
    // analysis caches; the native comparison snapshot stays immutable.
    if (Options.Refresh || S.OnlyFunctionEntries.size() != 1 ||
        !S.OnlyFunctionEntries.contains(F.Address))
      S.selectFunction(F.Address);
    if (Control.Progress)
      Control.Progress("analysis", 0, 2);
    if (!neverd_prepare_function_view(&S, F.Address,
                                      Options.Representation.c_str()))
      throw std::runtime_error(S.LastError.empty() ? "function analysis failed"
                                                   : S.LastError);
    if (Stopped())
      return Result;
    if (Control.Progress)
      Control.Progress("render", 1, 2);
    using Text = std::unique_ptr<const char, decltype(&neverd_free_string)>;
    Text Raw(neverd_ir_view_json(&S, F.Address, Options.Representation.c_str(),
                                 Options.Offset, Options.Limit),
             neverd_free_string);
    if (!Raw)
      throw std::runtime_error(S.LastError.empty() ? "source emission failed"
                                                   : S.LastError);
    if (Stopped())
      return Result;
    auto Parsed = llvm::json::parse(Raw.get());
    if (!Parsed)
      throw std::runtime_error(llvm::toString(Parsed.takeError()));
    auto *Page = Parsed->getAsObject();
    if (!Page)
      throw std::runtime_error("source emitter returned no page object");
    if (!Page->getString("text")) {
      Result.Reason = Page->getString("reason")
                          .value_or(Page->getString("mapping_status")
                                        .value_or("source unavailable"))
                          .str();
      return Result;
    }
    const auto Total = Page->getInteger("total_lines");
    if (!Total || *Total < 0 || Options.Offset > uint64_t(*Total))
      throw std::runtime_error("source offset exceeds the rendered line count");

    std::map<va_t, uint32_t> Native;
    for (const auto &Block : P.blocks(F))
      for (uint32_t I = 0; I < Block.InstructionCount; ++I) {
        const auto Instruction = Block.FirstInstruction + I;
        Native.emplace(P.Instructions[Instruction].Address, Instruction);
      }
    auto *Rows = Page->getArray("rows");
    if (!Rows)
      throw std::runtime_error("source emitter returned no line records");
    for (auto &Value : *Rows) {
      if (Stopped())
        return Result;
      auto *Row = Value.getAsObject();
      if (!Row)
        throw std::runtime_error("source emitter returned an invalid line");
      llvm::json::Array Anchors;
      if (const auto *Addresses = Row->getArray("addresses"))
        for (const auto &Address : *Addresses) {
          const auto Name = Address.getAsString();
          va_t VA = 0;
          if (!Name || Name->getAsInteger(0, VA))
            throw std::runtime_error(
                "source emitter returned an invalid anchor");
          if (const auto It = Native.find(VA); It != Native.end())
            Anchors.push_back(llvm::json::Object{
                {"address", *Name}, {"instruction_index", It->second}});
        }
      // Keep the authoritative emitter's addresses, including its explicit
      // synthetic/unmapped rows. Only these separately checked anchors may
      // navigate within the captured comparison.
      (*Row)["comparison_anchors"] = std::move(Anchors);
    }
    Result.Document = std::move(*Page);
    Result.Availability = "available";
    if (Control.Progress)
      Control.Progress("ready", 2, 2);
    (void)Stopped();
  } catch (const std::exception &Error) {
    Result.Availability = "failed";
    Result.Reason = Error.what();
    Result.Document.clear();
    (void)Stopped();
  }
  Result.Reason.resize(std::min<size_t>(Result.Reason.size(),
                                        limits::diff::kMaxSourceReasonBytes));
  return Result;
}
} // namespace neverd::sdk
