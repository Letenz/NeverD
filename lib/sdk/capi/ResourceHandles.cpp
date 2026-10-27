//===- ResourceHandles.cpp - Validated, non-reused resource handles
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validated, non-reused resource handles.
///

#include "ResourceHandles.h"

#include "neverd/sdk/Limits.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace neverd::sdk::resources {
namespace {
struct Entry {
  HandleKind Kind;
  std::shared_ptr<void> Value;
};
struct Registry {
  std::mutex Mutex;
  uint64_t Next = 1;
  std::map<uint64_t, Entry> Entries;
};
Registry &registry() {
  static Registry R;
  return R;
}
llvm::Error failure(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace
llvm::Expected<uint64_t> retain(HandleKind Kind, std::shared_ptr<void> Value) {
  auto &R = registry();
  std::unique_lock Lock(R.Mutex);
  if (!Value || R.Next == UINT64_MAX)
    return failure("resource handle identities exhausted or value is empty");
  const auto ID = R.Next++;
  R.Entries.emplace(ID, Entry{Kind, std::move(Value)});
  return ID;
}
llvm::Expected<std::shared_ptr<void>> acquire(uint64_t ID, HandleKind Kind) {
  auto &R = registry();
  std::unique_lock Lock(R.Mutex);
  const auto Found = R.Entries.find(ID);
  if (Found == R.Entries.end() || Found->second.Kind != Kind)
    return failure("unknown, expired or wrong-kind resource handle");
  return Found->second.Value;
}
llvm::Error release(uint64_t ID, HandleKind Kind) {
  std::shared_ptr<void> Removed;
  auto &R = registry();
  {
    std::unique_lock Lock(R.Mutex);
    const auto Found = R.Entries.find(ID);
    if (Found == R.Entries.end() || Found->second.Kind != Kind)
      return failure("unknown, expired or wrong-kind resource handle");
    Removed = std::move(Found->second.Value);
    R.Entries.erase(Found);
  }
  return llvm::Error::success();
}
llvm::Expected<llvm::StringRef> text(const char *Value, uint64_t Limit,
                                     llvm::StringRef What, bool AllowEmpty) {
  if (!Value)
    return failure(What + " is required");
  uint64_t Length = 0;
  while (Length <= Limit && Value[Length])
    ++Length;
  if (Length > Limit || (!AllowEmpty && !Length))
    return failure(What + " is empty or exceeds its input bound");
  const llvm::StringRef Result(Value, Length);
  if (!llvm::json::isUTF8(Result))
    return failure(What + " must be UTF-8");
  return Result;
}
llvm::Expected<const char *> ownedString(llvm::StringRef Text, uint64_t Limit) {
  if (Text.size() > Limit || Text.size() == SIZE_MAX)
    return failure("resource JSON result exceeds its bound");
  if (!llvm::json::isUTF8(Text))
    return failure("resource JSON result is not UTF-8");
  auto Copy = static_cast<char *>(std::malloc(Text.size() + 1));
  if (!Copy)
    return failure("could not allocate resource JSON result");
  std::memcpy(Copy, Text.data(), Text.size());
  Copy[Text.size()] = '\0';
  return Copy;
}
void reportError(const char **Error, llvm::StringRef Text) noexcept {
  if (!Error)
    return;
  if (!llvm::json::isUTF8(Text))
    Text = "resource operation failed; diagnostic contains invalid UTF-8";
  auto Length = std::min<size_t>(
      Text.size(),
      ::neverd::sdk::kResourceHandlesReportErrorLengthMinArgument2);
  if (Length < Text.size())
    while (Length && (static_cast<unsigned char>(Text[Length]) & 0xC0) == 0x80)
      --Length;
  auto Copy = static_cast<char *>(std::malloc(Length + 1));
  if (!Copy)
    return;
  std::memcpy(Copy, Text.data(), Length);
  Copy[Length] = '\0';
  *Error = Copy;
}
} // namespace neverd::sdk::resources
