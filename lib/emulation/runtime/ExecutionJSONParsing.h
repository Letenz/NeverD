//===- ExecutionJSONParsing.h - Shared workload wire admission ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared workload wire admission.
///

#ifndef NEVERD_EMULATION_EXECUTIONJSONPARSING_H
#define NEVERD_EMULATION_EXECUTIONJSONPARSING_H
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation::execution_json {
llvm::Error invalid(const llvm::Twine &);
llvm::Expected<llvm::json::Value> parse(llvm::StringRef);
llvm::Error keys(const llvm::json::Object &, llvm::ArrayRef<llvm::StringRef>);
llvm::Expected<std::string> text(const llvm::json::Object &, llvm::StringRef,
                                 uint64_t Limit, bool Required = true,
                                 bool Empty = false);
llvm::Expected<uint64_t> number(const llvm::json::Value *, llvm::StringRef,
                                std::optional<uint64_t> Default = {});
llvm::Error positive(const llvm::json::Object &, llvm::StringRef, uint64_t &,
                     uint64_t Ceiling = UINT64_MAX);
llvm::Error boolean(const llvm::json::Object &, llvm::StringRef, bool &);
} // namespace neverd::emulation::execution_json
#endif
