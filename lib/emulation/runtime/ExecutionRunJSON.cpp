//===- ExecutionRunJSON.cpp - Explicit workload wire contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit workload wire contracts.
///

#include "neverd/emulation/ExecutionRunJSON.h"

#include "ExecutionJSONParsing.h"

#include "neverd/emulation/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <set>

namespace neverd::emulation {
namespace execution_json {
namespace j = llvm::json;
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "execution JSON: " + Text);
}
// LLVM's parser intentionally accepts duplicate keys and substitutes U+FFFD
// for unpaired UTF-16 escapes. Neither is an unambiguous public identity. Run
// this check only after syntax validation; string token boundaries are known.
llvm::Error unambiguous(llvm::StringRef Text) {
  struct Container {
    bool Object, Key;
    std::set<std::string> Keys;
  };
  std::vector<Container> Stack;
  for (size_t I = 0; I < Text.size(); ++I) {
    const auto C = Text[I];
    if (C == '{' || C == '[') {
      if (Stack.size() == ::neverd::limits::emulation::
                              kExecutionRunJSONUnambiguousStackSizeLimit)
        return invalid("nesting exceeds 64 containers");
      Stack.push_back({C == '{', C == '{', {}});
    } else if (C == '}' || C == ']') {
      Stack.pop_back();
    } else if (C == ',' && Stack.back().Object) {
      Stack.back().Key = true;
    } else if (C == ':' && Stack.back().Object) {
      Stack.back().Key = false;
    } else if (C == '"') {
      const auto Start = I++;
      while (Text[I] != '"') {
        if (Text[I] == '\\') {
          ++I;
          if (Text[I] == 'u') {
            unsigned Unit = 0;
            if (Text.substr(I + 1, 4).getAsInteger(16, Unit))
              return invalid("invalid Unicode escape");
            I += 4;
            if (Unit >= 0xD800 && Unit <= 0xDBFF) {
              if (Text.substr(I + 1, 2) != "\\u")
                return invalid("unpaired Unicode surrogate");
              unsigned Low = 0;
              if (Text.substr(I + 3, 4).getAsInteger(16, Low) || Low < 0xDC00 ||
                  Low > 0xDFFF)
                return invalid("unpaired Unicode surrogate");
              I += 6;
            } else if (Unit >= 0xDC00 && Unit <= 0xDFFF) {
              return invalid("unpaired Unicode surrogate");
            }
          }
        }
        ++I;
      }
      if (!Stack.empty() && Stack.back().Object && Stack.back().Key) {
        auto Key = j::parse(Text.slice(Start, I + 1));
        if (!Key)
          return Key.takeError();
        if (!Stack.back().Keys.insert(Key->getAsString()->str()).second)
          return invalid("duplicate object field");
      }
    }
  }
  return llvm::Error::success();
}
llvm::Expected<j::Value> parse(llvm::StringRef Text) {
  if (Text.size() > execution_limit::JSONBytes || Text.contains('\0') ||
      !j::isUTF8(Text))
    return invalid("input must be bounded UTF-8 without NUL");
  auto Value = j::parse(Text);
  if (!Value)
    return Value.takeError();
  if (auto E = unambiguous(Text))
    return std::move(E);
  return std::move(*Value);
}
llvm::Error keys(const j::Object &O, llvm::ArrayRef<llvm::StringRef> Allowed) {
  for (const auto &[K, V] : O)
    if (!llvm::is_contained(Allowed, llvm::StringRef(K)))
      return invalid("unknown field: " + K.str());
  return llvm::Error::success();
}
llvm::Expected<std::string> text(const j::Object &O, llvm::StringRef Key,
                                 uint64_t Limit, bool Required, bool Empty) {
  const auto *V = O.get(Key);
  if (!V && !Required)
    return std::string();
  auto S = V ? V->getAsString() : std::nullopt;
  if (!S || (!Empty && S->empty()) || S->size() > Limit || S->contains('\0') ||
      !j::isUTF8(*S))
    return invalid(Key + " must be a bounded UTF-8 string");
  return S->str();
}
llvm::Expected<uint64_t> number(const j::Value *V, llvm::StringRef Key,
                                std::optional<uint64_t> Default) {
  if (!V)
    return Default ? llvm::Expected<uint64_t>(*Default)
                   : llvm::Expected<uint64_t>(invalid("missing " + Key));
  if (auto N = V->getAsUINT64())
    return *N;
  if (auto S = V->getAsString()) {
    unsigned Radix = 10;
    if (S->consume_front("0x") || S->consume_front("0X"))
      Radix = 16;
    uint64_t N;
    if (!S->empty() &&
        llvm::all_of(*S,
                     [=](char C) {
                       return (C >= '0' && C <= '9') ||
                              (Radix == 16 && ((C >= 'a' && C <= 'f') ||
                                               (C >= 'A' && C <= 'F')));
                     }) &&
        !S->getAsInteger(Radix, N))
      return N;
  }
  return invalid(Key + " must be an unsigned integer or decimal/hex string");
}
llvm::Error positive(const j::Object &O, llvm::StringRef Key, uint64_t &Value,
                     uint64_t Ceiling) {
  auto N = number(O.get(Key), Key, Value);
  if (!N)
    return N.takeError();
  if (!*N || *N > Ceiling)
    return invalid(Key + " exceeds its positive bound");
  Value = *N;
  return llvm::Error::success();
}
llvm::Error limits(const j::Value &V, ExecutionLimits &L) {
  const auto *O = V.getAsObject();
  if (!O)
    return invalid("limits must be an object");
  if (auto E = keys(*O, {"instructions", "events", "timeout_microseconds"}))
    return E;
  if (auto E = positive(*O, "instructions", L.Instructions))
    return E;
  if (auto E = positive(*O, "events", L.Events))
    return E;
  return positive(*O, "timeout_microseconds", L.TimeoutMicroseconds);
}
llvm::Error boolean(const j::Object &O, llvm::StringRef Key, bool &Value) {
  if (const auto *V = O.get(Key)) {
    auto B = V->getAsBoolean();
    if (!B)
      return invalid(Key + " must be boolean");
    Value = *B;
  }
  return llvm::Error::success();
}
} // namespace execution_json
namespace {
using namespace execution_json;
llvm::Expected<std::vector<ExecutionBindingInput>> bindings(const j::Value &V) {
  const auto *A = V.getAsArray();
  if (!A || A->size() > execution_limit::Instances)
    return invalid("bindings must be a bounded array");
  std::vector<ExecutionBindingInput> Result;
  for (const auto &Row : *A) {
    const auto *O = Row.getAsObject();
    if (!O)
      return invalid("binding must be an object");
    if (auto E = keys(*O, {"id", "module_id", "guest_name", "scope",
                           "address_space", "base", "size"}))
      return std::move(E);
    ExecutionBindingInput B;
    for (auto [Key, Field] : {std::pair{"id", &B.ID},
                              {"module_id", &B.ModuleID},
                              {"guest_name", &B.GuestName},
                              {"scope", &B.Scope},
                              {"address_space", &B.AddressSpace}}) {
      const bool Optional = llvm::StringRef(Key) == "scope" ||
                            llvm::StringRef(Key) == "address_space";
      auto S = text(*O, Key,
                    llvm::StringRef(Key) == "guest_name"
                        ? execution_limit::GuestNameBytes
                        : execution_limit::NameBytes,
                    !Optional, Optional);
      if (!S)
        return S.takeError();
      *Field = std::move(*S);
    }
    for (auto [Key, Field] : {std::pair{"base", &B.Base}, {"size", &B.Size}})
      if (const auto *Value = O->get(Key)) {
        auto N = number(Value, Key);
        if (!N)
          return N.takeError();
        *Field = *N;
      }
    // Extent alignment, overlap, resource existence and mapping policy belong
    // to ModuleBindings, not the wire decoder or the frontend.
    Result.push_back(std::move(B));
  }
  return Result;
}
std::string hex(uint64_t N) { return "0x" + llvm::utohexstr(N); }
llvm::Error reportText(llvm::StringRef Text) {
  if (Text.size() > execution_limit::JSONBytes || Text.contains('\0') ||
      !j::isUTF8(Text))
    return invalid("report contains invalid or excessive text");
  return llvm::Error::success();
}
llvm::Expected<std::string> identify(const std::shared_ptr<AddressSpace> &Space,
                                     const ExecutionSpaceIdentifier &ID) {
  if (!Space || !ID)
    return invalid("report requires an environment address-space identity");
  auto Name = ID(Space);
  if (!Name)
    return Name.takeError();
  if (Name->empty() || Name->size() > execution_limit::NameBytes ||
      llvm::StringRef(*Name).contains('\0') || !j::isUTF8(*Name))
    return invalid("invalid environment address-space identity");
  return std::move(*Name);
}
j::Object token(ModuleRequestToken T) {
  return j::Object{{"owner", hex(T.Owner)},
                   {"version", hex(T.Version)},
                   {"sequence", hex(T.Sequence)}};
}
j::Object fault(const BackendFault &F) {
  j::Object O{{"kind", backendFaultKindName(F.Kind)}, {"pc", hex(F.PC)}};
  if (F.Address)
    O["address"] = hex(*F.Address);
  if (F.Size)
    O["size"] = hex(*F.Size);
  if (F.Access)
    O["access"] = backendAccessKindName(*F.Access);
  if (F.Interrupt)
    O["interrupt"] = hex(*F.Interrupt);
  if (F.ErrorCode)
    O["error_code"] = hex(*F.ErrorCode);
  if (F.Cause)
    O["cause"] = backendFaultCauseName(*F.Cause);
  return O;
}
j::Object resource(const ResourcePause &P) {
  return j::Object{{"fault", fault(P.Fault)},
                   {"resume_pc", hex(P.ResumePC)},
                   {"instruction_pc", hex(P.InstructionPC)},
                   {"restart", resourceRestartKindName(P.Restart)},
                   {"memory_effects_may_exist", P.MemoryEffectsMayExist},
                   {"required_permissions", hex(P.RequiredPermissions)}};
}
llvm::Expected<j::Object> request(const ModuleRequest &R,
                                  const ExecutionSpaceIdentifier &ID) {
  for (const auto &Text :
       {R.InstanceID.value_or(""), R.ModuleID.value_or(""), R.Dependency})
    if (auto E = reportText(Text))
      return std::move(E);
  auto Space = identify(R.Space, ID);
  if (!Space)
    return Space.takeError();
  const char *Kind = nullptr, *Access = nullptr;
  switch (R.Kind) {
  case ModuleRequestKind::DeclaredModule:
    Kind = "declared_module";
    break;
  case ModuleRequestKind::MissingDependency:
    Kind = "missing_dependency";
    break;
  case ModuleRequestKind::UnmappedCandidate:
    Kind = "unmapped_candidate";
    break;
  }
  switch (R.Access) {
  case ModuleAccess::Read:
    Access = "read";
    break;
  case ModuleAccess::Write:
    Access = "write";
    break;
  case ModuleAccess::Fetch:
    Access = "fetch";
    break;
  }
  if (!Kind || !Access)
    return invalid("invalid module request kind/access");
  j::Object O{{"token", token(R.Token)},
              {"kind", Kind},
              {"address_space", std::move(*Space)},
              {"pc", hex(R.PC)},
              {"address", hex(R.Address)},
              {"size", hex(R.Size)},
              {"access", Access},
              {"required_permissions", hex(R.RequiredPermissions)}};
  if (R.InstanceID)
    O["instance_id"] = *R.InstanceID;
  if (R.ModuleID)
    O["module_id"] = *R.ModuleID;
  if (!R.Dependency.empty())
    O["dependency"] = R.Dependency;
  return O;
}
j::Object cpu(const SessionExit &S) {
  j::Object O{{"kind", sessionExitKindName(S.Kind)}};
  if (S.CPU) {
    const auto &E = *S.CPU;
    j::Object C{{"kind", executionExitKindName(E.Kind)},
                {"diagnostic", E.Diagnostic},
                {"stop_requested", E.StopRequested},
                {"deadline_reached", E.DeadlineReached}};
    if (E.Fault)
      C["fault"] = fault(*E.Fault);
    if (E.Resource)
      C["resource"] = resource(*E.Resource);
    if (E.Service)
      C["service"] =
          j::Object{{"kind", serviceRequestKindName(E.Service->Kind)},
                    {"pc", hex(E.Service->PC)},
                    {"next_pc", hex(E.Service->NextPC)},
                    {"immediate", hex(E.Service->Immediate)}};
    O["cpu"] = std::move(C);
  }
  return O;
}
llvm::Expected<j::Object> invocation(const GuestInvocationResult &R) {
  if (R.Buffers.size() > execution_limit::Buffers)
    return invalid("too many invocation output buffers");
  j::Array Buffers;
  uint64_t Total = 0;
  for (const auto &B : R.Buffers) {
    if (B.Bytes.size() > execution_limit::TransferBytes - Total)
      return invalid("invocation output exceeds transfer ceiling");
    Total += B.Bytes.size();
    Buffers.push_back(j::Object{{"address", hex(B.Address)},
                                {"bytes_hex", llvm::toHex(B.Bytes, true)}});
  }
  return j::Object{{"entry", hex(R.Entry)},
                   {"value", hex(R.Value)},
                   {"instructions", hex(R.Instructions)},
                   {"events", hex(R.Events)},
                   {"microseconds", hex(R.Microseconds)},
                   {"buffers", std::move(Buffers)}};
}
llvm::Expected<j::Object> event(const ExecutionRunEvent &E,
                                const ExecutionSpaceIdentifier &ID) {
  if (auto Error = reportText(E.Stage))
    return std::move(Error);
  if (auto Error = reportText(E.Diagnostic))
    return std::move(Error);
  if (E.CPU && E.CPU->CPU)
    if (auto Error = reportText(E.CPU->CPU->Diagnostic))
      return std::move(Error);
  j::Object O{{"state", executionRunStateName(E.State)},
              {"reason", executionRunReasonName(E.Reason)},
              {"stage", E.Stage},
              {"diagnostic", E.Diagnostic}};
  if (E.CPU)
    O["cpu"] = cpu(*E.CPU);
  if (E.Request) {
    auto R = request(*E.Request, ID);
    if (!R)
      return R.takeError();
    O["request"] = std::move(*R);
  }
  if (E.Resource)
    O["resource"] = resource(*E.Resource);
  if (E.Invocation) {
    auto R = invocation(*E.Invocation);
    if (!R)
      return R.takeError();
    O["invocation"] = std::move(*R);
  }
  return O;
}
llvm::Expected<std::string> encode(j::Object O) {
  O["schema_version"] = 1;
  auto Text = llvm::formatv("{0}", j::Value(std::move(O))).str();
  if (Text.size() > execution_limit::JSONOutputBytes)
    return invalid("encoded report exceeds output ceiling");
  return Text;
}
} // namespace

llvm::Expected<ExecutionCreationInput>
executionCreationFromJSON(llvm::StringRef JSON) {
  auto V = parse(JSON);
  if (!V)
    return V.takeError();
  const auto *O = V->getAsObject();
  if (!O)
    return invalid("execution configuration must be an object");
  if (auto E = keys(*O, {"environment", "run", "bindings"}))
    return std::move(E);
  const auto *Environment = O->getObject("environment");
  if (!Environment)
    return invalid("execution requires an explicit environment object");
  auto Kind = text(*Environment, "kind", execution_limit::NameBytes);
  if (!Kind)
    return Kind.takeError();
  ExecutionCreationInput Input;
  Input.EnvironmentKind = std::move(*Kind);
  Input.EnvironmentJSON =
      llvm::formatv("{0}", j::Value(j::Object(*Environment))).str();
  if (const auto *R = O->get("run")) {
    auto Options = executionRunOptionsFromJSON(llvm::formatv("{0}", *R).str());
    if (!Options)
      return Options.takeError();
    Input.Options = std::move(*Options);
  }
  if (const auto *B = O->get("bindings")) {
    auto Bindings = bindings(*B);
    if (!Bindings)
      return Bindings.takeError();
    Input.Bindings = std::move(*Bindings);
  }
  return Input;
}

llvm::Expected<ExecutionRunOptions>
executionRunOptionsFromJSON(llvm::StringRef JSON) {
  auto Value = parse(JSON);
  if (!Value)
    return Value.takeError();
  const auto *O = Value->getAsObject();
  if (!O)
    return invalid("run options must be an object");
  if (auto E = keys(*O, {"limits", "time_policy", "instruction_quantum",
                         "actions_per_run", "instance_limit",
                         "memory_transfer_limit", "enable_resource_pauses",
                         "pause_unknown_addresses", "pause_stages"}))
    return std::move(E);
  ExecutionRunOptions R;
  if (const auto *L = O->get("limits"))
    if (auto E = limits(*L, R.Limits))
      return std::move(E);
  if (const auto *P = O->get("time_policy")) {
    auto S = P->getAsString();
    if (S == "active_work")
      R.TimePolicy = ExecutionTimePolicy::ActiveWork;
    else if (S == "absolute_deadline")
      R.TimePolicy = ExecutionTimePolicy::AbsoluteDeadline;
    else
      return invalid("unsupported time_policy");
  }
  if (auto E = positive(*O, "instruction_quantum", R.InstructionQuantum))
    return std::move(E);
  if (auto E = positive(*O, "actions_per_run", R.ActionsPerRun,
                        execution_limit::Actions))
    return std::move(E);
  if (auto E = positive(*O, "instance_limit", R.InstanceLimit,
                        execution_limit::Instances))
    return std::move(E);
  if (auto E = positive(*O, "memory_transfer_limit", R.MemoryTransferLimit,
                        execution_limit::TransferBytes))
    return std::move(E);
  if (auto E = boolean(*O, "enable_resource_pauses", R.EnableResourcePauses))
    return std::move(E);
  if (auto E = boolean(*O, "pause_unknown_addresses", R.PauseUnknownAddresses))
    return std::move(E);
  if (const auto *P = O->get("pause_stages")) {
    const auto *A = P->getAsArray();
    if (!A || A->size() > execution_limit::Stages)
      return invalid("pause_stages must be a bounded array");
    std::set<std::string> Seen;
    for (const auto &V : *A) {
      j::Object Stage{{"stage", V}};
      auto S = text(Stage, "stage", execution_limit::NameBytes);
      if (!S)
        return S.takeError();
      if (!Seen.insert(*S).second)
        return invalid("duplicate pause stage");
      R.PauseStages.push_back(std::move(*S));
    }
  }
  return R;
}
llvm::Expected<std::vector<ExecutionBindingInput>>
executionBindingsFromJSON(llvm::StringRef JSON) {
  auto Value = parse(JSON);
  if (!Value)
    return Value.takeError();
  return bindings(*Value);
}
llvm::Expected<ExecutionSupplyInput>
executionSupplyFromJSON(llvm::StringRef JSON) {
  auto Value = parse(JSON);
  if (!Value)
    return Value.takeError();
  const auto *O = Value->getAsObject();
  if (!O)
    return invalid("supply must be an object");
  if (auto E = keys(*O, {"token", "bindings"}))
    return std::move(E);
  const auto *T = O->getObject("token");
  const auto *B = O->get("bindings");
  if (!T || !B)
    return invalid("supply requires token and bindings");
  if (auto E = keys(*T, {"owner", "version", "sequence"}))
    return std::move(E);
  ExecutionSupplyInput R;
  for (auto [Key, Field] : {std::pair{"owner", &R.Token.Owner},
                            {"version", &R.Token.Version},
                            {"sequence", &R.Token.Sequence}}) {
    auto N = number(T->get(Key), Key);
    if (!N)
      return N.takeError();
    *Field = *N;
  }
  // Revision zero is valid before the first module declaration.
  if (!R.Token.Owner || !R.Token.Sequence)
    return invalid("resource token owner/sequence must be nonzero");
  auto Inputs = bindings(*B);
  if (!Inputs)
    return Inputs.takeError();
  R.Bindings = std::move(*Inputs);
  return R;
}
llvm::Expected<std::vector<ModuleBinding>>
resolveExecutionBindings(llvm::ArrayRef<ExecutionBindingInput> Inputs,
                         const ExecutionSpaceResolver &Resolve) {
  if (Inputs.size() > execution_limit::Instances ||
      (!Inputs.empty() && !Resolve))
    return invalid("bindings require a bounded batch and space resolver");
  std::vector<ModuleBinding> Result;
  for (const auto &B : Inputs) {
    auto Space = Resolve(B.AddressSpace);
    if (!Space)
      return Space.takeError();
    if (!*Space)
      return invalid("environment returned an unknown address space");
    Result.push_back({B.ID, B.ModuleID, B.GuestName, B.Scope, std::move(*Space),
                      B.Base, B.Size});
  }
  return Result;
}
llvm::Expected<GuestInvocationRequest>
guestInvocationFromJSON(llvm::StringRef JSON, uint64_t ByteLimit) {
  if (!ByteLimit || ByteLimit > execution_limit::TransferBytes)
    return invalid("invalid invocation transfer limit");
  auto Value = parse(JSON);
  if (!Value)
    return Value.takeError();
  const auto *O = Value->getAsObject();
  if (!O)
    return invalid("invocation must be an object");
  if (auto E = keys(*O, {"instance_id", "entry", "abi", "arguments", "buffers",
                         "limits"}))
    return std::move(E);
  GuestInvocationRequest R;
  auto Instance =
      text(*O, "instance_id", execution_limit::NameBytes, false, true);
  if (!Instance)
    return Instance.takeError();
  R.InstanceID = std::move(*Instance);
  auto Entry = number(O->get("entry"), "entry");
  if (!Entry)
    return Entry.takeError();
  R.Entry = *Entry;
  auto ABI = O->getString("abi");
  if (ABI == "win64")
    R.ABI = IntegerCallingConvention::Win64;
  else if (ABI == "sysv_amd64")
    R.ABI = IntegerCallingConvention::SysVAMD64;
  else if (ABI == "aapcs64")
    R.ABI = IntegerCallingConvention::AAPCS64;
  else
    return invalid("invocation requires an explicit supported scalar ABI");
  if (const auto *L = O->get("limits"))
    if (auto E = limits(*L, R.Limits))
      return std::move(E);
  uint64_t Total = 0;
  if (const auto *V = O->get("buffers")) {
    const auto *A = V->getAsArray();
    if (!A || A->size() > execution_limit::Buffers)
      return invalid("buffers must be a bounded array");
    auto ABIInfo = IntegerABI::get(*R.ABI);
    if (!ABIInfo)
      return ABIInfo.takeError();
    const uint64_t Alignment = ABIInfo->info().StackAlignment;
    for (const auto &Row : *A) {
      const auto *B = Row.getAsObject();
      if (!B)
        return invalid("buffer must be an object");
      if (auto E = keys(*B, {"size", "input_hex", "capture"}))
        return std::move(E);
      auto Size = number(B->get("size"), "buffer size");
      if (!Size)
        return Size.takeError();
      if (*Size > ByteLimit)
        return invalid("buffer exceeds transfer bound");
      const uint64_t Aligned = (*Size + Alignment - 1) & ~(Alignment - 1);
      if (Aligned > ByteLimit - Total)
        return invalid("aligned buffers exceed aggregate transfer bound");
      Total += Aligned;
      GuestInvocationBuffer Buffer;
      Buffer.Size = *Size;
      if (auto E = boolean(*B, "capture", Buffer.Capture))
        return std::move(E);
      if (const auto *Input = B->get("input_hex")) {
        auto Hex = Input->getAsString();
        if (!Hex || Hex->size() % 2 || Hex->size() / 2 > Buffer.Size ||
            !llvm::all_of(*Hex, llvm::isHexDigit))
          return invalid("input_hex must encode bytes within buffer size");
        const auto Bytes = llvm::fromHex(*Hex);
        Buffer.Input.assign(Bytes.begin(), Bytes.end());
      }
      R.Buffers.push_back(std::move(Buffer));
    }
  }
  if (const auto *V = O->get("arguments")) {
    const auto *A = V->getAsArray();
    if (!A || A->size() > execution_limit::Arguments)
      return invalid("arguments must be a bounded array");
    for (const auto &Row : *A) {
      const auto *Arg = Row.getAsObject();
      if (!Arg)
        return invalid("argument must be an object");
      GuestInvocationArgument Argument;
      if (Arg->get("buffer")) {
        if (auto E = keys(*Arg, {"buffer", "offset"}))
          return std::move(E);
        auto Index = number(Arg->get("buffer"), "buffer index");
        if (!Index)
          return Index.takeError();
        auto Offset = number(Arg->get("offset"), "buffer offset", 0);
        if (!Offset)
          return Offset.takeError();
        if (*Index >= R.Buffers.size() || *Offset > R.Buffers[*Index].Size)
          return invalid("argument refers outside its buffer");
        Argument.Buffer = uint32_t(*Index);
        Argument.Value = *Offset;
      } else {
        if (auto E = keys(*Arg, {"value"}))
          return std::move(E);
        auto Scalar = number(Arg->get("value"), "argument value");
        if (!Scalar)
          return Scalar.takeError();
        Argument.Value = *Scalar;
      }
      R.Arguments.push_back(Argument);
    }
  }
  return R;
}
llvm::Expected<std::string>
executionRunEventJSON(const ExecutionRunEvent &E,
                      const ExecutionSpaceIdentifier &ID) {
  auto O = event(E, ID);
  if (!O)
    return O.takeError();
  return encode(std::move(*O));
}
llvm::Expected<std::string>
executionRunSnapshotJSON(const ExecutionRunSnapshot &S,
                         const ExecutionSpaceIdentifier &ID) {
  if (auto E = reportText(S.Environment))
    return std::move(E);
  auto Event = event(S.Event, ID);
  if (!Event)
    return Event.takeError();
  j::Object O{{"event", std::move(*Event)},
              {"environment", S.Environment},
              {"time_policy", executionTimePolicyName(S.TimePolicy)},
              {"instructions", hex(S.Instructions)},
              {"events", hex(S.Events)},
              {"remaining_microseconds", hex(S.RemainingMicroseconds)},
              {"module_revision", hex(S.ModuleRevision)}};
  if (S.Continuation)
    O["continuation"] = resource(*S.Continuation);
  if (S.Invocation) {
    const auto &P = *S.Invocation;
    const auto &F = P.Frame;
    O["invocation"] = j::Object{
        {"frame",
         j::Object{{"entry", hex(F.Entry)},
                   {"return_pc", hex(F.ReturnPC)},
                   {"stack_base", hex(F.StackBase)},
                   {"stack_size", hex(F.StackSize)},
                   {"stack_pointer", hex(F.StackPointer)},
                   {"return_stack_pointer", hex(F.ReturnStackPointer)}}},
        {"instructions", hex(P.Instructions)},
        {"events", hex(P.Events)},
        {"microseconds", hex(P.Microseconds)},
        {"remaining_instructions", hex(P.RemainingInstructions)},
        {"remaining_events", hex(P.RemainingEvents)},
        {"remaining_microseconds", hex(P.RemainingMicroseconds)}};
  }
  return encode(std::move(O));
}
llvm::Expected<std::string>
executionInstancesJSON(llvm::ArrayRef<ModuleInstance> Instances,
                       const ExecutionSpaceIdentifier &ID) {
  if (Instances.size() > execution_limit::Instances)
    return invalid("too many module instances to report");
  j::Array A;
  for (const auto &I : Instances) {
    const auto &B = I.Binding;
    for (const auto &Text : {B.ID, B.ModuleID, B.GuestName, B.Scope, I.Role})
      if (auto E = reportText(Text))
        return std::move(E);
    auto Space = identify(B.Space, ID);
    if (!Space)
      return Space.takeError();
    j::Object O{{"id", B.ID},
                {"module_id", B.ModuleID},
                {"guest_name", B.GuestName},
                {"scope", B.Scope},
                {"address_space", std::move(*Space)},
                {"load", modules::loadPolicyName(I.Load)},
                {"mapped", I.mapped()}};
    if (I.EnvironmentOwned) {
      O["environment_owned"] = true;
      O["role"] = I.Role;
    }
    if (B.Base)
      O["declared_base"] = hex(*B.Base);
    if (B.Size)
      O["declared_size"] = hex(*B.Size);
    if (I.mapped()) {
      O["base"] = hex(I.Base);
      O["size"] = hex(I.Size);
      O["entry"] = hex(I.Entry);
      O["image_sha256"] = I.Source->imageDigest();
      if (!I.Source->debugDigest().empty())
        O["debug_sha256"] = I.Source->debugDigest();
      // Symbol metadata neither grants memory permission nor certifies a
      // function's modeled behavior. Do not add a derived 'executable' flag.
      O["export_count"] = hex(I.Exports.size());
    }
    A.push_back(std::move(O));
  }
  return encode(j::Object{{"instances", std::move(A)}});
}
} // namespace neverd::emulation
