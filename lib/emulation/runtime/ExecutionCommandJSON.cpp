//===- ExecutionCommandJSON.cpp - Common command envelope admission -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Common command envelope admission.
///

#include "ExecutionJSONParsing.h"

#include "neverd/emulation/ExecutionCommand.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation {
using namespace execution_json;
llvm::Expected<ExecutionCommand>
executionCommandFromJSON(llvm::StringRef JSON, uint64_t TransferLimit) {
  if (!TransferLimit || TransferLimit > execution_limit::TransferBytes)
    return invalid("invalid command transfer bound");
  auto V = parse(JSON);
  if (!V)
    return V.takeError();
  const auto *O = V->getAsObject();
  if (!O)
    return invalid("command must be an object");
  auto ID = text(*O, "id", execution_limit::NameBytes);
  if (!ID)
    return ID.takeError();
  auto Op = text(*O, "op", execution_limit::NameBytes);
  if (!Op)
    return Op.takeError();
  std::optional<ExecutionCommandKind> Kind;
#define NEVERD_EXECUTION_COMMAND(Name, Text)                                   \
  if (*Op == Text)                                                             \
    Kind = ExecutionCommandKind::Name;
#include "neverd/emulation/ExecutionCommand.def"
#undef NEVERD_EXECUTION_COMMAND
  if (!Kind)
    return invalid("unknown execution command");
  ExecutionCommand C;
  C.Kind = *Kind;
  C.ID = std::move(*ID);
  std::vector<llvm::StringRef> Allowed{"id", "op"};
  llvm::StringRef Payload;
  switch (C.Kind) {
  case ExecutionCommandKind::Declare:
    Payload = "binding";
    break;
  case ExecutionCommandKind::Supply:
    Payload = "supply";
    break;
  case ExecutionCommandKind::Invoke:
    Payload = "invocation";
    break;
  case ExecutionCommandKind::ModuleAdd:
  case ExecutionCommandKind::ModuleRevise:
    Payload = "module";
    break;
  case ExecutionCommandKind::ModuleDebug:
    Payload = "debug";
    break;
  default:
    break;
  }
  if (!Payload.empty()) {
    Allowed.push_back(Payload);
    const auto *Input = O->get(Payload);
    if (!Input || !Input->getAsObject())
      return invalid(Payload + " must be an object");
    C.Payload = llvm::formatv("{0}", *Input).str();
  }
  auto String = [&](llvm::StringRef Name, std::string &Target,
                    uint64_t Limit) -> llvm::Error {
    Allowed.push_back(Name);
    auto S = text(*O, Name, Limit);
    if (!S)
      return S.takeError();
    Target = std::move(*S);
    return llvm::Error::success();
  };
  if (C.Kind == ExecutionCommandKind::ModuleDebug)
    if (auto E = String("module_id", C.ModuleID, execution_limit::NameBytes))
      return std::move(E);
  if (C.Kind == ExecutionCommandKind::Lookup) {
    if (auto E =
            String("instance_id", C.InstanceID, execution_limit::NameBytes))
      return std::move(E);
    if (auto E = String("symbol", C.Symbol, 4096))
      return std::move(E);
  }
  if (C.Kind == ExecutionCommandKind::Read ||
      C.Kind == ExecutionCommandKind::Write) {
    Allowed.push_back("address");
    auto A = number(O->get("address"), "address");
    if (!A)
      return A.takeError();
    C.Address = *A;
  }
  if (C.Kind == ExecutionCommandKind::Read) {
    Allowed.push_back("size");
    auto N = number(O->get("size"), "size");
    if (!N)
      return N.takeError();
    if (*N > TransferLimit)
      return invalid("command read exceeds transfer bound");
    C.Size = *N;
  }
  if (C.Kind == ExecutionCommandKind::Write) {
    Allowed.push_back("bytes_hex");
    auto H = O->getString("bytes_hex");
    if (!H || H->size() % 2 || H->size() / 2 > TransferLimit ||
        !llvm::all_of(*H, llvm::isHexDigit))
      return invalid("command bytes_hex exceeds transfer bound or is invalid");
    auto Bytes = llvm::fromHex(*H);
    C.Bytes.assign(Bytes.begin(), Bytes.end());
    C.Size = C.Bytes.size();
  }
  if (C.Size && C.Size - 1 > UINT64_MAX - C.Address)
    return invalid("command memory range wraps");
  if (auto E = keys(*O, Allowed))
    return std::move(E);
  return C;
}
} // namespace neverd::emulation
