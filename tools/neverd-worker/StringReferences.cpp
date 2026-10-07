#include "StringReferences.h"

#include <algorithm>
#include <limits>

namespace neverd::worker {
namespace {

constexpr std::size_t MaxStringReferencePage = 512;

/// The ASCII lower case of \p c; other bytes are themselves.
char lowerASCII(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Whether \p text contains \p needle, which is lower case, ignoring the
/// case of ASCII letters.
bool containsFolded(std::string_view text, std::string_view needle) {
  return needle.empty() || std::search(text.begin(), text.end(), needle.begin(),
                                       needle.end(), [](char a, char b) {
                                         return lowerASCII(a) == b;
                                       }) != text.end();
}

/// Orders text ignoring the case of ASCII letters.
bool lessFolded(std::string_view a, std::string_view b) {
  return std::lexicographical_compare(
      a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return static_cast<unsigned char>(lowerASCII(x)) <
               static_cast<unsigned char>(lowerASCII(y));
      });
}

} // namespace

void StringReferenceTable::reset(std::vector<StringReference> rows) {
  rows_ = std::move(rows);
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
  auto filter = stringField(payload, "filter", {}, 4096);
  std::transform(filter.begin(), filter.end(), filter.begin(), lowerASCII);
  std::string key = filter;
  key += '\0';
  key += sort;
  key += descending ? 'd' : 'a';
  if (!ordered_ || key != orderKey_) {
    order_.clear();
    const bool hexFilter =
        filter.find_first_not_of("0123456789abcdefx") == std::string::npos;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
      const auto &ref = rows_[i];
      if (filter.empty() || containsFolded(source.text(ref), filter) ||
          containsFolded(source.functionName(ref), filter) ||
          (hexFilter && containsFolded(hexAddress(ref.from), filter)))
        order_.push_back(static_cast<std::uint32_t>(i));
    }
    const auto order = [&](auto field) {
      std::stable_sort(order_.begin(), order_.end(),
                       [&](std::uint32_t a, std::uint32_t b) {
                         return lessFolded(field(rows_[a]), field(rows_[b]));
                       });
    };
    if (sort == "text")
      order([&](const StringReference &ref) { return source.text(ref); });
    else if (sort == "function")
      order(
          [&](const StringReference &ref) { return source.functionName(ref); });
    else if (sort == "type")
      order([&](const StringReference &ref) { return source.encoding(ref); });
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
