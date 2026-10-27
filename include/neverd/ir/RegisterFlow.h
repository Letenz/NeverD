//===- RegisterFlow.h - Physical register queries ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Physical register queries.
//===----------------------------------------------------------------------===//

#pragma once
#include "neverd/ir/low/LowIR.h"

#include <map>

namespace neverd {
struct RegisterValueAudit {
  bool Complete = false;
  std::vector<std::pair<size_t, NdVar>> Undefined;
};
struct RegisterFlowResult {
  struct Site {
    va_t Address;
    std::string Kind, Reason;
  };
  std::vector<Site> Sites;
  std::optional<uint64_t> Value;
  bool Unknown = false;
  std::string Error;
};
/// Read-only physical register query over original LowIR. Definitions and
/// values are before Instruction; uses start after it. Calls and opaque
/// operations are explicit boundaries, never inferred ABI effects.
RegisterFlowResult
queryRegisterFlow(const LowFunc &, Arch, va_t Instruction, NdVar Register,
                  const std::string &Kind,
                  const std::map<va_t, RegisterValueAudit> &);
} // namespace neverd
