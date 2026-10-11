//===- BytecodeObserve.cpp - Bounded LowIR observations ------------------===//
//
// NeverD Decompiler
//
// This analysis driver supplies traversal and explicit input contracts. The
// shared BytecodeDecoder, SymExec and bitvector solver own all instruction,
// memory, arithmetic and feasibility semantics. No dialect is built in.
//===----------------------------------------------------------------------===//
/// \file
/// Traverse bytecode with shared symbolic semantics and emit bounded evidence.

#include "neverd/analysis/BytecodeDecoder.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;
namespace J = llvm::json;

namespace {
std::string readFile(llvm::StringRef Path) {
  auto B = llvm::MemoryBuffer::getFile(Path);
  if (!B)
    throw std::runtime_error("cannot read " + Path.str());
  if ((*B)->getBufferSize() > 64 * 1024 * 1024)
    throw std::runtime_error("input exceeds 64 MiB");
  return (*B)->getBuffer().str();
}
J::Value parse(llvm::StringRef S) {
  auto V = J::parse(S);
  if (!V)
    throw std::runtime_error(llvm::toString(V.takeError()));
  return std::move(*V);
}
const J::Object &object(const J::Value &V) {
  if (auto *O = V.getAsObject())
    return *O;
  throw std::runtime_error("expected JSON object");
}
std::string textField(const J::Object &O, llvm::StringRef K) {
  if (auto V = O.getString(K))
    return V->str();
  throw std::runtime_error("missing or invalid " + K.str());
}
uint64_t number(const J::Value &V) {
  if (auto N = V.getAsUINT64())
    return *N;
  uint64_t N;
  if (auto S = V.getAsString(); S && !S->getAsInteger(0, N))
    return N;
  throw std::runtime_error("expected unsigned integer");
}
uint64_t field(const J::Object &O, llvm::StringRef K) {
  auto *V = O.get(K);
  if (!V)
    throw std::runtime_error("missing " + K.str());
  return number(*V);
}
void arrayFields(const J::Object &O,
                 std::initializer_list<llvm::StringRef> Keys) {
  for (auto K : Keys)
    if (O.get(K) && !O.getArray(K))
      throw std::runtime_error("expected array: " + K.str());
}
void validateCall(const J::Object &O, unsigned Depth = 0) {
  if (Depth > 1)
    throw std::runtime_error("nested selector cases are unsupported");
  arrayFields(O, {"argument_registers", "indirect_argument_registers",
                  "preserve_registers", "register_writes"});
  for (auto K : {"string", "selector_names", "selector_cases"})
    if (O.get(K) && !O.getObject(K))
      throw std::runtime_error(std::string("expected object: ") + K);
  if (O.get("continuation")) {
    auto K = textField(O, "continuation");
    if (K != "stop" && K != "havoc" && K != "register_effects" &&
        K != "memory_copy" && K != "memory_fill" && K != "memory_havoc")
      throw std::runtime_error("unsupported call continuation");
    if (K == "register_effects" && O.getString("memory_effect") != "none")
      throw std::runtime_error(
          "register_effects requires explicit memory_effect=none");
    if (K == "memory_copy" || K == "memory_fill" || K == "memory_havoc") {
      field(O, "destination_register");
      field(O, "size_register");
      if (K != "memory_havoc")
        field(O, "source_register");
      for (auto Flag : {"require_disjoint", "return_destination"})
        if (O.get(Flag) && !O.getBoolean(Flag))
          throw std::runtime_error(std::string("expected boolean: ") + Flag);
    }
  }
  if (auto *Cases = O.getObject("selector_cases"))
    for (const auto &[Key, Case] : *Cases) {
      uint64_t Selector;
      if (!llvm::StringRef(Key).starts_with("0x") ||
          llvm::StringRef(Key).getAsInteger(0, Selector))
        throw std::runtime_error("selector case keys must be hexadecimal");
      validateCall(object(Case), Depth + 1);
    }
}
std::string hex(uint64_t N) { return llvm::formatv("0x{0:x-}", N).str(); }

struct Frame {
  uint64_t Function, ReturnPC;
};
struct Path {
  SymState State;
  uint64_t PC, Function;
  SymRef Predicate;
  std::vector<Frame> Frames;
  std::map<uint64_t, unsigned> Visits;
  unsigned Steps = 0, Opaque = 0;
  bool Feasible = true;
  Path(SymContext &C, uint64_t Entry, llvm::endianness Order)
      : State(C, Order), PC(Entry), Function(Entry), Predicate(C.mkTrue()) {}
};

class Observer {
  std::string Code;
  std::unique_ptr<BytecodeDecoder> Decoder;
  J::Value Configuration;
  std::map<uint64_t, std::pair<uint64_t, std::string>> Functions;
  std::map<uint64_t, BytecodeInstruction> Instructions;
  std::map<uint64_t, const J::Object *> Calls;
  std::map<uint64_t, std::string> InitialMemory;
  J::Array Results;
  uint64_t MaxSteps = 50000, MaxPaths = 32, MaxVisits = 64;
  uint64_t MaxMilliseconds = 10000;
  unsigned Queries = 0;
  const J::Object &config() const { return *Configuration.getAsObject(); }
  uint64_t registerWidth(const J::Object &O, llvm::StringRef Key) const {
    auto *V = O.get(Key);
    auto Width = V ? number(*V) : 8;
    if (!Width || Width > 8)
      throw std::runtime_error("invalid register width");
    return Width;
  }
  void checkRange(uint64_t Offset, uint64_t Bytes) const {
    auto Size = Decoder->profile().RegisterBytes;
    if (!Bytes || Bytes > 8 || Offset > Size || Bytes > Size - Offset)
      throw std::runtime_error("invalid register range");
  }
  SymRef readRegister(Path &P, uint64_t Offset, uint64_t Bytes = 8) {
    checkRange(Offset, Bytes);
    return P.State.read(SymSpace::Register, Offset, Bytes);
  }

  std::optional<uint64_t> value(SymContext &C, SymRef R) {
    if (auto N = C.asConst(R); N && N->getBitWidth() <= 64)
      return N->getZExtValue();
    return {};
  }
  J::Value describe(SymContext &C, SymRef R) {
    if (auto N = value(C, R))
      return hex(*N);
    // Truncating after rendering is too late for a compact DAG whose printed
    // tree is exponential. Keep identity and width without expanding it.
    if (C.readabilityCost(R) > 80)
      return J::Object{{"expression_ref", R.index()},
                       {"bits", C.width(R)},
                       {"rendering_omitted", "expression budget"}};
    auto S = C.toString(R);
    if (S.size() > 300)
      S = S.substr(0, 300) + "...";
    return J::Object{{"expression", S}};
  }
  std::optional<std::string> string(Path &P, uint64_t Address,
                                    uint8_t Xor = 0) {
    auto &C = P.State.context();
    std::string S;
    for (unsigned I = 0; I < 512; ++I) {
      if (Address > UINT64_MAX - I)
        return {};
      auto N = value(C, P.State.load(C.mkConst(64, Address + I), 1));
      if (!N)
        return {};
      unsigned B = *N ^ Xor;
      if (!B)
        return S;
      if ((B < 32 && B != '\n' && B != '\r' && B != '\t') || B >= 127)
        return {};
      S.push_back(B);
    }
    return {};
  }
  std::optional<std::string> inputString(uint64_t Address) const {
    auto I = InitialMemory.upper_bound(Address);
    if (I == InitialMemory.begin())
      return {};
    --I;
    auto Offset = Address - I->first;
    if (Offset >= I->second.size())
      return {};
    std::string Text;
    for (; Offset < I->second.size() && Text.size() < 512; ++Offset) {
      unsigned char B = I->second[Offset];
      if (!B)
        return Text.size() >= 4 ? std::optional(Text) : std::nullopt;
      if (B < 32 || B >= 127)
        return {};
      Text += char(B);
    }
    return {};
  }
  bool feasible(SymContext &C, SymRef Pred, bool &Known) {
    if (auto N = value(C, Pred))
      return *N != 0;
    solver::SolverOptions O;
    O.BuildModel = false;
    O.Sat.MaxConflicts = 500;
    O.Sat.MaxPropagations = 50000;
    O.Sat.MaxWatchVisits = 500000;
    O.Blast.MaxGates = 65536;
    ++Queries;
    auto R = solver::checkSat(C, Pred, nullptr, O);
    if (R == solver::SatResult::Invalid)
      throw std::runtime_error("invalid path predicate");
    if (R == solver::SatResult::Unknown)
      Known = false;
    return R != solver::SatResult::Unsat;
  }
  void initialize(Path &P) {
    auto &C = P.State.context();
    if (auto *Seeds = config().getArray("registers"))
      for (auto &V : *Seeds) {
        auto &O = object(V);
        uint64_t Offset = field(O, "offset"), Bytes = field(O, "bytes");
        checkRange(Offset, Bytes);
        P.State.write(SymSpace::Register, Offset,
                      C.mkConst(Bytes * 8, field(O, "value")));
      }
    for (const auto &[Address, Bytes] : InitialMemory) {
      for (size_t I = 0; I < Bytes.size(); ++I)
        P.State.store(C.mkConst(64, Address + I),
                      C.mkConst(8, uint8_t(Bytes[I])));
    }
  }
  J::Object observeCall(Path &P, const J::Object *Spec, uint64_t Target,
                        unsigned PathID) {
    auto &C = P.State.context();
    J::Object Event{{"pc", hex(P.PC)},
                    {"function", hex(P.Function)},
                    {"target", hex(Target)},
                    {"path", PathID},
                    {"prior_opaque_calls", P.Opaque},
                    {"path_constraints_satisfiable", P.Feasible}};
    if (Spec) {
      Event["name"] = Spec->getString("name").value_or("external").str();
      if (auto *Selector = Spec->get("selector_register")) {
        Event["selector"] =
            describe(C, readRegister(P, number(*Selector),
                                     registerWidth(*Spec, "selector_bytes")));
        if (auto *Names = Spec->getObject("selector_names"))
          if (auto N = value(
                  C, readRegister(P, number(*Selector),
                                  registerWidth(*Spec, "selector_bytes"))))
            if (auto Name = Names->getString(hex(*N)))
              Event["service_name"] = Name->str();
      }
      J::Array Args;
      std::optional<uint64_t> Count;
      if (auto *R = Spec->get("argument_count_register")) {
        auto V = readRegister(P, number(*R),
                              registerWidth(*Spec, "argument_count_bytes"));
        Event["argument_count"] = describe(C, V);
        Count = value(C, V);
      }
      if (auto *Arguments = Spec->getArray("argument_registers"))
        for (auto &R : *Arguments) {
          if (Count && Args.size() >= uint32_t(*Count))
            break;
          auto V = readRegister(P, number(R));
          J::Object Arg{{"value", describe(C, V)}};
          if (auto Address = value(C, V)) {
            if (auto S = string(P, *Address); S && !S->empty())
              Arg["string"] = *S;
            else if (auto S = inputString(*Address))
              Arg["initial_string_reference"] = *S;
          }
          Args.push_back(std::move(Arg));
        }
      Event["arguments"] = std::move(Args);
      if (auto *A = Spec->getArray("indirect_argument_registers")) {
        J::Array Indirect;
        for (auto &R : *A) {
          auto Off = value(C, readRegister(P, number(R)));
          if (Off && *Off <= Decoder->profile().RegisterBytes - 8)
            Indirect.push_back(
                describe(C, P.State.read(SymSpace::Register, *Off, 8)));
          else
            Indirect.push_back(nullptr);
        }
        Event["indirect_arguments"] = std::move(Indirect);
      }
      if (auto *S = Spec->getObject("string")) {
        std::optional<uint64_t> PointerRegister;
        if (auto *R = S->get("pointer_register"))
          PointerRegister = number(*R);
        else if (auto *R = S->get("pointer_offset_register"))
          PointerRegister = value(C, readRegister(P, number(*R)));
        auto Address =
            PointerRegister &&
                    *PointerRegister <= Decoder->profile().RegisterBytes - 8
                ? value(C,
                        P.State.read(SymSpace::Register, *PointerRegister, 8))
                : std::nullopt;
        std::optional<uint64_t> Key = 0;
        if (auto *ModeReg = S->get("mode_register")) {
          auto Mode = value(C, readRegister(P, number(*ModeReg)));
          if (!Mode)
            Key.reset();
          else if (*Mode == field(*S, "encoded_mode"))
            Key = value(C, readRegister(P, field(*S, "key_register")));
        }
        if (Address && Key)
          if (auto Text = string(P, *Address, uint8_t(*Key))) {
            Event["string"] = *Text;
            Event["string_address"] = hex(*Address);
            Event["string_xor"] = hex(uint8_t(*Key));
          }
      }
    }
    return Event;
  }

  // An opt-in caller contract, never an inferred effect of an unknown call.
  // This is useful for read-only queries whose only visible result is written
  // back into an explicit VM state channel. Such paths remain conditional on
  // that contract, and reports identify every use.
  void applyRegisterEffects(Path &P, const J::Object &Spec) {
    if (Spec.getString("memory_effect") != "none")
      throw std::runtime_error(
          "register_effects requires explicit memory_effect=none");
    applyResult(P, Spec);
    applyRegisterWrites(P, Spec);
  }
  void applyResult(Path &P, const J::Object &Spec, SymRef Override = {}) {
    auto &C = P.State.context();
    if (auto *R = Spec.get("result_offset_register")) {
      auto Offset = value(C, readRegister(P, number(*R)));
      auto Bytes = registerWidth(Spec, "result_bytes");
      if (auto *W = Spec.get("result_width_register")) {
        auto Width = value(C, readRegister(P, number(*W)));
        if (!Width)
          throw std::runtime_error("unknown contract result width");
        Bytes = *Width;
      }
      if (!Offset)
        throw std::runtime_error("unknown contract result offset");
      checkRange(*Offset, Bytes);
      auto *Concrete = Spec.get("result_value");
      auto Result =
          Override.isValid()
              ? C.mkZExtOrTrunc(Override, Bytes * 8)
              : (Concrete ? C.mkConst(Bytes * 8, number(*Concrete))
                          : C.mkFreshVar(Bytes * 8, "external_result"));
      P.State.write(SymSpace::Register, *Offset, Result);
    }
  }
  void applyRegisterWrites(Path &P, const J::Object &Spec) {
    auto &C = P.State.context();
    if (auto *Writes = Spec.getArray("register_writes"))
      for (auto &V : *Writes) {
        auto &W = object(V);
        auto Offset = field(W, "offset"), Bytes = field(W, "bytes");
        checkRange(Offset, Bytes);
        P.State.write(SymSpace::Register, Offset,
                      C.mkConst(Bytes * 8, field(W, "value")));
      }
  }
  bool applyMemoryEffect(Path &P, const J::Object &Spec, const LowOp &Op) {
    auto &C = P.State.context();
    auto Count = value(C, readRegister(P, field(Spec, "size_register")));
    if (!Count || *Count > 4096)
      return false;
    auto Destination = readRegister(P, field(Spec, "destination_register"));
    auto Kind = Spec.getString("continuation");
    auto Source = Kind == "memory_havoc"
                      ? SymRef{}
                      : readRegister(P, field(Spec, "source_register"));
    for (auto Address :
         {Destination, Kind == "memory_copy" ? Source : SymRef{}})
      if (Address.isValid())
        if (auto N = value(C, Address);
            N && *Count && *N > UINT64_MAX - (*Count - 1))
          return false;
    if (*Count && Kind == "memory_copy" &&
        Spec.getBoolean("require_disjoint").value_or(false)) {
      auto D = value(C, Destination), S = value(C, Source);
      if (!D || !S || (*D <= *S ? *S - *D < *Count : *D - *S < *Count))
        return false;
    }
    SymCallEffect Effect;
    Effect.Memory = SymCallMemoryEffect::Preserve;
    for (uint64_t I = 0; I < *Count; ++I) {
      auto Offset = C.mkConst(64, I);
      auto Byte = Kind == "memory_copy"
                      ? P.State.load(C.mkAdd(Source, Offset), 1)
                  : Kind == "memory_fill" ? C.mkZExtOrTrunc(Source, 8)
                                          : C.mkFreshVar(8, "external_byte");
      Effect.Writes.push_back({C.mkAdd(Destination, Offset), Byte});
    }
    // The descriptor is a complete state-gateway contract. The shared engine
    // applies its memory writes, overlap/alias rules and preservation policy.
    SymExec Exec(C, P.State);
    std::vector<SymRegisterRange> Preserve;
    for (uint64_t I = 0; I < Decoder->profile().RegisterBytes;) {
      auto Size =
          std::min<uint64_t>(UINT16_MAX, Decoder->profile().RegisterBytes - I);
      Preserve.push_back({I, uint16_t(Size)});
      I += Size;
    }
    Exec.setCallPreservedRegisters(std::move(Preserve));
    if (Exec.step(Op, &Effect) == StepResult::Unmodelled)
      return false;
    applyResult(P, Spec,
                Spec.getBoolean("return_destination").value_or(false)
                    ? Destination
                    : SymRef{});
    applyRegisterWrites(P, Spec);
    return true;
  }

public:
  Observer(std::string Bytes, llvm::StringRef Profile, J::Value F,
           J::Value Options)
      : Code(std::move(Bytes)), Configuration(std::move(Options)) {
    auto P = readBytecodeProfile(Profile);
    if (!P)
      throw std::runtime_error(llvm::toString(P.takeError()));
    auto D = BytecodeDecoder::create(std::move(*P));
    if (!D)
      throw std::runtime_error(llvm::toString(D.takeError()));
    Decoder = std::move(*D);
    if (!F.getAsArray() || !Configuration.getAsObject())
      throw std::runtime_error("functions/options shape");
    arrayFields(config(), {"registers", "memory", "calls", "entries"});
    if (auto *Memory = config().getArray("memory"))
      for (const auto &V : *Memory) {
        const auto &O = object(V);
        auto Bytes = readFile(textField(O, "path"));
        auto Address = field(O, "address");
        if (Bytes.size() > 1024 * 1024 || Address > UINT64_MAX - Bytes.size())
          throw std::runtime_error("invalid memory seed");
        auto Next = InitialMemory.lower_bound(Address);
        if ((Next != InitialMemory.end() &&
             Next->first < Address + Bytes.size()) ||
            (Next != InitialMemory.begin() &&
             std::prev(Next)->first + std::prev(Next)->second.size() > Address))
          throw std::runtime_error("overlapping memory seeds");
        if (!InitialMemory.emplace(Address, std::move(Bytes)).second)
          throw std::runtime_error("duplicate memory seed");
      }
    llvm::ArrayRef<uint8_t> Raw(reinterpret_cast<const uint8_t *>(Code.data()),
                                Code.size());
    for (auto &V : *F.getAsArray()) {
      auto &O = object(V);
      auto Entry = field(O, "entry"), End = field(O, "end");
      std::string Name = textField(O, "name");
      auto Checked = Decoder->function(Raw, 0, Entry, End, Name);
      if (!Checked)
        throw std::runtime_error(llvm::toString(Checked.takeError()));
      if (!Functions.emplace(Entry, std::pair{End, Name}).second)
        throw std::runtime_error("duplicate function entry");
      for (const auto &B : Checked->Function.Blocks)
        for (const auto &Op : B.Ops)
          if (!Instructions.count(Op.Addr)) {
            auto I = Decoder->decode(Raw.drop_front(Op.Addr), Op.Addr);
            if (!I)
              throw std::runtime_error(llvm::toString(I.takeError()));
            Instructions.emplace(Op.Addr, std::move(*I));
          }
    }
    uint64_t PreviousEnd = 0;
    for (const auto &[Entry, F] : Functions) {
      if (Entry < PreviousEnd)
        throw std::runtime_error("overlapping function ranges");
      PreviousEnd = F.first;
    }
    if (auto *V = config().get("max_steps"))
      MaxSteps = number(*V);
    if (auto *V = config().get("max_paths"))
      MaxPaths = number(*V);
    if (auto *V = config().get("max_visits"))
      MaxVisits = number(*V);
    if (auto *V = config().get("max_milliseconds"))
      MaxMilliseconds = number(*V);
    if (!MaxSteps || MaxSteps > 1000000 || !MaxPaths || MaxPaths > 256 ||
        !MaxVisits || MaxVisits > 100000 || !MaxMilliseconds ||
        MaxMilliseconds > 600000)
      throw std::runtime_error("analysis budget out of range");
    if (auto *A = config().getArray("calls"))
      for (auto &V : *A) {
        auto &O = object(V);
        validateCall(O);
        if (Functions.count(field(O, "target")))
          throw std::runtime_error(
              "external binding conflicts with a function");
        if (!Calls.emplace(field(O, "target"), &O).second)
          throw std::runtime_error("duplicate call target");
      }
  }

  void run() {
    for (const auto &[Entry, F] : Functions) {
      if (auto *Selected = config().getArray("entries")) {
        bool Found = false;
        for (auto &V : *Selected)
          Found |= number(V) == Entry;
        if (!Found)
          continue;
      }
      SymContext C;
      std::deque<Path> Work;
      Path Initial(C, Entry, Decoder->profile().ByteOrder);
      initialize(Initial);
      Work.push_back(std::move(Initial));
      J::Array Events, Stops, Strings;
      std::set<std::pair<uint64_t, std::string>> SeenStrings;
      unsigned Paths = 0, Steps = 0;
      auto Deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(MaxMilliseconds);
      while (!Work.empty() && Paths < MaxPaths) {
        Path P = std::move(Work.front());
        Work.pop_front();
        unsigned PathID = Paths++;
        std::string Stop;
        while (Stop.empty()) {
          if (std::chrono::steady_clock::now() >= Deadline) {
            Stop = "time_budget";
            break;
          }
          if (++P.Steps > MaxSteps || ++Steps > MaxSteps * 4 ||
              C.numNodes() > 1000000) {
            Stop = "step_or_expression_budget";
            break;
          }
          if (++P.Visits[P.PC] > MaxVisits) {
            Stop = "loop_budget";
            break;
          }
          auto It = Instructions.find(P.PC);
          if (It == Instructions.end()) {
            Stop = "unknown_instruction_boundary";
            break;
          }
          auto &I = It->second;
          uint64_t Next = P.PC + I.Size;
          SymExec Exec(C, P.State);
          for (auto &Op : I.Operations) {
            if (Op.Opcode == NdOp::CALL) {
              auto Target = value(C, Exec.operandValue(Op.Inputs[0]));
              if (!Target) {
                Stop = "unknown_call_target";
                break;
              }
              if (Functions.count(*Target)) {
                if (P.Frames.size() == 32) {
                  Stop = "call_depth_budget";
                  break;
                }
                P.Frames.push_back({P.Function, Next});
                P.Function = *Target;
                Next = *Target;
                break;
              } else {
                auto It = Calls.find(*Target);
                const J::Object *Spec =
                    It == Calls.end() ? nullptr : It->second;
                if (Spec && Spec->get("selector_register"))
                  if (auto *Cases = Spec->getObject("selector_cases"))
                    if (auto Selector = value(
                            C, readRegister(
                                   P, field(*Spec, "selector_register"),
                                   registerWidth(*Spec, "selector_bytes"))))
                      if (auto *Selected = Cases->getObject(hex(*Selector)))
                        Spec = Selected;
                auto Event = observeCall(P, Spec, *Target, PathID);
                if (Spec && Spec->get("dispatch_register")) {
                  auto Dispatch = value(
                      C, readRegister(P, field(*Spec, "dispatch_register")));
                  if (!Dispatch || !Functions.count(*Dispatch)) {
                    Events.push_back(std::move(Event));
                    Stop = "unknown_bytecode_dispatch";
                    break;
                  }
                  if (P.Frames.size() == 32) {
                    Stop = "call_depth_budget";
                    break;
                  }
                  Event["dispatched_to"] = hex(*Dispatch);
                  Events.push_back(std::move(Event));
                  P.Frames.push_back({P.Function, Next});
                  P.Function = *Dispatch;
                  Next = *Dispatch;
                  break;
                }
                if (Spec &&
                    Spec->getString("continuation") == "register_effects") {
                  Event["continuation_contract"] =
                      "register_effects; memory_effect=none";
                  Events.push_back(std::move(Event));
                  applyRegisterEffects(P, *Spec);
                  ++P.Opaque;
                  continue;
                }
                if (Spec && Spec->getString("continuation")
                                .value_or("")
                                .starts_with("memory_")) {
                  Event["continuation_contract"] =
                      *Spec->getString("continuation");
                  Events.push_back(std::move(Event));
                  if (!applyMemoryEffect(P, *Spec, Op)) {
                    Stop = "unresolved_memory_effect";
                    break;
                  }
                  ++P.Opaque;
                  continue;
                }
                Events.push_back(std::move(Event));
                // Only an explicit analysis contract may retain registers.
                // Memory always becomes unknown after an opaque call.
                if (!Spec || Spec->getString("continuation") != "havoc") {
                  Stop = "external_call";
                  break;
                }
                std::vector<SymRegisterRange> Preserve;
                if (auto *A = Spec->getArray("preserve_registers"))
                  for (auto &V : *A) {
                    auto &O = object(V);
                    auto Off = field(O, "offset"), Bytes = field(O, "bytes");
                    auto Size = Decoder->profile().RegisterBytes;
                    if (!Bytes || Bytes > UINT16_MAX || Off > Size ||
                        Bytes > Size - Off)
                      throw std::runtime_error(
                          "invalid preserved register range");
                    Preserve.push_back({Off, uint16_t(Bytes)});
                  }
                Exec.setCallPreservedRegisters(std::move(Preserve));
                Exec.step(Op);
                ++P.Opaque;
                if (auto *R = Spec->get("result_offset_register")) {
                  auto Off = value(C, readRegister(P, number(*R)));
                  auto Width = value(
                      C,
                      readRegister(P, field(*Spec, "result_width_register")));
                  auto Size = Decoder->profile().RegisterBytes;
                  if (!Off || !Width || !*Width || *Width > 8 || *Off > Size ||
                      *Width > Size - *Off) {
                    Stop = "unknown_result_channel";
                    break;
                  }
                  P.State.write(SymSpace::Register, *Off,
                                C.mkFreshVar(*Width * 8, "external_result"));
                }
                applyRegisterWrites(P, *Spec);
              }
              continue;
            }
            auto Step = Exec.step(Op);
            if (Step == StepResult::Unmodelled || Exec.opaqueOperationCount()) {
              Stop = "unsupported_semantics";
              break;
            }
            if (Step == StepResult::Return) {
              if (P.Frames.empty()) {
                Stop = "returned";
                break;
              }
              auto Frame = P.Frames.back();
              P.Frames.pop_back();
              P.Function = Frame.Function;
              Next = Frame.ReturnPC;
              break;
            } else if (Step == StepResult::Branch ||
                       Step == StepResult::CondBranch) {
              auto Target = value(C, Exec.branchTarget());
              if (!Target) {
                Stop = "unknown_branch_target";
                break;
              }
              if (Step == StepResult::Branch)
                Next = *Target;
              else {
                auto Cond = Exec.branchCondition();
                if (auto V = value(C, Cond)) {
                  if (*V)
                    Next = *Target;
                } else {
                  Path Taken = P;
                  Taken.PC = *Target;
                  Taken.Predicate = C.mkAnd(P.Predicate, Cond);
                  if (feasible(C, Taken.Predicate, Taken.Feasible)) {
                    if (Work.size() + Paths < MaxPaths)
                      Work.push_back(std::move(Taken));
                    else
                      Stops.push_back(J::Object{{"pc", hex(P.PC)},
                                                {"reason", "path_budget"}});
                  }
                  P.Predicate = C.mkAnd(P.Predicate, C.mkNot(Cond));
                  if (!feasible(C, P.Predicate, P.Feasible)) {
                    Stop = "infeasible";
                    break;
                  }
                }
              }
            } else if (Step == StepResult::IndirectBranch) {
              Stop = "unresolved_indirect_branch";
              break;
            }
            if (Op.Output.Size && Op.Output.Space == VnodeSpace::REG) {
              auto V = value(C, P.State.read(SymSpace::Register,
                                             Op.Output.Offset, Op.Output.Size));
              if (V) {
                auto Text = string(P, *V);
                llvm::StringRef Provenance = "symbolic_memory";
                if (!Text || Text->size() < 4) {
                  Text = inputString(*V);
                  Provenance = "initial_image_reference";
                }
                if (Text && Text->size() >= 4 &&
                    SeenStrings.emplace(*V, *Text).second)
                  Strings.push_back(
                      J::Object{{"pc", hex(P.PC)},
                                {"address", hex(*V)},
                                {"text", *Text},
                                {"provenance", Provenance},
                                {"prior_opaque_calls", P.Opaque}});
              }
            }
          }
          if (Stop.empty())
            P.PC = Next;
        }
        Stops.push_back(J::Object{{"pc", hex(P.PC)},
                                  {"reason", Stop},
                                  {"steps", P.Steps},
                                  {"path", PathID}});
      }
      llvm::errs() << F.second << ": " << Paths << " paths, " << Events.size()
                   << " calls\n";
      Results.push_back(J::Object{{"entry", hex(Entry)},
                                  {"name", F.second},
                                  {"steps", Steps},
                                  {"calls", std::move(Events)},
                                  {"strings", std::move(Strings)},
                                  {"stops", std::move(Stops)}});
    }
  }
  J::Value result() {
    return J::Object{
        {"schemaVersion", 1},
        {"engine", "NeverD BytecodeDecoder + SymExec + BitVectorSolver"},
        {"scope", "Independent function entries under explicit register/memory "
                  "assumptions; observations, not whole-program equivalence."},
        {"solver_queries", Queries},
        {"functions", std::move(Results)}};
  }
};

std::string commentText(llvm::StringRef Text) {
  std::string Out;
  for (size_t I = 0; I < Text.size() && I < 1000; ++I) {
    unsigned char C = Text[I];
    if (C == '*' && I + 1 < Text.size() && Text[I + 1] == '/')
      Out += "* ";
    else if (C < 32 || C >= 127)
      Out += ' ';
    else
      Out += char(C);
  }
  return Out;
}

std::string annotate(llvm::StringRef Source, const J::Value &Report) {
  auto *Functions = object(Report).getArray("functions");
  if (!Functions)
    throw std::runtime_error("missing observation functions");
  std::map<std::string, std::string> Comments;
  for (const auto &V : *Functions) {
    const auto &F = object(V);
    auto Name = textField(F, "name");
    std::string Text =
        "/* NeverD observations: " + commentText(Name) +
        "\n"
        " * Conditional on supplied inputs and call contracts; paths may be "
        "incomplete.\n"
        " * These comments do not change the emitted function body.\n";
    std::set<std::string> Seen;
    if (auto *Calls = F.getArray("calls"))
      for (const auto &V : *Calls) {
        const auto &Call = object(V);
        auto Label = Call.getString("service_name")
                         .value_or(Call.getString("name").value_or("external"));
        std::string Line = " * " + Call.getString("pc").value_or("?").str() +
                           ": " + Label.str();
        if (auto String = Call.getString("string"))
          Line += " " + llvm::formatv("{0}", J::Value(*String)).str();
        if (auto *Args = Call.getArray("arguments"))
          for (size_t I = 0; I < Args->size(); ++I)
            if (auto *Arg = (*Args)[I].getAsObject()) {
              if (auto String = Arg->getString("string"))
                Line += " arg" + std::to_string(I) + "=" +
                        llvm::formatv("{0}", J::Value(*String)).str();
              else if (auto String = Arg->getString("initial_string_reference"))
                Line += " arg" + std::to_string(I) + " references initial " +
                        llvm::formatv("{0}", J::Value(*String)).str();
              else if (auto Value = Arg->getString("value"))
                Line += " arg" + std::to_string(I) + "=" + Value->str();
            }
        if (auto N = Call.getInteger("prior_opaque_calls"); N && *N)
          Line += " [after abstracted calls]";
        if (Seen.size() < 96 && Seen.insert(Line).second)
          Text += commentText(Line) + "\n";
      }
    if (auto *Strings = F.getArray("strings"))
      for (const auto &V : *Strings) {
        const auto &S = object(V);
        if (auto String = S.getString("text")) {
          std::string Line = " * string " +
                             S.getString("pc").value_or("?").str() + ": " +
                             llvm::formatv("{0}", J::Value(*String)).str();
          if (S.getString("provenance") == "initial_image_reference")
            Line += " [initial image reference only]";
          if (Seen.size() < 128 && Seen.insert(Line).second)
            Text += commentText(Line) + "\n";
        }
      }
    Text += " */\n";
    Comments.emplace("uint64_t " + Name + "(", std::move(Text));
  }
  std::string Out = "/* NeverD evidence-annotated view. See the observation "
                    "JSON for scope and stops. */\n";
  while (!Source.empty()) {
    auto [Line, Rest] = Source.split('\n');
    if (Line.trim().ends_with("{"))
      for (const auto &[Prefix, Comment] : Comments)
        if (Line.trim().starts_with(Prefix)) {
          Out += Comment;
          break;
        }
    Out += Line.str();
    if (Source.size() != Line.size())
      Out += '\n';
    Source = Rest;
  }
  return Out;
}
} // namespace

int main(int Argc, char **Argv) {
  if (Argc == 5 && llvm::StringRef(Argv[1]) == "--annotate-c") {
    try {
      auto Source = annotate(readFile(Argv[2]), parse(readFile(Argv[3])));
      std::ofstream Out(Argv[4]);
      Out << Source;
      if (!Out)
        throw std::runtime_error("output write failed");
      return 0;
    } catch (const std::exception &E) {
      llvm::errs() << E.what() << '\n';
      return 1;
    }
  }
  if (Argc != 6) {
    llvm::errs() << "usage: neverd-bytecode-observe code.bin profile.json "
                    "functions.json inputs.json output.json\n";
    llvm::errs() << "   or: neverd-bytecode-observe --annotate-c source.c "
                    "observations.json annotated.c\n";
    return 2;
  }
  try {
    Observer O(readFile(Argv[1]), readFile(Argv[2]), parse(readFile(Argv[3])),
               parse(readFile(Argv[4])));
    O.run();
    std::ofstream Out(Argv[5]);
    Out << llvm::formatv("{0:2}", O.result()).str() << '\n';
    if (!Out)
      throw std::runtime_error("output write failed");
  } catch (const std::exception &E) {
    llvm::errs() << E.what() << '\n';
    return 1;
  }
  return 0;
}
