//===- BytecodeStateSourceContract.cpp - Explicit source ABI input --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject incomplete declarations; execution observations are not contracts.

#include "BytecodeStateSourceContract.h"

#include "llvm/Support/Errc.h"

#include <map>

namespace neverd {
namespace {
llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "source_contracts: " + Message);
}
bool keys(const llvm::json::Object &O,
          std::initializer_list<llvm::StringRef> Allowed) {
  for (const auto &[K, V] : O)
    if (!llvm::is_contained(Allowed, llvm::StringRef(K)))
      return false;
  return true;
}
std::optional<uint64_t> number(const llvm::json::Object &O,
                               llvm::StringRef Key) {
  auto *V = O.get(Key);
  return V ? V->getAsUINT64() : std::nullopt;
}
llvm::Expected<StateSourceSlot> slot(const llvm::json::Value &V) {
  auto *O = V.getAsObject();
  if (!O || !keys(*O, {"offset", "bytes"}))
    return invalid("invalid state slot");
  auto Offset = O->getInteger("offset"), Bytes = O->getInteger("bytes");
  if (!Offset || !Bytes || *Offset < 0 || *Offset > 65536 ||
      (*Bytes != 1 && *Bytes != 2 && *Bytes != 4 && *Bytes != 8))
    return invalid("invalid slot offset or width");
  return StateSourceSlot{unsigned(*Offset), unsigned(*Bytes)};
}
llvm::Expected<StateSourceType> type(const llvm::json::Value &V) {
  static const std::map<llvm::StringRef, StateSourceType> Types = {
      {"void", StateSourceType::Void}, {"u32", StateSourceType::U32},
      {"u64", StateSourceType::U64},   {"i32", StateSourceType::I32},
      {"i64", StateSourceType::I64},   {"pointer", StateSourceType::Pointer}};
  auto Name = V.getAsString();
  if (!Name || !Types.count(*Name))
    return invalid("unsupported source type");
  return Types.at(*Name);
}
} // namespace
llvm::Expected<StateSourceContract>
readBytecodeStateSourceContract(const llvm::json::Value &V,
                                unsigned StateBytes) {
  auto *Root = V.getAsObject();
  if (!Root ||
      !keys(*Root, {"schemaVersion", "state_bytes", "isolated_bank",
                    "zero_status_gateways", "gateways"}) ||
      Root->getInteger("schemaVersion") != 1 ||
      Root->getInteger("state_bytes") != StateBytes ||
      Root->getBoolean("isolated_bank") != true ||
      Root->getBoolean("zero_status_gateways") != true)
    return invalid("explicit isolation/status assertions and matching "
                   "state_bytes are required");
  auto *Gateways = Root->getArray("gateways");
  if (!Gateways || Gateways->size() > 256)
    return invalid("invalid gateways array");
  StateSourceContract Result;
  Result.StateBytes = StateBytes;
  Result.IsolatedBank = true;
  Result.ZeroStatusGateways = true;
  for (const auto &Item : *Gateways) {
    auto *O = Item.getAsObject();
    if (!O ||
        !keys(*O, {"name", "selector", "argument_count", "arguments",
                   "result_offset", "result_bytes", "writes", "imports"}) ||
        !O->getString("name") || !O->getArray("arguments") ||
        !O->getArray("writes") || !O->getArray("imports") ||
        !O->get("result_offset") || !O->get("result_bytes"))
      return invalid("incomplete gateway contract");
    StateSourceGateway G;
    G.Name = O->getString("name")->str();
    for (auto [Name, Destination] :
         {std::pair{"selector", &G.Selector},
          std::pair{"argument_count", &G.ArgumentCount}})
      if (auto *Value = O->get(Name)) {
        auto S = slot(*Value);
        if (!S)
          return S.takeError();
        *Destination = *S;
      }
    for (auto [Name, Destination] :
         {std::pair{"result_offset", &G.ResultOffset},
          std::pair{"result_bytes", &G.ResultBytes}}) {
      auto S = slot(*O->get(Name));
      if (!S)
        return S.takeError();
      *Destination = *S;
    }
    if (O->getArray("arguments")->size() > 16 ||
        O->getArray("writes")->size() > 128 ||
        O->getArray("imports")->size() > 4096)
      return invalid("gateway description exceeds work limit");
    for (const auto &A : *O->getArray("arguments")) {
      auto S = slot(A);
      if (!S)
        return S.takeError();
      G.Arguments.push_back(*S);
    }
    for (const auto &W : *O->getArray("writes")) {
      auto *Write = W.getAsObject();
      if (!Write || !keys(*Write, {"slot", "value"}) || !Write->get("slot") ||
          !number(*Write, "value"))
        return invalid("invalid fixed bank write");
      auto S = slot(*Write->get("slot"));
      if (!S)
        return S.takeError();
      G.Writes.push_back({*S, *number(*Write, "value")});
    }
    for (const auto &I : *O->getArray("imports")) {
      auto *Import = I.getAsObject();
      if (!Import ||
          !keys(*Import, {"selector", "name", "parameters", "result"}) ||
          !number(*Import, "selector") || !Import->getString("name") ||
          !Import->getArray("parameters") || !Import->get("result"))
        return invalid("incomplete imported signature");
      StateSourceImport Native;
      Native.Selector = *number(*Import, "selector");
      Native.Name = Import->getString("name")->str();
      auto T = type(*Import->get("result"));
      if (!T)
        return T.takeError();
      Native.Result = *T;
      if (Import->getArray("parameters")->size() > 16)
        return invalid("too many parameters");
      for (const auto &Parameter : *Import->getArray("parameters")) {
        auto P = type(Parameter);
        if (!P)
          return P.takeError();
        Native.Parameters.push_back(*P);
      }
      G.Imports.push_back(std::move(Native));
    }
    Result.Gateways.push_back(std::move(G));
  }
  return Result;
}
} // namespace neverd
