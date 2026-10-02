//===- MobileAndroid.cpp - Native Android staging and source recovery
//------===//
#include "MobileCommon.h"
#include "MobileDalvik.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace neverd::mobile {
bool androidDexName(std::string_view name) {
  if (name == "classes.dex")
    return true;
  if (!name.starts_with("classes") || !name.ends_with(".dex"))
    return false;
  auto number = name.substr(7, name.size() - 11);
  if (number.empty() || number.front() == '0')
    return false;
  if (number.size() == 1 && number.front() < '2')
    return false;
  return std::all_of(number.begin(), number.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}
namespace {
using llvm::json::Array;
using llvm::json::Object;
using llvm::json::Value;
struct WorkDirectory {
  fs::path path;
  bool active = true;
  explicit WorkDirectory(const fs::path &staging)
      : path(staging / ".android-work") {
    if (!fs::create_directory(path))
      throw Error("Android workspace must be a new exclusive directory");
  }
  void clear() {
    fs::remove_all(path);
    active = false;
  }
  ~WorkDirectory() {
    if (active) {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  }
};
Array strings(const std::vector<std::string> &values) {
  Array result;
  for (auto &value : values)
    result.push_back(value);
  return result;
}
void copyInput(const fs::path &source, const fs::path &destination,
               const Limits &limits) {
  // readFile rejects links and nonregular input; writeFile creates exclusively.
  auto bytes = readFile(source, limits.max_bytes);
  writeFile(destination, bytes);
}
void checkDex(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  std::array<char, 8> magic{};
  stream.read(magic.data(), magic.size());
  if (stream.gcount() != 8 || std::string_view(magic.data(), 4) != "dex\n" ||
      magic[7] || magic[4] < '0' || magic[4] > '9' || magic[5] < '0' ||
      magic[5] > '9' || magic[6] < '0' || magic[6] > '9')
    throw Error("Invalid DEX header: " + pathText(path.filename()));
}
struct Inputs {
  std::string kind;
  fs::path code;
  std::vector<std::string> names;
};
Inputs stageInputs(const fs::path &source, const fs::path &work,
                   const Limits &limits) {
  Inputs result;
  result.code = work / "code";
  if (!fs::create_directory(result.code))
    throw Error("Android code staging directory already exists");
  if (fs::is_directory(source)) {
    result.kind = "smali-directory";
    auto tree = work / "tree";
    copyTree(source, tree, limits);
    std::vector<fs::path> paths;
    for (const auto &entry : fs::recursive_directory_iterator(tree))
      if (entry.is_regular_file() &&
          lowerASCII(pathText(entry.path().extension())) == ".smali")
        paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end(),
              [](const fs::path &a, const fs::path &b) {
                return pathText(a) < pathText(b);
              });
    for (auto &path : paths) {
      result.names.push_back(pathText(path.lexically_relative(tree)));
      auto ordinal = std::to_string(result.names.size());
      if (ordinal.size() < 6)
        ordinal.insert(0, 6 - ordinal.size(), '0');
      fs::rename(path, result.code / (ordinal + ".smali"));
    }
    if (result.names.empty())
      throw Error("Smali directory contains no .smali files");
  } else {
    result.kind = lowerASCII(pathText(source.extension()));
    if (!result.kind.empty() && result.kind.front() == '.')
      result.kind.erase(0, 1);
    if (result.kind != "apk" && result.kind != "dex" && result.kind != "smali")
      throw Error(
          "Android input must be an APK, DEX, smali file, or smali directory");
    if (result.kind == "apk") {
      auto archive = work / "input.apk", tree = work / "archive";
      copyInput(source, archive, limits);
      auto extracted =
          extractZip(archive, tree, limits, [](const fs::path &Path) {
            return Path.parent_path().empty() &&
                   androidDexName(pathText(Path.filename()));
          });
      std::vector<fs::path> paths;
      for (auto &path : extracted)
        if (path.parent_path() == tree &&
            androidDexName(pathText(path.filename())))
          paths.push_back(path);
      std::sort(paths.begin(), paths.end(),
                [](const fs::path &a, const fs::path &b) {
                  return pathText(a) < pathText(b);
                });
      if (paths.empty())
        throw Error(
            "APK contains no root classes.dex or classesN.dex bytecode");
      for (auto &path : paths) {
        checkDex(path);
        result.names.push_back(pathText(path.filename()));
        fs::rename(path, result.code / path.filename());
      }
    } else {
      auto path = result.code / ("input." + result.kind);
      copyInput(source, path, limits);
      if (result.kind == "dex")
        checkDex(path);
      result.names.push_back(pathText(source.filename()));
    }
  }
  return result;
}
Object builtin(const Options &options, const fs::path &staging,
               Budget &budget) {
  const auto &limits = options.limits;
  WorkDirectory work(staging);
  auto inputs = stageInputs(fs::absolute(options.input), work.path, limits);
  std::vector<fs::path> paths;
  for (const auto &entry : fs::directory_iterator(inputs.code))
    if (entry.is_regular_file())
      paths.push_back(entry.path());
  std::sort(paths.begin(), paths.end(),
            [](const fs::path &a, const fs::path &b) {
              return pathText(a) < pathText(b);
            });
  if (paths.size() != inputs.names.size())
    throw Error("Android staged input inventory is inconsistent");
  bool dex = inputs.kind == "apk" || inputs.kind == "dex";
  std::vector<dalvik::Class> classes;
  for (size_t i = 0; i < paths.size(); ++i) {
    auto content = readFile(paths[i], limits.max_bytes);
    // Each reader charges byte scanning and structural work to this budget.
    budget.check();
    if (dex) {
      auto parsed = dalvik::parseDex(content, inputs.names[i], budget);
      classes.insert(classes.end(), std::make_move_iterator(parsed.begin()),
                     std::make_move_iterator(parsed.end()));
    } else {
      if (!llvm::json::isUTF8(content))
        throw Error("smali input is not valid UTF-8");
      classes.push_back(dalvik::parseSmali(content, inputs.names[i], budget));
    }
  }
  Value coverage(dalvik::recoverJava(
      dalvik::linkClasses(std::move(classes), budget), budget));
  auto *object = coverage.getAsObject();
  auto *source_units = object ? object->getArray("source_units") : nullptr;
  if (!source_units)
    throw Error("Android native emitter produced no source inventory");
  Array units = std::move(*source_units);
  object->erase("source_units");
  work.clear();
  auto metadata_path = pathFromUTF8("metadata/android-methods.json");
  auto metadata = jsonText(coverage);
  budget.output(metadata.size());
  uint64_t total_bytes = metadata.size();
  std::map<std::string, std::pair<std::string, bool>> entries;
  auto reserve = [&](const fs::path &path, bool directory) {
    auto name = pathText(path), key = portableCaseKey(name);
    auto [found, inserted] = entries.emplace(key, std::pair{name, directory});
    if (!inserted &&
        (found->second != std::pair{name, directory} || !directory))
      throw Error(
          "Android Java output has conflicting class or directory paths");
    if (entries.size() > limits.max_files)
      throw Error(
          "Android output exceeds the file-count limit including directories");
  };
  reserve(metadata_path.parent_path(), true);
  reserve(metadata_path, false);
  std::vector<std::pair<fs::path, std::string>> pending;
  for (auto &unit_value : units) {
    auto *unit = unit_value.getAsObject();
    if (!unit)
      throw Error("Android native emitter has an invalid source unit");
    auto name = unit->getString("path"), source = unit->getString("source");
    if (!name || !source || source->empty() || !llvm::json::isUTF8(*source))
      throw Error("Android native emitter has an invalid Java source");
    auto relative =
        fs::path("sources") /
        relativeMember(std::string_view(name->data(), name->size()));
    for (auto parent = relative.parent_path(); !parent.empty() && parent != ".";
         parent = parent.parent_path())
      reserve(parent, true);
    reserve(relative, false);
    budget.tick(source->size());
    if (source->size() >
        limits.max_bytes - std::min(total_bytes, limits.max_bytes))
      throw Error("Android source and metadata output exceed the byte limit");
    total_bytes += source->size();
    pending.emplace_back(relative, source->str());
  }
  if (total_bytes > limits.max_bytes)
    throw Error("Android metadata output exceeds the byte limit");
  // The complete artifact set is checked before publishing the first Java file.
  std::vector<std::string> sources;
  for (auto &[relative, source] : pending) {
    writeFile(staging / relative, source);
    sources.push_back(pathText(relative));
  }
  writeFile(staging / metadata_path, metadata);
  validateTree(staging, limits);
  return Object{
      {"status", "success"},
      {"platform", "android"},
      {"input_kind", inputs.kind},
      {"backend",
       Object{{"name", "neverd"}, {"version", "1"}, {"execution", "builtin"}}},
      {"input_code_files", strings(inputs.names)},
      {"dex_count", dex ? uint64_t(inputs.names.size()) : 0},
      {"smali_count", dex ? 0 : uint64_t(inputs.names.size())},
      {"java_source_count", uint64_t(sources.size())},
      {"java_sources", strings(sources)},
      {"logs", Array{}},
      {"android_method_recovery", std::move(coverage)},
      {"limitations",
       Array{"Java is reconstructed from bytecode; compilation-lost source "
             "text and identifiers cannot be restored.",
             "Unsupported instructions, declarations and unproven register "
             "flows reject the input instead of publishing missing bodies.",
             "Generated method control flow can use a Java dispatch loop; it "
             "does not execute the input DEX or call an external decompiler.",
             "Native and abstract declarations remain declaration-only and are "
             "counted separately from recovered bodies.",
             "Android resources, manifests and native libraries are outside "
             "this Java recovery workflow.",
             "Successful recovery does not certify behavior for arbitrary "
             "applications."}}};
}
} // namespace
Object recoverAndroid(const Options &options, const fs::path &staging,
                      Budget &budget) {
  if (!fs::is_directory(fs::symlink_status(staging)))
    throw Error("Android output staging must be a directory");
  return builtin(options, staging, budget);
}
} // namespace neverd::mobile
