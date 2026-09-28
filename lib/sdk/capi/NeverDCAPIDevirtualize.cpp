//===- NeverDCAPIDevirtualize.cpp - Interpreter recovery C API -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIDevirtualize.h"

#include "SessionImpl.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/SHA256.h"

using namespace neverd;
using namespace neverd::sdk;

namespace {
const char *statusName(analysis::SpecializationStatus Status) {
  switch (Status) {
  case analysis::SpecializationStatus::Complete:
    return "complete";
  case analysis::SpecializationStatus::InvalidInput:
    return "invalid-input";
  case analysis::SpecializationStatus::Unsupported:
    return "unsupported";
  case analysis::SpecializationStatus::UnresolvedControl:
    return "unresolved-control";
  case analysis::SpecializationStatus::BudgetExceeded:
    return "budget-exceeded";
  }
  return "invalid-input";
}
} // namespace

extern "C" const char *
neverd_devirtualize_source_v1(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v1 *Options,
                              const char **Report) {
  if (Report)
    *Report = nullptr;
  auto *S = toSession(Session);
  if (!S)
    return nullptr;
  S->clearError();
  llvm::json::Object Evidence{
      {"schemaVersion", 1},
      {"entry", vaHex(Entry)},
      {"status", "invalid-input"},
      {"complete", false},
      {"contract", "fixed image mappings and permissions; no concurrent "
                   "mutation; source recovery only"}};
  auto PublishReport = [&] {
    if (Report)
      *Report = dupStr(
          llvm::formatv("{0:2}", llvm::json::Value(std::move(Evidence))).str());
  };
  auto Fail = [&](const std::string &Error) -> const char * {
    S->setError(Error);
    Evidence["error"] = Error;
    PublishReport();
    return nullptr;
  };
  try {
    PipelineOptions PO;
    PO.OnlyFunctionEntries = {Entry};
    PO.InterpreterSpecialization.emplace();
    auto &Config = *PO.InterpreterSpecialization;
    if (Options) {
      if (Options->struct_size < sizeof(*Options))
        return Fail(
            "devirtualize options do not cover the complete v1 structure");
      if (Options->reserved ||
          (Options->use_llvm != 0 && Options->use_llvm != 1) ||
          (Options->no_opt != 0 && Options->no_opt != 1))
        return Fail("invalid devirtualize flags");
      if (Options->control_register_count > 16 ||
          (Options->control_register_count && !Options->control_registers))
        return Fail("invalid control register list");
      if (S->Img.Arch != Arch::X64)
        return Fail("interpreter recovery currently requires x64");
      const auto &TRI = getTargetRegInfo(Arch::X64);
      for (size_t I = 0; I < Options->control_register_count; ++I) {
        const char *Name = Options->control_registers[I];
        bool Found = false;
        if (Name)
          for (uint64_t Offset : TRI.GeneralRegs)
            if (llvm::StringRef(Name).equals_insensitive(
                    TRI.GetRegName(Offset, 8))) {
              Config.ControlRegisters.push_back({Offset, 8});
              Found = true;
              break;
            }
        if (!Found)
          return Fail("control register must be a full x64 general register");
      }
      if (Options->control_frame_slot_count > 64 ||
          (Options->control_frame_slot_count && !Options->control_frame_slots))
        return Fail("invalid frame control slot list");
      for (size_t I = 0; I < Options->control_frame_slot_count; ++I) {
        const auto &Slot = Options->control_frame_slots[I];
        if (!Slot.bytes || Slot.bytes > 8 || Slot.reserved)
          return Fail("invalid frame control slot range");
        Config.ControlFrameSlots.push_back({Slot.offset, Slot.bytes});
      }
      if (Options->max_nodes)
        Config.MaxNodes = Options->max_nodes;
      if (Options->max_contexts_per_address)
        Config.MaxContextsPerAddress = Options->max_contexts_per_address;
      if (Options->max_operations)
        Config.MaxOperations = Options->max_operations;
      PO.LiftMode = Options->use_llvm != 0;
      PO.NoOpt = Options->no_opt != 0;
    }
    llvm::LLVMContext Context;
    if (S->Img.InputFileSHA256)
      Evidence["imageSha256"] = llvm::toHex(*S->Img.InputFileSHA256);
    else if (!S->Img.Raw.empty())
      Evidence["imageSha256"] = llvm::toHex(llvm::SHA256::hash(S->Img.Raw));
    else
      Evidence["imageSha256"] = nullptr;
    llvm::json::Array Controls;
    for (const auto &Range : Config.ControlRegisters)
      Controls.push_back(
          llvm::json::Object{{"offset", static_cast<int64_t>(Range.Offset)},
                             {"bytes", Range.Bytes}});
    Evidence["controlRegisters"] = std::move(Controls);
    llvm::json::Array FrameSlots;
    for (const auto &Slot : Config.ControlFrameSlots)
      FrameSlots.push_back(
          llvm::json::Object{{"offset", Slot.Offset}, {"bytes", Slot.Bytes}});
    Evidence["controlFrameSlots"] = std::move(FrameSlots);
    Evidence["frameRoot"] = "entry-rsp";
    Evidence["returnContract"] =
        "ordinary ABI return; every external-origin store target range, "
        "including computed external addresses, is disjoint from the entry "
        "return-address slot (environment precondition); root-derived writes "
        "checked";
    Evidence["maxNodes"] = Config.MaxNodes;
    Evidence["maxContextsPerAddress"] = Config.MaxContextsPerAddress;
    Evidence["maxOperations"] = static_cast<int64_t>(Config.MaxOperations);
    Evidence["maxNodeEvaluations"] = Config.MaxNodeEvaluations;
    Evidence["maxIndirectTargets"] = Config.MaxIndirectTargets;
    Evidence["maxImmutableReadAddresses"] = Config.MaxImmutableReadAddresses;
    Evidence["maxControlTuples"] = Config.MaxControlTuples;
    Evidence["maxControlFields"] = Config.MaxControlFields;
    Evidence["maxSolverQueries"] =
        static_cast<int64_t>(Config.MaxSolverQueries);
    Evidence["maxSolverGates"] = static_cast<int64_t>(Config.MaxSolverGates);
    Evidence["maxSolverConflicts"] =
        static_cast<int64_t>(Config.MaxSolverConflicts);
    Evidence["maxSolverPropagations"] =
        static_cast<int64_t>(Config.MaxSolverPropagations);
    Evidence["maxSolverWatchVisits"] =
        static_cast<int64_t>(Config.MaxSolverWatchVisits);
    Evidence["maxSymbolicNodes"] =
        static_cast<int64_t>(Config.MaxSymbolicNodes);
    Pipeline P;
    auto Result = P.run(S->Img, Context, PO);
    if (Result.InterpreterRecovery) {
      const auto &R = *Result.InterpreterRecovery;
      Evidence["status"] = statusName(R.Status);
      Evidence["controlComplete"] = R.complete();
      Evidence["contexts"] = static_cast<int64_t>(R.Contexts);
      Evidence["nodeEvaluations"] = static_cast<int64_t>(R.NodeEvaluations);
      Evidence["evaluatedOperations"] =
          static_cast<int64_t>(R.EvaluatedOperations);
      Evidence["solverQueries"] = static_cast<int64_t>(R.SolverQueries);
      Evidence["relationalWidenings"] = R.RelationalWidenings;
      Evidence["residualBlocks"] =
          static_cast<int64_t>(R.Residual.Blocks.size());
      llvm::json::Array Reads;
      for (const auto &Read : R.Reads)
        Reads.push_back(llvm::json::Object{
            {"instruction", vaHex(Read.InstructionAddress)},
            {"operation", Read.OpSeq},
            {"address", vaHex(Read.Address)},
            {"bytes", llvm::toHex(llvm::ArrayRef<uint8_t>(Read.Bytes))},
            {"evidence", Read.Evidence}});
      Evidence["immutableReads"] = std::move(Reads);
      llvm::json::Array Origins;
      for (const auto &Origin : R.Origins)
        Origins.push_back(llvm::json::Object{
            {"residual", vaHex(Origin.ResidualAddress)},
            {"native", vaHex(Origin.NativeInstruction.Address)},
            {"size", Origin.NativeInstruction.Size}});
      Evidence["origins"] = std::move(Origins);
    }
    if (!Result.Success || Result.MedIRVerifierFailures ||
        Result.LLVMVerifierFailed || Result.BackendUnhandledValueIntrinsics)
      return Fail(Result.Error.empty() ? "recovered IR verification failed"
                                       : Result.Error);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions EmitOptions;
    EmitOptions.TheArch = S->Img.Arch;
    EmitOptions.Format = S->Img.Format;
    EmitOptions.Image = &S->Img;
    if (PO.LiftMode) {
      if (!Result.LlvmModule)
        return Fail("recovery produced no LLVM module");
      LLVMCEmitter Emitter;
      if (!Emitter.emit(*Result.LlvmModule, OS, EmitOptions, nullptr, &S->Img))
        return Fail("recovered LLVM-to-C emission failed");
    } else {
      if (Result.HighFuncs.empty())
        return Fail("recovery produced no source function");
      HighCEmitter Emitter;
      if (!Emitter.emit(Result.HighFuncs, OS, EmitOptions))
        return Fail("recovered HighC emission failed");
    }
    Evidence["complete"] = true;
    PublishReport();
    return dupStr(Source);
  } catch (const std::exception &Error) {
    return Fail(Error.what());
  }
}
