#pragma once

#include "Protocol.h"
#include "References.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::worker {

/// An instruction that refers to string text, as the engine's
/// neverd_string_refs_json reports it.
struct StringReference {
  /// The instruction, the referenced byte, and the relocated slot it reads
  /// the address from, or 0 for a direct reference.
  std::uint64_t from = 0, to = 0, via = 0;
  /// The string in the listing's string list, and where the referenced text
  /// begins in its UTF-8 value.
  std::uint32_t string = 0, textOffset = 0;
  RefKind kind = RefKind::Offset;
};

/// What a string reference table shows of its rows: the listing's text,
/// names and instructions for them.
class StringReferenceSource {
public:
  virtual ~StringReferenceSource() = default;
  virtual std::string_view text(const StringReference &ref) const = 0;
  /// The engine's encoding name and the listing's type column for it.
  virtual std::string_view encoding(const StringReference &ref) const = 0;
  virtual std::string type(const StringReference &ref) const = 0;
  /// The function holding the instruction, if one does.
  virtual std::optional<std::uint64_t>
  functionEntry(const StringReference &ref) const = 0;
  virtual std::string_view functionName(const StringReference &ref) const = 0;
  /// The instruction at \p address as the listing shows it.
  virtual std::string disassembly(std::uint64_t address) = 0;
};

/// The `string_references` table.  A query filters the rows by text,
/// function or address, ignoring case in any script, and sorts them
/// by address, text, function or type; its rows are kept for its pages.
class StringReferenceTable {
public:
  /// Replaces the rows, which come in instruction order.
  void reset(std::vector<StringReference> rows);
  /// One page for {offset, limit, filter, sort, descending}.
  Json page(const Json &payload, StringReferenceSource &source);

private:
  std::vector<StringReference> rows_;
  /// Each row's text case-folded, made when a query first needs it.
  std::vector<std::string> foldedText_;
  std::vector<std::uint32_t> order_;
  std::string orderKey_;
  bool ordered_ = false;
};

} // namespace neverd::worker
