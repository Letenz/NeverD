#pragma once

#include "OperandFormat.h"
#include "Protocol.h"

#include "neverd/sdk/NeverDCAPITypes.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace neverd::worker {

/// Address class of the navigation overview and listing prefixes
/// (AddressClasses.def).
enum class AddressClass : std::uint8_t {
#define NEVERD_ADDRESS_CLASS(Name, Code) Name = Code,
#include "AddressClasses.def"
};

/// Address-ordered listing of one loaded image, formatted as an interactive
/// disassembler shows it: segment directives, function headers, labels,
/// instructions, data items and reference comments.  The listing reads only
/// public C ABI queries.  Instruction semantics (transfers, references) come
/// from the engine's lift; this class only names and lays them out.
///
/// The background reference index decodes every known function once, in
/// bounded steps, so a host can interleave it with interactive requests.
class Listing {
public:
  explicit Listing(neverd_session_t session);
  ~Listing();
  Listing(const Listing &) = delete;
  Listing &operator=(const Listing &) = delete;

  /// Names, comments or function boundaries changed.
  void invalidate();
  /// The neverd_strings_ex_json options strings are found with, as JSON
  /// text; empty for the engine's defaults.
  void setStringOptions(std::string options);
  /// `listing`: lines around an address.
  Json page(const Json &payload);
  /// `overview`: address classes in equal linear buckets.
  Json overview(const Json &payload);
  /// `xrefs` from the reference index or, for "from", the instruction's lift.
  Json references(std::uint64_t address, const Json &payload);
  /// `string_references`: every instruction that refers to a string, directly
  /// or through a pointer slot, as a filtered and sorted table page.
  Json stringReferences(const Json &payload);
  /// Resolve an automatic or listing-local name to its address.
  std::optional<std::uint64_t> resolveName(const std::string &name);
  /// Whether \p address is an import's slot or the entry of its thunk.
  bool isImport(std::uint64_t address);
  /// Build the reference index again, as for a changed function list.
  void reindex();
  /// Formatted instruction lines (no prefixes) of [start, end), for graph
  /// nodes.  Each element is {address, text, spans}.
  Json blockLines(std::uint64_t start, std::uint64_t end);
  /// Functions in address order under their display names:
  /// [{"name","engine_name","address","size","library","thunk","exported"}].
  const Json &functionRows();
  /// Engine function name -> display name, for engine text such as
  /// pseudocode that spells functions the engine's way.
  const std::unordered_map<std::string, std::string> &functionAliases();
  /// `names`, `imports`, `exports`, `segments_ex` table rows.
  Json names();
  Json regions();

  /// Read the jump tables of whole-program analysis, once it has run: their
  /// names, slots, comments and references join the listing.
  void loadSwitches();

  bool hasIdleWork() const;
  /// Advance the reference index by one bounded step.
  void idleStep();
  /// {state, done, total, generation} of the reference index.
  Json indexState() const;
  /// Changes whenever published listing text can change.
  std::uint64_t generation() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace neverd::worker
