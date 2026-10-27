//===- NativeImmediateIndex.h - Per-image immediate index ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Per-image immediate index.
//===----------------------------------------------------------------------===//

#pragma once
#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd::sdk {
struct Session;
llvm::json::Object searchNativeImmediates(Session &, llvm::StringRef,
                                          std::optional<va_t>, bool, size_t);
struct NativeImmediateIndex {
  struct Item {
    va_t PC;
    uint64_t Value;
    unsigned Operand, Bits;
  };
  struct Gap {
    va_t From, To;
    std::string Reason;
  };
  bool Initialized = false, Complete = false;
  size_t Segment = 0, Offset = 0;
  uint64_t Bytes = 0;
  uint64_t FunctionRevision = 0;
  std::vector<size_t> Segments;
  std::vector<va_t> Seeds;
  std::vector<Item> Items;
  std::vector<Gap> Gaps;
  std::string MatchPattern;
  size_t MatchProgress = 0;
  std::map<va_t, std::vector<Item>> Matches;
};
} // namespace neverd::sdk
