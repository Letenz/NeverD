//===- NeverDCAPIRegisterFlow.cpp - Register query adapter ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Register query adapter.
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/decode/NativeOperands.h"
#include "neverd/ir/RegisterFlow.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/sdk/Limits.h"

#include "llvm/ADT/StringExtras.h"

using namespace neverd;
using namespace neverd::sdk;
namespace {
bool sameStorage(NdVar A, NdVar B) {
  return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
}
bool sameInstructionShape(llvm::ArrayRef<LowOp> Actual,
                          llvm::ArrayRef<LowOp> Fresh) {
  if (Actual.size() != Fresh.size())
    return false;
  for (size_t I = 0; I < Actual.size(); ++I) {
    const auto &A = Actual[I], &B = Fresh[I];
    if (A.Opcode != B.Opcode || A.NumInputs != B.NumInputs ||
        !sameStorage(A.Output, B.Output) ||
        A.MemoryOrdering != B.MemoryOrdering ||
        A.MemoryAddressSpace != B.MemoryAddressSpace)
      return false;
    for (unsigned J = 0; J < A.NumInputs; ++J)
      if (!sameStorage(A.Inputs[J], B.Inputs[J]) &&
          !(A.Inputs[J].isConst() && B.Inputs[J].isConst() &&
            A.Inputs[J].Size == B.Inputs[J].Size &&
            isExactAddressProvenance(A.Inputs[J].Provenance)))
        return false;
  }
  return true;
}
bool decode(Session &S, Decoder &D, va_t PC, DecodedInsn &I,
            std::optional<InstructionMode> Mode = {}) {
  const auto *Seg = S.Img.getSegmentFor(PC);
  if (!Seg || PC < Seg->VA || PC - Seg->VA >= Seg->Data.size() ||
      !D.selectMode(S.Img, PC, Mode))
    return false;
  return D.decodeOne(Seg->Data.data() + (PC - Seg->VA),
                     std::min<size_t>(16, Seg->Data.size() - (PC - Seg->VA)),
                     PC, I) > 0;
}
} // namespace

const char *neverd_register_flow_json(neverd_session_t Sess, neverd_va_t Entry,
                                      neverd_va_t PC, const char *OptionsJSON) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    const llvm::StringRef Text = OptionsJSON ? OptionsJSON : "{}";
    if (Text.size() > kMaxRegisterFlowOptionsBytes)
      throw std::invalid_argument("Register query exceeds 4 KiB");
    auto Parsed = llvm::json::parse(Text);
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      throw std::invalid_argument("Invalid register query JSON");
    }
    const auto *Options = Parsed->getAsObject();
    if (!Options)
      throw std::invalid_argument("Register query requires an object");
    for (const auto &Field : *Options)
      if ((Field.first != "kind" && Field.first != "register") ||
          !Field.second.getAsString())
        throw std::invalid_argument(
            "Register query accepts only string kind and register");
    if (S->Img.Arch != Arch::X64 && S->Img.Arch != Arch::X86 &&
        S->Img.Arch != Arch::ARM && S->Img.Arch != Arch::AArch64)
      throw std::invalid_argument(
          "Register flow requires a native x86 or ARM image");
    const auto Kind = Options->getString("kind").value_or("registers");
    if (!neverd_prepare_function_view(Sess, Entry, "low") &&
        !S->findLowFunc(Entry))
      return nullptr;
    S->clearError();
    const auto *F = S->findLowFunc(Entry);
    if (!F || F->Blocks.size() > kMaxRegisterFlowBlocks)
      throw std::invalid_argument("No bounded original native function");
    const auto &TRI = getTargetRegInfo(S->Img.Arch);
    std::set<std::pair<uint64_t, uint16_t>> Storage;
    std::map<va_t, std::vector<LowOp>> Instructions;
    std::map<va_t, InstructionMode> Modes;
    size_t Count = 0;
    auto Remember = [&](NdVar V) {
      if (V.isReg() && V.Size && V.Size <= 64)
        Storage.emplace(V.Offset, V.Size);
    };
    for (const auto &B : F->Blocks) {
      for (const auto &Boundary : B.InstructionBoundaries)
        Modes[Boundary.Address] = Boundary.Mode;
      for (const auto &O : B.Ops) {
        if (++Count >
            ::neverd::sdk::
                kNeverDCAPIRegisterFlowNeverdRegisterFlowJsonCountLimit)
          throw std::invalid_argument(
              "Register-flow operation budget exceeded");
        Instructions[O.Addr].push_back(O);
        Remember(O.Output);
        for (unsigned J = 0; J < O.NumInputs; ++J)
          Remember(O.Inputs[J]);
      }
    }
    for (const auto &Sub : TRI.SubRegs) {
      Storage.emplace(Sub.WideRegOff, Sub.WideSize);
      Storage.emplace(Sub.NarrowRegOff, Sub.NarrowSize);
    }
    if (!Instructions.count(PC) ||
        Instructions.size() > kMaxRegisterFlowInstructions)
      throw std::invalid_argument(
          "Select a retained instruction within the register-flow budget");
    Decoder D;
    if (!D.init(S->Img))
      throw std::invalid_argument("No native decoder for register queries");
    DecodedInsn Selected{};
    if (!decode(*S, D, PC, Selected,
                Modes.count(PC) ? std::optional(Modes.at(PC)) : std::nullopt))
      throw std::invalid_argument("Unknown instruction boundary or mode");
    const auto Operands = describeNativeOperands(D, Selected, S->Img.Arch);
    std::map<std::string, NdVar> Registers;
    llvm::json::Array Inventory;
    for (const auto &Reg : Operands.Registers)
      for (const auto &[Offset, Bytes] : Storage) {
        const auto *Name =
            TRI.GetRegName ? TRI.GetRegName(Offset, Bytes) : nullptr;
        if (!Name || !llvm::StringRef(Name).equals_insensitive(Reg.Name))
          continue;
        Registers[llvm::StringRef(Reg.Name).lower()] =
            NdVar::reg(Offset, Bytes);
        Inventory.push_back(llvm::json::Object{{"name", Reg.Name},
                                               {"bytes", Bytes},
                                               {"read", Reg.Read},
                                               {"write", Reg.Write}});
        break;
      }
    if (Kind == "registers")
      return dupStr(jsonToString(llvm::json::Value(
          llvm::json::Object{{"registers", std::move(Inventory)}})));
    const auto Name = Options->getString("register").value_or("").lower();
    if (!Registers.count(Name))
      throw std::invalid_argument(
          "Select a retained register used by this decoded instruction");
    std::map<va_t, RegisterValueAudit> Audits;
    if (Kind == "value") {
      D.setStrict(true);
      for (const auto &[At, Ops] : Instructions) {
        auto &Audit = Audits[At];
        DecodedInsn I{};
        if (!decode(*S, D, At, I,
                    Modes.count(At) ? std::optional(Modes.at(At))
                                    : std::nullopt))
          continue;
        std::vector<LowOp> Fresh;
        LowInstructionUndefinedEffects Undefined;
        try {
          D.liftToLow(I, Fresh, {}, {}, &Undefined);
        } catch (const std::exception &) {
          continue;
        }
        Audit.Complete = Undefined.Coverage == LowUndefinedCoverage::Complete &&
                         sameInstructionShape(Ops, Fresh);
        if (Audit.Complete)
          for (const auto &Effect : Undefined.Effects)
            Audit.Undefined.emplace_back(Effect.AfterOp, Effect.Output);
      }
    }
    auto Result = queryRegisterFlow(*F, S->Img.Arch, PC, Registers.at(Name),
                                    Kind.str(), Audits);
    if (!Result.Error.empty())
      throw std::invalid_argument(Result.Error);
    llvm::json::Array Sites;
    for (const auto &Site : Result.Sites) {
      DecodedInsn I{};
      std::string Context;
      if (decode(*S, D, Site.Address, I,
                 Modes.count(Site.Address)
                     ? std::optional(Modes.at(Site.Address))
                     : std::nullopt))
        Context = std::string(I.Raw->mnemonic) + " " + I.Raw->op_str;
      Sites.push_back(llvm::json::Object{{"address", vaHex(Site.Address)},
                                         {"kind", Site.Kind},
                                         {"name", Site.Reason},
                                         {"context", Context}});
    }
    llvm::json::Object Reply{{"register", Name},
                             {"instruction", vaHex(PC)},
                             {"kind", Kind},
                             {"unknown", Result.Unknown},
                             {"items", std::move(Sites)}};
    Reply["value"] =
        Result.Value ? llvm::json::Value("0x" + llvm::utohexstr(*Result.Value))
                     : llvm::json::Value(nullptr);
    if (Kind == "value" && !Result.Value)
      Reply["reason"] = "No single defined static value reaches this "
                        "instruction (entry, memory, path disagreement, "
                        "undefined output, call or unsupported value transfer)";
    return dupStr(jsonToString(llvm::json::Value(std::move(Reply))));
  } catch (const std::exception &E) {
    S->setError(E.what());
    return nullptr;
  }
}
