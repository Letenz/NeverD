#include "StringReferences.h"

#include "TextFold.h"

#include <algorithm>
#include <limits>

namespace neverd::worker {
namespace {

constexpr std::size_t MaxStringReferencePage = 512;

} // namespace

void StringReferenceTable::reset(std::vector<StringReference> rows) {
  rows_ = std::move(rows);
  foldedText_.clear();
  order_.clear();
  orderKey_.clear();
  ordered_ = false;
}

Json StringReferenceTable::page(const Json &payload,
                                StringReferenceSource &source) {
  const auto offset =
      sizeField(payload, "offset", 0, std::numeric_limits<std::size_t>::max());
  const auto limit = std::max<std::size_t>(
      1, sizeField(payload, "limit", 128, MaxStringReferencePage));
  const auto sort = stringField(payload, "sort", "address", 16);
  if (sort != "address" && sort != "text" && sort != "function" &&
      sort != "type")
    throw Error("invalid_request",
                "sort must be address, text, function or type");
  const bool descending = payload.value("descending", false);
  // The filter and the text compare case-folded in every script.
  const std::string filter = foldText(stringField(payload, "filter", {}, 4096));
  std::string key = filter;
  key += '\0';
  key += sort;
  key += descending ? 'd' : 'a';
  if (!ordered_ || key != orderKey_) {
    if ((!filter.empty() || sort == "text") &&
        foldedText_.size() != rows_.size()) {
      foldedText_.clear();
      foldedText_.reserve(rows_.size());
      for (const auto &ref : rows_)
        foldedText_.push_back(foldText(source.text(ref)));
    }
    order_.clear();
    const bool hexFilter =
        filter.find_first_not_of("0123456789abcdefx") == std::string::npos;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
      const auto &ref = rows_[i];
      if (filter.empty() || foldedText_[i].find(filter) != std::string::npos ||
          foldText(source.functionName(ref)).find(filter) !=
              std::string::npos ||
          (hexFilter && hexAddress(ref.from).find(filter) != std::string::npos))
        order_.push_back(static_cast<std::uint32_t>(i));
    }
    if (sort != "address") {
      // Each row's key once, then the rows in key order.
      std::vector<std::string> keys(rows_.size());
      for (const auto i : order_)
        keys[i] = sort == "text"       ? foldedText_[i]
                  : sort == "function" ? foldText(source.functionName(rows_[i]))
                                       : std::string(source.encoding(rows_[i]));
      std::stable_sort(
          order_.begin(), order_.end(),
          [&](std::uint32_t a, std::uint32_t b) { return keys[a] < keys[b]; });
    }
    if (descending)
      std::reverse(order_.begin(), order_.end());
    orderKey_ = std::move(key);
    ordered_ = true;
  }
  Json items = Json::array();
  const std::size_t total = order_.size();
  for (std::size_t i = std::min(offset, total);
       i < total && items.size() < limit; ++i) {
    const auto &ref = rows_[order_[i]];
    Json item = {{"address", hexAddress(ref.from)},
                 {"string_address", hexAddress(ref.to)},
                 {"text", std::string(source.text(ref))},
                 {"type", source.type(ref)},
                 {"encoding", std::string(source.encoding(ref))},
                 {"kind", std::string(refKindName(ref.kind))},
                 {"disasm", source.disassembly(ref.from)},
                 {"function", std::string(source.functionName(ref))}};
    if (const auto entry = source.functionEntry(ref))
      item["function_address"] = hexAddress(*entry);
    if (ref.via)
      item["via"] = hexAddress(ref.via);
    items.push_back(std::move(item));
  }
  const bool complete = offset >= total || items.size() >= total - offset;
  return {{"items", std::move(items)},
          {"total", total},
          {"offset", offset},
          {"next_offset", complete ? Json(nullptr) : Json(offset + limit)},
          {"complete", complete}};
}

} // namespace neverd::worker
