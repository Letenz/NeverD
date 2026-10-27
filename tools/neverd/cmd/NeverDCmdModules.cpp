//===- NeverDCmdModules.cpp - Module C API consumer -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Module C API consumer.
///

#include "../NeverDCLI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
namespace {
int error(const char *Text) {
  llvm::WithColor::error() << (Text ? Text : "module operation failed") << "\n";
  neverd_free_string(Text);
  return 1;
}
} // namespace
int runModules() {
  const auto &Action = ModulesAction.getValue();
  if (Action != "inspect" && Action != "validate" && Action != "lookup") {
    llvm::WithColor::error()
        << "modules action must be inspect, validate or lookup\n";
    return 1;
  }
  if ((Action == "lookup" && (ModulesID.empty() || ModulesSymbol.empty())) ||
      (Action != "lookup" &&
       (ModulesID.getNumOccurrences() || ModulesSymbol.getNumOccurrences()))) {
    llvm::WithColor::error()
        << "--module and --symbol are required only for modules lookup\n";
    return 1;
  }
  const char *Error = nullptr;
  const auto Set = neverd_module_set_from_file(
      ModulesConfiguration.getValue().c_str(), &Error);
  if (!Set)
    return error(Error);
  auto Cleanup =
      llvm::scope_exit([&] { neverd_module_set_destroy(Set, nullptr); });
  if (Action == "validate" && !neverd_module_set_prepare_eager(Set, &Error))
    return error(Error);
  const char *Report = Action == "lookup"
                           ? neverd_module_set_lookup_symbol_json(
                                 Set, ModulesID.getValue().c_str(),
                                 ModulesSymbol.getValue().c_str(), &Error)
                           : neverd_module_set_inspect_json(Set, &Error);
  if (!Report)
    return error(Error);
  llvm::outs() << Report << "\n";
  auto Parsed = llvm::json::parse(Report);
  neverd_free_string(Report);
  if (!Parsed) {
    llvm::WithColor::error() << llvm::toString(Parsed.takeError()) << "\n";
    return 1;
  }
  if (Action == "lookup") {
    auto Root = Parsed->getAsObject();
    auto Found = Root ? Root->getBoolean("found") : std::nullopt;
    if (!Found) {
      llvm::WithColor::error() << "module query has no found result\n";
      return 1;
    }
    return *Found ? 0 : 2;
  }
  return 0;
}
} // namespace neverd::cli
