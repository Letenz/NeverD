//===- AndroidVersion.cpp - Explicit Android version field projection ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 scalar Build.VERSION values over a supplied property snapshot.
//===----------------------------------------------------------------------===//
#include "neverd/dalvik/Limits.h"
#include "neverd/dalvik/Runtime.h"

#include <charconv>

namespace neverd::mobile::dalvik {
namespace {
int64_t propertyInteger(std::string_view text) {
  // Android base ParseInt selects hexadecimal only from the original prefix,
  // then accepts leading C whitespace/signs but no trailing characters.
  const bool hex = text.starts_with("0x") || text.starts_with("0X");
  while (!text.empty() && std::string_view(" \t\n\r\f\v").find(text[0]) !=
                              std::string_view::npos)
    text.remove_prefix(1);
  bool negative = !text.empty() && text[0] == '-';
  if (!text.empty() && (text[0] == '+' || text[0] == '-'))
    text.remove_prefix(1);
  if (hex)
    text.remove_prefix(2);
  uint64_t magnitude = 0;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(),
                                      magnitude, hex ? 16 : 10);
  if (error != std::errc() || end != text.data() + text.size() ||
      magnitude > uint64_t(0x7fffffff) + negative)
    return 0;
  return negative ? -int64_t(magnitude) : int64_t(magnitude);
}
} // namespace

void Program::addAndroidVersion(
    const std::map<std::string, std::string> &properties) {
  Class version;
  version.name = "Landroid/os/Build$VERSION;";
  version.superclass = "Ljava/lang/Object;";
  version.access = {"public"};
  version.source_id = "android-api28-version";
  auto add = [&](const char *name, const char *key, const char *fallback) {
    auto found = properties.find(key);
    const std::string text = found == properties.end() ? "" : found->second;
    if (text.size() >=
            ::neverd::limits::dalvik::
                kAndroidVersionProgramAddAndroidVersionAddAddTextSizeLimit ||
        text.find('\0') != std::string::npos)
      throw Error("invalid Android version property value");
    Field field;
    field.reference = {version.name, name,
                       fallback ? "Ljava/lang/String;" : "I"};
    field.access = {"public", "static", "final"};
    PlatformValue value;
    if (fallback) {
      value.value = text.empty() ? std::string(fallback) : text;
      value.intern_string = text.empty();
      // Refuse unadmitted encodings rather than guessing ART replacement.
      (void)javaStringBytes(std::get<std::string>(value.value));
    } else {
      value.value = propertyInteger(text);
    }
    platform_values.emplace(field.reference, std::move(value));
    version.fields.push_back(std::move(field));
  };
  add("INCREMENTAL", "ro.build.version.incremental", "unknown");
  add("RELEASE", "ro.build.version.release", "unknown");
  add("BASE_OS", "ro.build.version.base_os", "");
  add("SECURITY_PATCH", "ro.build.version.security_patch", "");
  add("SDK", "ro.build.version.sdk", "unknown");
  add("SDK_INT", "ro.build.version.sdk", nullptr);
  add("FIRST_SDK_INT", "ro.product.first_api_level", nullptr);
  add("PREVIEW_SDK_INT", "ro.build.version.preview_sdk", nullptr);
  add("CODENAME", "ro.build.version.codename", "unknown");
  add("MIN_SUPPORTED_TARGET_SDK_INT",
      "ro.build.version.min_supported_target_sdk", nullptr);
  classes.emplace(version.name, std::move(version));
}

const Program::PlatformValue *Program::platformValue(const Field &field) const {
  if (findField(field.reference, true) != &field)
    return nullptr;
  auto found = platform_values.find(field.reference);
  return found == platform_values.end() ? nullptr : &found->second;
}
} // namespace neverd::mobile::dalvik
