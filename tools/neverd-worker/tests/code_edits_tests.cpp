#include "CodeEdits.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

using namespace neverd::worker;

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
Json page(const std::string &text) {
  Json rows = Json::array();
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line))
    rows.push_back({{"line", rows.size()}, {"addresses", Json::array()}});
  return {{"text", text},
          {"rows", rows},
          {"prelude", {{"end_byte", 0}}},
          {"source_names", Json::array()},
          {"library_regions", Json::array()}};
}
std::optional<std::uint64_t> resolve(const std::string &name) {
  if (name == "target")
    return 0x1000;
  if (name == "callee")
    return 0x1100;
  return std::nullopt;
}
void decorate(Json &view, const Json &row = nullptr,
              const Json &renames = Json::array(),
              const CodeEdits::FunctionAliases &displayNames = {}) {
  CodeEdits::decorate(view, row, 0x1000, "c", renames, Json::array(),
                      displayNames, resolve);
}
Json target(const Json &view, const std::string &original) {
  for (const auto &name : view.at("code_names"))
    if (name.at("original") == original && name.at("kind") == "local")
      return name;
  return nullptr;
}
void refused(const Json &view, const Json &request, const std::string &code) {
  try {
    CodeEdits::validateChange(view, request);
    check(false, "Invalid edit accepted");
  } catch (const Error &error) {
    check(error.code == code, "Wrong edit rejection");
  }
}
Json sourceName(std::size_t begin, std::size_t end) {
  return {{"begin_byte", begin}, {"end_byte", end}, {"address", "0x1100"}};
}
void sourceSpanBoundaries() {
  auto spelling = page("fini fini fini fini fini");
  spelling["identifier_symbols"] = Json::array(
      {{{"identifier", "fini"}, {"symbol", "_fini"}, {"address", "0x1100"}}});
  spelling["source_names"] =
      Json::array({sourceName(6, 7), sourceName(1, 3), sourceName(16, 16),
                   sourceName(9, 10)});
  CodeEdits::spellIdentifierSymbols(spelling);
  std::vector<std::size_t> added;
  for (const auto &name : spelling.at("source_names"))
    if (name.contains("identifier"))
      added.push_back(name.at("begin_byte").get<std::size_t>());
  check(added == std::vector<std::size_t>({10, 20}),
        "Spelling changed partial overlap or touching span boundaries");

  const std::string text = "int target(void) { return callee(); }\n";
  const auto begin = text.find("callee");
  auto adjacent = page(text);
  adjacent["source_names"] = Json::array(
      {sourceName(begin + 3, begin + 6), sourceName(begin, begin + 3)});
  decorate(adjacent);
  const auto &names = adjacent.at("code_names");
  check(names.size() == 4 && names[0].at("original") == "lee" &&
            names[1].at("original") == "cal" &&
            names[3].at("original") == "callee",
        "Adjacent spans were merged or partial coverage hid the full token");

  // Accepted spans may arrive in either order. Containment and duplicate
  // starts must be rejected just like a partial overlap.
  const std::vector<std::pair<std::size_t, std::size_t>> overlaps = {
      {0, 6}, {1, 3}, {3, 8}, {0, 3}};
  for (const auto &[left, right] : overlaps) {
    for (bool reverse : {false, true}) {
      auto invalid = page(text);
      auto a = sourceName(begin, begin + 6);
      auto b = sourceName(begin + left, begin + right);
      invalid["source_names"] =
          reverse ? Json::array({b, a}) : Json::array({a, b});
      try {
        decorate(invalid);
        check(false, "Overlapping explicit source names were accepted");
      } catch (const Error &error) {
        check(error.code == "invalid_engine_result",
              "Overlapping source names produced the wrong error");
      }
    }
  }

  auto ignored = page(text);
  ignored["source_names"] = Json::array({sourceName(begin, begin + 6),
                                         sourceName(begin + 2, begin + 2),
                                         sourceName(begin + 3, begin + 1),
                                         sourceName(begin, text.size() + 1),
                                         {{"begin_byte", begin + 1},
                                          {"end_byte", begin + 4},
                                          {"symbol", "unknown"}}});
  decorate(ignored);
  check(ignored.at("code_names").size() == 2,
        "Invalid or unresolved spans interfered with accepted source names");
}

void manySourceNames() {
  std::string text = "int target(void) {\n";
  Json spans = Json::array();
  for (unsigned i = 0; i < 256; ++i) {
    spans.push_back(sourceName(text.size() + 2, text.size() + 8));
    text += "  callee();\n";
  }
  text += "  return 0;\n}\n";
  std::reverse(spans.begin(), spans.end());
  auto view = page(text);
  view["source_names"] = spans;
  decorate(view, nullptr,
           Json::array({{{"addr", "0x1100"}, {"renamed", "renamed_callee"}}}));
  check(view.at("code_names").size() == 257 &&
            view.at("source_names").size() == 256 &&
            view.at("rows").size() == 259,
        "Unordered source names lost edit targets or source rows");
  const auto &rendered = view.at("text").get_ref<const std::string &>();
  for (const auto &span : view.at("source_names")) {
    const auto begin = span.at("begin_byte").get<std::size_t>();
    const auto end = span.at("end_byte").get<std::size_t>();
    check(end <= rendered.size() && begin < end &&
              rendered.substr(begin, end - begin) == "renamed_callee",
          "A renamed explicit source span lost its displayed edit target");
  }
}
} // namespace

int main() {
  sourceSpanBoundaries();
  manySourceNames();
  const std::string source =
      "extern int32_t global_data; /* 0x2000 */\n"
      "int32_t callee(int32_t);\n"
      "int32_t target(int32_t value) {\n"
      "  int32_t temp = value + callee(value);\n"
      "  object.value = temp; // value global_data callee\n"
      "  const char *str = \"value callee global_data\";\n"
      "  const char *raw = u8R\"tag(\" value global_data \" )tag\";\n"
      "  const char *rust_raw = r##\"\" value callee \"##;\n"
      "  return value + global_data;\n"
      "}\n";
  auto original = page(source);
  decorate(original);
  const auto variable = target(original, "value");
  check(!variable.is_null(), "Declared parameter has no edit target");
  const Json rename = {
      {"address", "0x1000"},   {"representation", "c"},
      {"kind", "name"},        {"original", "value"},
      {"name", "input_value"}, {"identity", variable.at("identity")}};
  CodeEdits::validateChange(original, rename);
  auto row = CodeEdits::change(nullptr, rename);
  const Json note = {{"address", "0x1000"},
                     {"representation", "c"},
                     {"kind", "comment"},
                     {"line", 3},
                     {"anchor", "  int32_t temp = value + callee(value);"},
                     {"text", "\xE6\xB3\xA8\xE9\x87\x8A\nsecond */ line"}};
  CodeEdits::validateChange(original, note);
  row = CodeEdits::change(row, note);
  const Json imageNames = Json::array({{{"addr", "0x1100"},
                                        {"original", "callee"},
                                        {"renamed", "renamed_callee"}},
                                       {{"addr", "0x2000"},
                                        {"original", "global_data"},
                                        {"renamed", "renamed_data"}}});
  auto edited = page(source);
  decorate(edited, row, imageNames);
  const auto text = edited.at("text").get<std::string>();
  check(text.find("temp = input_value + renamed_callee(input_value)") !=
            std::string::npos,
        "Variables or called functions did not change");
  check(text.find("return input_value + renamed_data") != std::string::npos,
        "Explicit global address was not renamed");
  check(text.find("object.value = temp; // value global_data callee") !=
                std::string::npos &&
            text.find("\"value callee global_data\"") != std::string::npos &&
            text.find("u8R\"tag(\" value global_data \" )tag\"") !=
                std::string::npos &&
            text.find("r##\"\" value callee \"##") != std::string::npos,
        "A member, comment or string was changed");
  check(text.find("second * / line */") != std::string::npos &&
            std::count(text.begin(), text.end(), '\n') ==
                std::count(source.begin(), source.end(), '\n'),
        "An inline annotation changed the line mapping");
  for (const std::string prefix : {"R", "u8R", "uR", "UR", "LR"}) {
    const auto literal = prefix + "\"tag(\" value callee \" )tag\"";
    auto rawView = page("int target(int value) { const char *str = " + literal +
                        "; return value; }\n");
    decorate(rawView);
    auto rawRename = rename;
    rawRename["identity"] = target(rawView, "value").at("identity");
    const auto rawRow = CodeEdits::change(nullptr, rawRename);
    decorate(rawView, rawRow, imageNames);
    const auto rawText = rawView.at("text").get<std::string>();
    check(rawText.find(literal) != std::string::npos &&
              rawText.find("return input_value") != std::string::npos,
          "A prefixed raw string was mistaken for editable identifiers");
  }
  for (const auto &name : edited.at("code_names")) {
    const auto begin = name.at("begin_byte").get<std::size_t>();
    const auto end = name.at("end_byte").get<std::size_t>();
    check(end > begin && end <= text.size(), "Edit span outside UTF-8 text");
    const auto spelling = text.substr(begin, end - begin);
    check(spelling == "input_value" || spelling == "temp" ||
              spelling == "str" || spelling == "raw" ||
              spelling == "rust_raw" || spelling == "target" ||
              spelling == "renamed_callee" || spelling == "renamed_data",
          "Edit span does not cover the displayed identifier");
  }
  auto conflict = rename;
  conflict["name"] = "temp";
  refused(edited, conflict, "invalid_request");
  auto obsolete = rename;
  obsolete["identity"] = "0x1";
  refused(edited, obsolete, "stale_target");
  auto movedNote = note;
  movedNote["line"] = 4;
  refused(edited, movedNote, "stale_target");
  auto different = page(source + "// regenerated view\n");
  decorate(different, row);
  check(different.at("code_edits_stale") == true &&
            different.at("text").get<std::string>().find("input_value") ==
                std::string::npos,
        "Obsolete aliases were applied to a different rendering");
  // The listing names generic functions after what they are: main for the
  // one the C runtime's start routine calls, start for the image entry.  A
  // source view shows those names too, and the analyst's rename still wins.
  const CodeEdits::FunctionAliases displayNames = {{"target", "main"},
                                                   {"callee", "start"}};
  auto named = page(source);
  decorate(named, nullptr, Json::array(), displayNames);
  const auto namedText = named.at("text").get<std::string>();
  check(namedText.find("int32_t main(int32_t value) {") != std::string::npos &&
            namedText.find("temp = value + start(value)") != std::string::npos,
        "A source view did not show the listing's display names");
  check(namedText.find("// value global_data callee") != std::string::npos,
        "A display name changed a comment");
  auto renamedOverDisplay = page(source);
  decorate(renamedOverDisplay, nullptr, imageNames, displayNames);
  check(renamedOverDisplay.at("text").get<std::string>().find(
            "temp = value + renamed_callee(value)") != std::string::npos,
        "A display name replaced the analyst's rename");
  auto shadowed =
      page("int target(int value) { { int value = 1; } return value; }\n");
  decorate(shadowed);
  check(target(shadowed, "value").is_null(),
        "Shadowed declarations were guessed to be one variable");
  auto helpers =
      page("int synthetic_helper(int other) { return other; }\n" + source);
  decorate(helpers);
  check(target(helpers, "other").is_null() &&
            !target(helpers, "value").is_null(),
        "A synthetic helper was treated as the requested image function");

  // The source-cache transport fixture has 9,000 rows. Comments must not
  // become linker evidence or enter a whole-document regex search.
  std::string large =
      "extern int shim(int (*callback)(int)) __asm__(\"callee\");\n"
      "int target(void) { return shim(0) + fake(0); }\n";
  for (unsigned i = 0; i < 9000; ++i)
    large += "// " + std::string(512, 'x') +
             " int fake(int) __asm__(\"callee\"); neverd.image: 0x1100\n";
  auto largeView = page(large);
  decorate(largeView, nullptr, imageNames);
  check(largeView.at("text").get<std::string>().find(
            "return renamed_callee(0) + fake(0)") != std::string::npos,
        "Real asm linkage was lost or a comment supplied fake linkage");
  check(largeView.at("rows").size() == 9002 &&
            largeView.at("rows").back().at("code_anchor") ==
                large.substr(large.rfind('\n', large.size() - 2) + 1,
                             large.size() -
                                 large.rfind('\n', large.size() - 2) - 2),
        "Large comment source lost its final row or edit anchor");

  // C calls the start-up function `_fini` fini, as the C runtime defines its
  // own: the definition and the call find the function, and read as its
  // symbol, but the comments that spell them do not. The engine spells the
  // address in upper case.
  auto renamedView = page("/* _fini */\n"
                          "void fini(void) {\n"
                          "  return;\n"
                          "}\n"
                          "void other(void) { fini(); /* fini */ }\n");
  renamedView["identifier_symbols"] = Json::array(
      {{{"identifier", "fini"}, {"symbol", "_fini"}, {"address", "0x10AB"}}});
  CodeEdits::spellIdentifierSymbols(renamedView);
  check(renamedView.at("source_names").size() == 2,
        "An identifier the engine renamed is not a source name where spelled");
  for (const auto &name : renamedView.at("source_names"))
    check(name.at("identifier") == "fini" && name.at("symbol") == "_fini" &&
              name.at("address") == "0x10ab",
          "A renamed identifier lost its symbol or address");
  CodeEdits::decorate(
      renamedView, nullptr, 0x10ab, "c",
      Json::array({{{"addr", "0x10ab"}, {"renamed", "shutdown"}}}),
      Json::array(), {}, resolve);
  const auto &renamedText =
      renamedView.at("text").get_ref<const std::string &>();
  check(renamedText.find("void shutdown(void) {") != std::string::npos &&
            renamedText.find("{ shutdown(); /* fini */ }") !=
                std::string::npos &&
            renamedText.find("/* _fini */") != std::string::npos,
        "A rename of a renamed function does not show where C spells it");
  bool definitionTarget = false;
  for (const auto &name : renamedView.at("code_names"))
    definitionTarget |= name.at("original") == "fini" &&
                        name.value("address", std::string()) == "0x10ab";
  check(definitionTarget, "A renamed definition has no edit target");

  std::cout << "Precise variable/image targets, literal isolation, UTF-8 spans "
               "and stale-edit checks passed\n";
}
