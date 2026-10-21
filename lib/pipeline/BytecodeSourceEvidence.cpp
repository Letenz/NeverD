//===- BytecodeSourceEvidence.cpp - Validate observation source bindings -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind reports to current input and exact calls before source projection.

#include "BytecodeSourceEvidence.h"

#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/pipeline/Limits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/SHA256.h"

#include <map>
#include <set>

namespace neverd {
namespace {
llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "source observations: " + Message);
}
std::optional<uint64_t> number(const llvm::json::Object &O,
                               llvm::StringRef Key) {
  auto *V = O.get(Key);
  if (!V)
    return {};
  if (auto N = V->getAsUINT64())
    return N;
  uint64_t N;
  if (auto S = V->getAsString(); S && !S->getAsInteger(0, N))
    return N;
  return {};
}
std::string quoteText(llvm::StringRef S) {
  return llvm::formatv("{0}", llvm::json::Value(S)).str();
}
std::string argument(const llvm::json::Value &V) {
  if (const auto *O = V.getAsObject()) {
    if (auto S = O->getString("string"))
      return quoteText(*S);
    if (auto S = O->getString("value"))
      return S->str();
    if (auto S = O->getString("initial_string_reference"))
      return "initial-reference(" + quoteText(*S) + ")";
  }
  return "?";
}
} // namespace

llvm::Expected<std::vector<CSourceEvidence>> bindBytecodeSourceEvidence(
    const llvm::json::Value &Report, llvm::ArrayRef<uint8_t> Code,
    llvm::StringRef Profile, llvm::ArrayRef<MedFunc> Functions,
    const std::map<va_t,
                   std::vector<analysis::BytecodeStateFunction::StoreOrigin>>
        &Stores) {
  const auto *Root = Report.getAsObject();
  if (!Root || Root->getInteger("schemaVersion") != 1 ||
      !Root->getArray("functions"))
    return invalid("expected version 1 report with functions");
  auto SHA = llvm::toHex(llvm::SHA256::hash(Code), true);
  auto ProfileSHA = llvm::toHex(
      llvm::SHA256::hash(llvm::arrayRefFromStringRef(Profile)), true);
  if (Root->getString("input_sha256") != SHA ||
      Root->getString("profile_sha256") != ProfileSHA)
    return invalid("input/profile fingerprint mismatch; rerun observation");
  using Key = std::pair<va_t, va_t>;
  std::map<Key, std::vector<const MedOp *>> Calls;
  std::map<std::tuple<va_t, va_t, unsigned>, int> StoreSequences;
  for (const auto &[Entry, Origins] : Stores)
    for (const auto &Origin : Origins)
      StoreSequences.emplace(
          std::make_tuple(Entry, Origin.Address, Origin.InstructionOperation),
          Origin.LoweredSequence);
  for (const auto &F : Functions)
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Opcode == NdOp::CALL)
          Calls[{F.Entry, Op.Addr}].push_back(&Op);
  std::vector<CSourceEvidence> Result;
  std::set<std::tuple<va_t, va_t, int, std::string>> Seen;
  size_t Work = ::neverd::limits::pipeline::
      kBytecodeSourceEvidenceBindBytecodeSourceEvidenceWork;
  for (const auto &Function : *Root->getArray("functions")) {
    const auto *F = Function.getAsObject();
    if (!F)
      return invalid("malformed function record");
    const auto *Events = F->getArray("calls");
    if (!Events)
      return invalid("function is missing calls array");
    for (const auto &Event : *Events) {
      if (!Work--)
        return invalid("call event budget exceeded");
      const auto *E = Event.getAsObject();
      if (!E)
        return invalid("malformed call record");
      auto Entry = number(*E, "function"), PC = number(*E, "pc");
      auto Target = number(*E, "target");
      if (!Entry || !PC || !Target)
        return invalid("missing call occurrence or target");
      auto At = Calls.find({*Entry, *PC});
      if (At == Calls.end() || At->second.size() != 1)
        return invalid("call is absent or ambiguous in current IR");
      const auto &Op = *At->second.front();
      if (!Op.SourceCallHint || Op.SourceCallHint->TargetAddress != *Target)
        return invalid("call target differs from current IR");
      auto Name = E->getString("service_name")
                      .value_or(E->getString("name").value_or("external"));
      if (Name.empty() ||
          Name.size() >
              ::neverd::limits::pipeline::
                  kBytecodeSourceEvidenceBindBytecodeSourceEvidenceNameSizeLimit)
        return invalid("invalid observation name");
      std::string Text = llvm::formatv("@0x{0:x-} ", *PC).str();
      std::string ResultName = Name.str();
      std::string Module;
      if (const auto *Identity = E->getObject("resolved_target")) {
        auto Symbol = Identity->getString("name");
        if (Symbol &&
            Symbol->size() <=
                ::neverd::limits::pipeline::
                    kBytecodeSourceEvidenceBindBytecodeSourceEvidenceSymbolSizeLimit) {
          Text += "indirect ";
          Name = *Symbol;
          ResultName = Symbol->str();
          Module = Identity->getString("module").value_or("").str();
          if (Module.size() >
              ::neverd::limits::pipeline::
                  kBytecodeSourceEvidenceBindBytecodeSourceEvidenceModuleSizeLimit)
            return invalid("module name exceeds observation limit");
        }
      }
      Text += Name.str() + "(";
      bool First = true;
      if (auto S = E->getString("string")) {
        if (S->size() >
            ::neverd::limits::pipeline::
                kBytecodeSourceEvidenceBindBytecodeSourceEvidenceSSizeLimit)
          return invalid("string exceeds observation limit");
        Text += quoteText(*S);
        ResultName += "_" + S->str();
        First = false;
      }
      if (const auto *Args = E->getArray("arguments")) {
        if (Args->size() >
            ::neverd::limits::pipeline::
                kBytecodeSourceEvidenceBindBytecodeSourceEvidenceArgsSizeLimit)
          return invalid("argument budget exceeded");
        for (const auto &Arg : *Args) {
          if (!First)
            Text += ", ";
          auto A = argument(Arg);
          if (A.size() >
              ::neverd::limits::pipeline::
                  kBytecodeSourceEvidenceBindBytecodeSourceEvidenceASizeLimit)
            return invalid("argument text exceeds observation limit");
          Text += A;
          First = false;
        }
      }
      Text += ")";
      if (!Module.empty()) {
        Text += " from module " + quoteText(Module);
        ResultName = Module + "_" + ResultName;
      }
      if (auto Offset = E->getString("result_register"))
        Text +=
            "; resolver result -> state+" + Offset->str() + " (may be null)";
      Text += "; conditional on observation inputs/contracts";
      if (E->getBoolean("path_constraints_satisfiable") == false)
        Text += "; path feasibility unproved";
      if (auto N = E->getInteger("prior_opaque_calls"); N && *N)
        Text += "; after abstracted calls";
      if (Seen.emplace(*Entry, *PC, Op.OriginSeq, Text).second)
        Result.push_back({*Entry,
                          {Op.Addr, Op.OriginSeq},
                          std::move(Text),
                          ResultName + "_status"});
    }
    if (const auto *Flows = F->getArray("symbol_flows"))
      for (const auto &Flow : *Flows) {
        if (!Work--)
          return invalid("symbol flow budget exceeded");
        const auto *E = Flow.getAsObject();
        if (!E || E->getString("operation") != "store")
          return invalid("invalid symbol flow");
        auto Entry = number(*E, "function"), PC = number(*E, "pc");
        auto Index = number(*E, "instruction_operation");
        const auto *Identity = E->getObject("identity");
        if (!Entry || !PC || !Index || *Index > UINT_MAX || !Identity)
          return invalid("missing store occurrence or identity");
        auto At = StoreSequences.find({*Entry, *PC, unsigned(*Index)});
        if (At == StoreSequences.end())
          return invalid("symbol store differs from current IR");
        auto Name = Identity->getString("name");
        if (!Name || Name->empty() ||
            Name->size() >
                ::neverd::limits::pipeline::
                    kBytecodeSourceEvidenceBindBytecodeSourceEvidenceNameSizeLimit)
          return invalid("invalid stored identity");
        std::string Text =
            llvm::formatv("@0x{0:x-} store resolver result ", *PC).str();
        Text += quoteText(*Name);
        if (auto Module = Identity->getString("module");
            Module && !Module->empty())
          Text += " in " + quoteText(*Module);
        if (auto Resolver = Identity->getString("resolver"))
          Text += " from " + Resolver->str();
        if (auto Origin = Identity->getString("pc"))
          Text += "@" + Origin->str();
        if (auto Destination = E->getString("destination"))
          Text += " -> " + Destination->str();
        Text += "; may be null; conditional on observation inputs/contracts";
        if (auto N = E->getInteger("prior_opaque_calls"); N && *N)
          Text += "; after abstracted calls";
        if (Text.size() >
            ::neverd::limits::pipeline::
                kBytecodeSourceEvidenceBindBytecodeSourceEvidenceTextSizeLimit)
          return invalid("symbol flow text exceeds limit");
        if (Seen.emplace(*Entry, *PC, At->second, Text).second)
          Result.push_back({*Entry,
                            {*PC, At->second},
                            std::move(Text),
                            Name->str() + "_value",
                            CSourceEvidence::ValueRole::StoredValue});
      }
  }
  return Result;
}
} // namespace neverd
