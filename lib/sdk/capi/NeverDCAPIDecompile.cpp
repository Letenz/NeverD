//===- NeverDCAPIDecompile.cpp - C API: decompilation and IR
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Single-function decompilation, multi-stage IR dump (Low/Med/High/LLVM),
/// and high-level pipeline operations for lift and decompile-all.
///
//===----------------------------------------------------------------------===//

#include "LibraryPresentation.h"
#include "SessionImpl.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/emit/EVMCEmitter.h"
#include "neverd/evm/emit/EVMSolidityEmitter.h"
#include "neverd/ir/NdOps.h"
#include "neverd/loader/ARMModeCLIStrings.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/emit/SBFCEmitter.h"
#include "neverd/sbf/emit/SBFRustEmitter.h"

#include <limits>
#include <set>
#include <stdexcept>

using namespace neverd;
using namespace neverd::sdk;

namespace {
struct IRRowOrigin {
  std::string ObjectId;
  const char *Kind = "decoration";
  const char *Status = "unmapped";
  std::optional<va_t> Address;
  int Sequence = -1;
};
using IRRowSink = std::function<void(llvm::StringRef, const IRRowOrigin &)>;

std::set<std::pair<va_t, int>> instructionOrigins(const LowFunc &F) {
  std::set<std::pair<va_t, int>> Origins;
  for (const auto &B : F.Blocks)
    for (const auto &Boundary : B.InstructionBoundaries) {
      if (Boundary.FirstOp > B.Ops.size() ||
          Boundary.OpCount > B.Ops.size() - Boundary.FirstOp)
        continue;
      for (uint64_t I = Boundary.FirstOp;
           I < Boundary.FirstOp + Boundary.OpCount; ++I) {
        const auto &Op = B.Ops[I];
        if (Op.Seq >= 0 && Op.Addr == Boundary.Address && Boundary.Size > 0)
          Origins.emplace(Op.Addr, Op.Seq);
      }
    }
  return Origins;
}

std::string rowId(llvm::StringRef Stage, va_t Entry, int Block,
                  llvm::StringRef Kind, size_t Index = 0) {
  return (Stage + ":" + vaHex(Entry) + ":block:" + std::to_string(Block) + ":" +
          Kind + ":" + std::to_string(Index))
      .str();
}

// Both legacy text and mapped pages use these exact emission points. A name or
// operand containing a newline is split into physical rows by the page sink,
// rather than guessed from a parallel line-count model.
void emitLowView(const LowFunc &F, const IRRowSink &Sink,
                 bool CollectAnchors = false) {
  const auto Origins =
      CollectAnchors ? instructionOrigins(F) : std::set<std::pair<va_t, int>>();
  auto Emit = [&](const IRRowOrigin &Origin, auto Write) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Write(OS);
    Sink(Text, Origin);
  };
  Emit({"low:" + vaHex(F.Entry) + ":header", "header"}, [&](auto &OS) {
    OS << "; LowIR: " << F.Name << " @ " << vaHex(F.Entry) << "\n";
  });
  for (const auto &B : F.Blocks) {
    Emit({rowId("low", F.Entry, B.Id, "block"), "block"}, [&](auto &OS) {
      OS << "block_" << B.Id << ":  ; [" << vaHex(B.StartAddr) << " - "
         << vaHex(B.EndAddr) << ")\n";
    });
    for (size_t Index = 0; Index < B.Ops.size(); ++Index) {
      const auto &Op = B.Ops[Index];
      IRRowOrigin Origin{rowId("low", F.Entry, B.Id, "op", Index) + ":" +
                             vaHex(Op.Addr) + ":seq:" + std::to_string(Op.Seq),
                         "operation"};
      if (Origins.count({Op.Addr, Op.Seq})) {
        Origin.Address = Op.Addr;
        Origin.Sequence = Op.Seq;
        Origin.Status = "instruction_anchor";
      }
      Emit(Origin, [&](auto &OS) {
        OS << "  " << ndOpName(Op.Opcode);
        if (Op.Output.Size > 0)
          OS << " -> (" << static_cast<int>(Op.Output.Space) << ":"
             << Op.Output.Offset << ":" << Op.Output.Size << ")";
        for (int I = 0; I < Op.NumInputs; ++I)
          OS << " (" << static_cast<int>(Op.Inputs[I].Space) << ":"
             << Op.Inputs[I].Offset << ":" << Op.Inputs[I].Size << ")";
        OS << "\n";
      });
    }
    Emit({rowId("low", F.Entry, B.Id, "successors"), "successors"},
         [&](auto &OS) {
           OS << "  succs: [";
           for (size_t I = 0; I < B.Succs.size(); ++I) {
             if (I)
               OS << ", ";
             OS << B.Succs[I];
           }
           OS << "]\n";
         });
  }
}

void emitMedView(const MedFunc &F, const LowFunc *Low, const IRRowSink &Sink) {
  const auto LowOrigins =
      Low ? instructionOrigins(*Low) : std::set<std::pair<va_t, int>>();
  auto Emit = [&](const IRRowOrigin &Origin, auto Write) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Write(OS);
    Sink(Text, Origin);
  };
  Emit({"med:" + vaHex(F.Entry) + ":header", "header"}, [&](auto &OS) {
    OS << "; MedIR: " << F.Name << " @ " << vaHex(F.Entry) << "\n";
  });
  Emit({"med:" + vaHex(F.Entry) + ":frame", "header"}, [&](auto &OS) {
    OS << "; CC: " << static_cast<int>(F.CC) << " FrameSize: " << F.FrameSize
       << "\n";
  });
  for (const auto &B : F.Blocks) {
    Emit({rowId("med", F.Entry, B.Id, "block"), "block"},
         [&](auto &OS) { OS << "block_" << B.Id << ":\n"; });
    for (size_t Index = 0; Index < B.Phis.size(); ++Index) {
      const auto &Phi = B.Phis[Index];
      Emit({rowId("med", F.Entry, B.Id, "phi", Index), "phi", "synthetic"},
           [&](auto &OS) {
             OS << "  PHI " << Phi.Output.display() << " = [";
             for (size_t I = 0; I < Phi.Args.size(); ++I) {
               if (I)
                 OS << ", ";
               OS << "b" << Phi.Args[I].first << ":"
                  << Phi.Args[I].second.display();
             }
             OS << "]\n";
           });
    }
    for (size_t Index = 0; Index < B.Ops.size(); ++Index) {
      const auto &Op = B.Ops[Index];
      IRRowOrigin Origin{
          rowId("med", F.Entry, B.Id, "op", Index) + ":" + vaHex(Op.Addr) +
              ":seq:" + std::to_string(Op.OriginSeq),
          "operation", Op.OriginSeq < 0 ? "synthetic" : "unmapped"};
      if (Op.OriginSeq >= 0 && LowOrigins.count({Op.Addr, Op.OriginSeq})) {
        Origin.Address = Op.Addr;
        Origin.Sequence = Op.OriginSeq;
        Origin.Status = "instruction_anchor";
      }
      Emit(Origin, [&](auto &OS) {
        OS << "  " << ndOpName(Op.Opcode);
        if (Op.Output.Id >= 0)
          OS << " " << Op.Output.display() << " =";
        for (int I = 0; I < Op.NumInputs; ++I)
          OS << " " << Op.Inputs[I].display();
        OS << "\n";
      });
    }
    Emit({rowId("med", F.Entry, B.Id, "successors"), "successors"},
         [&](auto &OS) {
           OS << "  succs: [";
           for (size_t I = 0; I < B.Succs.size(); ++I) {
             if (I)
               OS << ", ";
             OS << B.Succs[I];
           }
           OS << "]\n";
         });
  }
}
} // namespace

// ===--------------------------------------------------------------------===//
// Single-function decompilation
// ===--------------------------------------------------------------------===//

namespace {
std::optional<std::string> missingARMModeReason(const BinaryImage &Img,
                                                va_t Entry) {
  if (Img.Arch != Arch::ARM || !Img.isCodeAddress(Entry) ||
      Img.instructionModeAt(Entry))
    return std::nullopt;
  return "ARM/Thumb mode cannot be established for function at 0x" +
         llvm::utohexstr(Entry) + "; supply --" + arm_mode_cli::Option +
         arm_mode_cli::HintEntryPrefix + arm_mode_cli::ARM + "|" +
         arm_mode_cli::Thumb;
}

/// Why \p Entry has no HighIR, naming the pipeline's audit disposition when
/// it recorded one (for example a rejected or absorbed candidate).
std::string missingHighFunctionReason(const PipelineResult &Result,
                                      neverd_va_t Entry) {
  for (const PipelineFunctionAudit &Audit : Result.FunctionAudits) {
    if (Audit.Entry != Entry ||
        Audit.Disposition == PipelineFunctionDisposition::Accepted)
      continue;
    std::string Reason = std::string("function not found in HighIR (") +
                         pipelineFunctionDispositionName(Audit.Disposition);
    auto Name = [&](const char *What, const std::vector<va_t> &Addrs) {
      if (Addrs.empty())
        return;
      Reason += std::string("; ") + What + " at";
      for (size_t I = 0; I < Addrs.size() && I < 4; ++I)
        Reason += " 0x" + llvm::utohexstr(Addrs[I]);
      if (Addrs.size() > 4)
        Reason += " ...";
    };
    std::vector<va_t> Unsupported;
    for (va_t Addr : Audit.UnsupportedInstructions)
      if (!llvm::is_contained(Audit.UnprovenReturns, Addr))
        Unsupported.push_back(Addr);
    Name("unsupported instruction", Unsupported);
    Name("return that may pop an address the function pushed",
         Audit.UnprovenReturns);
    Name("decode failure", Audit.DecodeFailures);
    return Reason + ")";
  }
  return "function not found in HighIR";
}
} // namespace

/// The source \p Route emitted for \p Entry under the current pipeline, if it
/// holds what the caller needs: a map when \p SourceMap asks for one.
static const char *cachedSource(Session &S, va_t Entry,
                                Session::SourceRoute Route,
                                CSourceMap *SourceMap) {
  const Session::FunctionSource *Source = S.findFunctionSource(Entry, Route);
  if (!Source || (SourceMap && !Source->Map))
    return nullptr;
  if (SourceMap) {
    *SourceMap = *Source->Map;
    SourceMap->Recognitions = &S.PipeResult.LibraryRecognitions;
  }
  return dupStr(Source->Text);
}

/// Keep what an emission of \p Entry's source produced and return a copy for
/// the caller.  Empty output is a refusal and is not kept.
static const char *keptSource(Session &S, va_t Entry,
                              Session::SourceRoute Route, std::string Text,
                              const CSourceMap *SourceMap) {
  const char *Result = dupStr(Text);
  if (!Text.empty())
    S.rememberFunctionSource(Entry, Route, std::move(Text), SourceMap);
  return Result;
}

static const char *decompileHighC(neverd_session_t Sess, neverd_va_t FuncEntry,
                                  CSourceMap *SourceMap) {
  auto *S = toSession(Sess);
  S->clearError();

  if (auto Reason = missingARMModeReason(S->Img, FuncEntry)) {
    S->setError(*Reason);
    return dupStr(std::string());
  }

  if (S->Img.Format == BinaryFormat::COFF)
    coff_loader::ensureExceptionHandlers(S->Img, {FuncEntry});

  if (!S->PipeRan)
    S->OnlyFunctionEntries.insert(FuncEntry);
  else if (!S->OnlyFunctionEntries.empty() &&
           !S->OnlyFunctionEntries.count(FuncEntry)) {
    S->invalidatePipeline();
    S->OnlyFunctionEntries = {FuncEntry};
  }

  if (!S->ensurePipeline())
    return dupStr(std::string());

  if (S->PipeResult.EVM) {
    auto Output = evm::emitC(*S->PipeResult.EVM);
    if (!Output) {
      S->setError(llvm::toString(Output.takeError()));
      return dupStr(std::string());
    }
    return dupStr(*Output);
  }

  if (S->PipeResult.SBF) {
    auto Output = sbf::emitC(*S->PipeResult.SBF);
    if (!Output) {
      S->setError(llvm::toString(Output.takeError()));
      return dupStr(std::string());
    }
    return dupStr(*Output);
  }

  if (const char *Cached =
          cachedSource(*S, FuncEntry, Session::SourceRoute::HighC, SourceMap))
    return Cached;

  const HighFunc *HF = S->findHighFunc(FuncEntry);
  if (!HF) {
    S->setError(missingHighFunctionReason(S->PipeResult, FuncEntry));
    return dupStr(std::string());
  }

  std::vector<HighFunc> Related = S->PipeResult.HighFuncs;
  attachCxxFuncletBodies(Related);
  const HighFunc *Attached = nullptr;
  for (const HighFunc &Func : Related)
    if (Func.Entry == FuncEntry) {
      Attached = &Func;
      break;
    }
  if (!Attached)
    Attached = HF;

  std::vector<HighFunc> Single = {*Attached};
  std::string Out;
  llvm::raw_string_ostream OS(Out);

  CEmitterOptions Opts;
  Opts.TheArch = S->Img.Arch;
  Opts.Format = S->Img.Format;
  Opts.Image = &S->Img;
  Opts.UserNames = &S->Renames;
  if (SourceMap) {
    SourceMap->Recognitions = &S->PipeResult.LibraryRecognitions;
    SourceMap->HighSources = &S->PipeResult.HighSources;
    Opts.SourceMap = SourceMap;
  }
  HighCEmitter Emitter;
  Emitter.emit(Single, OS, Opts, S->Dbg.get());

  return keptSource(*S, FuncEntry, Session::SourceRoute::HighC, std::move(Out),
                    SourceMap);
}

const char *neverd_decompile(neverd_session_t Sess, neverd_va_t FuncEntry) {
  return decompileHighC(Sess, FuncEntry, nullptr);
}

const char *neverd_decompile_llvm(neverd_session_t Sess,
                                  neverd_va_t FuncEntry) {
  return neverd_decompile_llvm_ex(Sess, FuncEntry, 0);
}

static const char *decompileLlvmC(neverd_session_t Sess, neverd_va_t FuncEntry,
                                  int NoOpt, CSourceMap *SourceMap) {
  auto *S = toSession(Sess);
  S->clearError();

  if (auto Reason = missingARMModeReason(S->Img, FuncEntry)) {
    S->setError(*Reason);
    return dupStr(std::string());
  }

  if (S->Img.Format == BinaryFormat::COFF)
    coff_loader::ensureExceptionHandlers(S->Img, {FuncEntry});

  if (!S->PipeRan)
    S->OnlyFunctionEntries.insert(FuncEntry);
  else if (!S->OnlyFunctionEntries.empty() &&
           !S->OnlyFunctionEntries.count(FuncEntry)) {
    S->invalidatePipeline();
    S->OnlyFunctionEntries = {FuncEntry};
  }

  if (!S->ensurePipeline())
    return dupStr(std::string());

  if (S->PipeResult.EVM) {
    S->setError("LLVM-to-C route is not supported for EVM; use the "
                "dedicated C backend");
    return dupStr(std::string());
  }
  if (S->PipeResult.SBF) {
    S->setError("LLVM-to-C route is not supported for SBF; use the "
                "dedicated C or Rust backend");
    return dupStr(std::string());
  }

  const auto Route =
      NoOpt ? Session::SourceRoute::LLVMCNoOpt : Session::SourceRoute::LLVMC;
  if (const char *Cached = cachedSource(*S, FuncEntry, Route, SourceMap))
    return Cached;

  const auto *Native = S->ensureFunctionLlvmModule(FuncEntry, NoOpt != 0);
  if (!Native)
    return dupStr(std::string());
  llvm::Function *LF = S->findNativeLlvmFunction(*Native->Module, FuncEntry);
  if (!LF)
    return dupStr(std::string());

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  CEmitterOptions Opts;
  Opts.TheArch = S->Img.Arch;
  Opts.Format = S->Img.Format;
  Opts.UserNames = &S->Renames;
  if (SourceMap) {
    SourceMap->Recognitions = &S->PipeResult.LibraryRecognitions;
    SourceMap->LLVMSources = Native->Sources.get();
    Opts.SourceMap = SourceMap;
  }
  LLVMCEmitter Emitter;
  Emitter.emit(*Native->Module, OS, Opts, S->Dbg.get(), &S->Img, LF);
  return keptSource(*S, FuncEntry, Route, std::move(Out), SourceMap);
}

const char *neverd_decompile_llvm_ex(neverd_session_t Sess,
                                     neverd_va_t FuncEntry, int NoOpt) {
  return decompileLlvmC(Sess, FuncEntry, NoOpt, nullptr);
}

namespace {

llvm::json::Object sourcePage(neverd_session_t Sess, va_t Entry,
                              llvm::StringRef Stage, size_t Offset,
                              size_t Limit) {
  auto &S = *toSession(Sess);
  CSourceMap Map;
  const char *Raw = Stage == "c" ? decompileHighC(Sess, Entry, &Map)
                                 : decompileLlvmC(Sess, Entry, 0, &Map);
  std::unique_ptr<const char, decltype(&neverd_free_string)> Owned(
      Raw, neverd_free_string);
  if (!Raw || !S.LastError.empty())
    throw std::runtime_error(S.LastError.empty() ? "source emission failed"
                                                 : S.LastError);
  (void)S.synchronizeFunctions();
  llvm::StringRef Full(Raw);
  if (Full.size() > 32 * 1024 * 1024 || !llvm::json::isUTF8(Full))
    throw std::length_error("source view exceeds the UTF-8/32 MiB budget");
  llvm::json::Array Regions;
  for (const auto &Region : Map.Regions) {
    const auto &Match = (*Map.Recognitions)[Region.Recognition];
    if (Match.Function != Entry)
      continue;
    std::string ID = vaHex(Entry) + ':' + Match.Pack + ':' + Match.Rule;
    if (Match.ResultOccurrence)
      ID += ':' + vaHex(Match.ResultOccurrence->Address) + '.' +
            std::to_string(Match.ResultOccurrence->Sequence);
    auto Item = libraryRecognitionJSON(Match);
    Item["id"] = ID;
    Item["function"] = vaHex(Entry);
    Item["foldable"] = Region.Mapped && Match.Isolated;
    Item["mapping_status"] = Region.Mapped ? "mapped" : "unknown";
    if (Match.Callee)
      Item["callee_identity"] = S.functionIdentity(*Match.Callee);
    llvm::json::Array Spans;
    for (const auto &Span : Region.Spans)
      Spans.push_back(
          llvm::json::Object{{"begin_byte", static_cast<int64_t>(Span.Begin)},
                             {"end_byte", static_cast<int64_t>(Span.End)}});
    Item["spans"] = std::move(Spans);
    Regions.push_back(std::move(Item));
  }
  std::string Text;
  llvm::json::Array Rows;
  size_t Total = 0, Start = 0, ByteOffset = Full.size();
  while (Start < Full.size()) {
    const size_t Newline = Full.find('\n', Start);
    const size_t End =
        Newline == llvm::StringRef::npos ? Full.size() : Newline + 1;
    if (Total >= Offset && Total - Offset < Limit) {
      if (Text.empty())
        ByteOffset = Start;
      if (End - Start > 2 * 1024 * 1024 - Text.size())
        throw std::length_error(
            "source page exceeds 2 MiB; request fewer lines");
      Text.append(Full.data() + Start, End - Start);
      std::set<va_t> Origins;
      for (const auto &Region : Map.Regions) {
        const auto &Match = (*Map.Recognitions)[Region.Recognition];
        if (Match.Function != Entry || !Region.Mapped)
          continue;
        for (const auto &Span : Region.Spans)
          if (Span.Begin < End && Start < Span.End)
            for (const auto &Origin : Match.Occurrences)
              Origins.insert(Origin.Address);
      }
      llvm::json::Array Addresses;
      for (va_t Address : Origins)
        Addresses.push_back(vaHex(Address));
      Rows.push_back(llvm::json::Object{
          {"line", static_cast<int64_t>(Total)},
          {"object_id",
           (Stage + ":" + vaHex(Entry) + ":line:" + std::to_string(Total))
               .str()},
          {"kind", "source"},
          {"mapping_status", Origins.empty() ? "unmapped" : "library_region"},
          {"addresses", std::move(Addresses)}});
    }
    ++Total;
    Start = End;
  }
  const size_t End = std::min(Offset, Total) + Rows.size();
  llvm::json::Object Page{
      {"schema_version", 1},
      {"address", vaHex(Entry)},
      {"representation", Stage},
      {"mapping_status", "library_regions"},
      {"provenance_complete", false},
      {"text", std::move(Text)},
      {"rows", std::move(Rows)},
      {"library_regions", std::move(Regions)},
      {"function_identity", S.functionIdentity(Entry)},
      {"offset", static_cast<int64_t>(Offset)},
      {"byte_offset", static_cast<int64_t>(ByteOffset)},
      {"total_lines", static_cast<int64_t>(Total)},
      {"complete", End == Total},
      {"recognition_budget_exhausted",
       S.PipeResult.LibraryRecognitionBudgetExhausted.contains(Entry)}};
  Page["next_offset"] = End == Total
                            ? llvm::json::Value(nullptr)
                            : llvm::json::Value(static_cast<int64_t>(End));
  // The lines before the function's definition: includes, support types and
  // declarations. Published only where the emitter recorded the definition
  // at the start of a line.
  if (Map.Definitions.size() == 1) {
    const size_t Begin = Map.Definitions.front().Begin;
    if (Begin > 0 && Begin < Full.size() && Full[Begin - 1] == '\n')
      Page["prelude"] = llvm::json::Object{
          {"lines", static_cast<int64_t>(Full.take_front(Begin).count('\n'))},
          {"end_byte", static_cast<int64_t>(Begin)}};
  }
  return Page;
}

} // namespace

// ===--------------------------------------------------------------------===//
// Multi-stage IR
// ===--------------------------------------------------------------------===//

const char *neverd_ir_low(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpLowIR(S->PipeResult.EVM->Low));
  if (S->PipeResult.SBF)
    return dupStr(sbf::dumpLowIR(S->PipeResult.SBF->Low));

  const LowFunc *F = S->findLowFunc(FuncEntry);
  if (!F) {
    S->setError("function not found in LowIR");
    return dupStr(std::string());
  }

  std::string Out;
  emitLowView(*F, [&](llvm::StringRef Text, const IRRowOrigin &) {
    Out.append(Text.data(), Text.size());
  });

  return dupStr(Out);
}

const char *neverd_ir_med(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpMedIR(S->PipeResult.EVM->Med));
  if (S->PipeResult.SBF)
    return dupStr(sbf::dumpMedIR(S->PipeResult.SBF->Med));

  for (const auto &F : S->PipeResult.MedFuncs) {
    if (F.Entry != FuncEntry)
      continue;

    std::string Out;
    emitMedView(F, nullptr, [&](llvm::StringRef Text, const IRRowOrigin &) {
      Out.append(Text.data(), Text.size());
    });
    return dupStr(Out);
  }

  S->setError("function not found in MedIR");
  return dupStr(std::string());
}

const char *neverd_ir_view_json(neverd_session_t Sess, neverd_va_t FuncEntry,
                                const char *Representation, size_t Offset,
                                size_t Limit) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    if (!Representation || Limit == 0 || Limit > 2048 ||
        Offset > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
      S->setError("invalid IR view representation or paging bounds");
      return nullptr;
    }
    const llvm::StringRef Stage(Representation);
    if (!llvm::json::isUTF8(Stage)) {
      S->setError("IR view representation is not valid UTF-8");
      return nullptr;
    }
    llvm::json::Object Result;
    Result["schema_version"] = 1;
    Result["address"] = vaHex(FuncEntry);
    Result["representation"] = Stage;
    Result["rows"] = llvm::json::Array();
    if (Stage == "c" || Stage == "llvmc") {
      if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF) {
        Result["mapping_status"] = "unsupported_architecture";
        return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
      }
      auto Page = sourcePage(Sess, FuncEntry, Stage, Offset, Limit);
      return dupStr(jsonToString(llvm::json::Value(std::move(Page))));
    }
    if (Stage != "low" && Stage != "med") {
      Result["mapping_status"] = "unsupported_representation";
      return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
    }
    if (!S->ensurePipeline())
      return nullptr;
    if (S->PipeResult.EVM || S->PipeResult.SBF) {
      Result["mapping_status"] = "unsupported_architecture";
      return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
    }
    const LowFunc *Low = S->findLowFunc(FuncEntry);
    const MedFunc *Med = nullptr;
    if (Stage == "med")
      for (const auto &F : S->PipeResult.MedFuncs)
        if (F.Entry == FuncEntry) {
          Med = &F;
          break;
        }
    if ((Stage == "low" && !Low) || (Stage == "med" && !Med)) {
      S->setError("function not found in requested IR stage");
      return nullptr;
    }
    size_t Total = 0;
    std::string Text;
    llvm::json::Array Rows;
    const IRRowSink Sink = [&](llvm::StringRef Chunk,
                               const IRRowOrigin &Origin) {
      size_t Start = 0;
      while (Start < Chunk.size()) {
        const auto Newline = Chunk.find('\n', Start);
        const auto End =
            Newline == llvm::StringRef::npos ? Chunk.size() : Newline + 1;
        if (Total >= Offset && Total - Offset < Limit) {
          if (End - Start > 2 * 1024 * 1024 - Text.size())
            throw std::length_error(
                "IR view page exceeds the 2 MiB budget; request fewer lines");
          Text.append(Chunk.data() + Start, End - Start);
          llvm::json::Object Row;
          Row["line"] = static_cast<int64_t>(Total);
          Row["object_id"] = Origin.ObjectId;
          Row["kind"] = Origin.Kind;
          Row["mapping_status"] = Origin.Status;
          llvm::json::Array Addresses;
          if (Origin.Address)
            Addresses.push_back(vaHex(*Origin.Address));
          Row["addresses"] = std::move(Addresses);
          if (Origin.Sequence >= 0)
            Row["origin_seq"] = Origin.Sequence;
          Rows.push_back(std::move(Row));
        }
        ++Total;
        Start = End;
      }
    };
    if (Stage == "low")
      emitLowView(*Low, Sink, true);
    else
      emitMedView(*Med, Low, Sink);
    if (!llvm::json::isUTF8(Text)) {
      S->setError("IR view page text is not valid UTF-8");
      return nullptr;
    }
    const size_t End = std::min(Offset, Total) + Rows.size();
    Result["mapping_status"] = "instruction_anchors";
    Result["provenance_complete"] = false;
    Result["text"] = std::move(Text);
    Result["rows"] = std::move(Rows);
    Result["offset"] = static_cast<int64_t>(Offset);
    Result["total_lines"] = static_cast<int64_t>(Total);
    Result["complete"] = End == Total;
    if (End == Total)
      Result["next_offset"] = nullptr;
    else
      Result["next_offset"] = static_cast<int64_t>(End);
    return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
  } catch (const std::exception &Error) {
    S->setError(Error.what());
    return nullptr;
  } catch (...) {
    S->setError("unexpected IR view failure");
    return nullptr;
  }
}

const char *neverd_ir_high(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpHighIR(S->PipeResult.EVM->High));
  if (S->PipeResult.SBF)
    return dupStr(sbf::dumpHighIR(S->PipeResult.SBF->High));

  const HighFunc *HF = S->findHighFunc(FuncEntry);
  if (!HF) {
    S->setError(missingHighFunctionReason(S->PipeResult, FuncEntry));
    return dupStr(std::string());
  }

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << "; HighIR: " << HF->Name << " @ " << vaHex(HF->Entry) << "\n";
  OS << "; Params: " << HF->Params.size() << ", Locals: " << HF->Locals.size()
     << "\n\n";
  for (const auto &Stmt : HF->Body)
    OS << Stmt.str(0) << "\n";

  return dupStr(Out);
}

const char *neverd_ir_llvm(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());
  if (S->PipeResult.EVM || S->PipeResult.SBF) {
    if (!S->ensureLlvmModule()) {
      if (S->LastError.empty())
        S->setError("failed to generate LLVM module");
      return dupStr(std::string());
    }
    return dupStr(S->PipeResult.EVM
                      ? evm::emitLLVMText(*S->PipeResult.LlvmModule)
                      : sbf::emitLLVMText(*S->PipeResult.LlvmModule));
  }

  const auto *Native = S->ensureFunctionLlvmModule(FuncEntry, false);
  if (!Native) {
    if (S->LastError.empty())
      S->setError("failed to generate LLVM module");
    return dupStr(std::string());
  }
  llvm::Function *LF = S->findNativeLlvmFunction(*Native->Module, FuncEntry);
  if (!LF)
    return dupStr(std::string());

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  LF->print(OS);

  return dupStr(Out);
}

// ===--------------------------------------------------------------------===//
// High-level pipeline: lift module
// ===--------------------------------------------------------------------===//

const char *neverd_lift_module(neverd_session_t Sess, const char *InputPath,
                               int NoOpt, int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.load(InputPath, Err, S)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  PipelineOptions Opts;
  Opts.LiftMode = true;
  Opts.NoOpt = NoOpt != 0;
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (S) {
    S->applyAnalysisOptions(Opts);
  }
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }
  if (!R.Result.LlvmModule)
    return nullptr;

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  R.Result.LlvmModule->print(OS, nullptr);
  return dupStr(Buf);
}

// ===--------------------------------------------------------------------===//
// High-level pipeline: lift dump (LowIR / MedIR / HighIR)
// ===--------------------------------------------------------------------===//

const char *neverd_lift_dump(neverd_session_t Sess, const char *InputPath,
                             int Level, int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.load(InputPath, Err, S)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  PipelineOptions Opts;
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (S) {
    S->applyAnalysisOptions(Opts);
  }
  if (Level == 0)
    Opts.DumpLow = true;
  else if (Level == 1)
    Opts.DumpMed = true;
  else
    Opts.DumpHigh = true;
  // The C API returns the dump as a string. Printing it from inside the shared
  // library as well gives Windows DLL and executable instances of llvm::outs()
  // independent buffers whose flush order is not deterministic.
  Opts.EmitDumpOutput = false;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  if (R.Result.EVM) {
    if (Level == 0)
      OS << evm::dumpLowIR(R.Result.EVM->Low);
    else if (Level == 1)
      OS << evm::dumpMedIR(R.Result.EVM->Med);
    else
      OS << evm::dumpHighIR(R.Result.EVM->High);
  } else if (R.Result.SBF) {
    if (Level == 0)
      OS << sbf::dumpLowIR(R.Result.SBF->Low);
    else if (Level == 1)
      OS << sbf::dumpMedIR(R.Result.SBF->Med);
    else
      OS << sbf::dumpHighIR(R.Result.SBF->High);
  } else if (Level == 0)
    Pipeline::dumpLowIR(R.Result.LowFuncs, OS);
  else if (Level == 1)
    Pipeline::dumpMedIR(R.Result.MedFuncs, OS);
  else
    Pipeline::dumpHighIR(R.Result.HighFuncs, OS);
  return dupStr(Buf);
}

// ===--------------------------------------------------------------------===//
// High-level pipeline: decompile all
// ===--------------------------------------------------------------------===//

static const char *decompileAllImpl(neverd_session_t Sess,
                                    const char *InputPath, int UseLlvmRoute,
                                    neverd_output_language_t Language,
                                    int NoOpt, int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.load(InputPath, Err, S)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  PipelineOptions Opts;
  Opts.NoOpt = NoOpt != 0;
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (S) {
    S->applyAnalysisOptions(Opts);
  }
  if (UseLlvmRoute)
    Opts.LiftMode = true;
  Opts.SourceProjection = true;
  // A named ARM function with no instruction-state evidence is not a
  // candidate for either C route. Reject the whole requested output before
  // analysis so a partly marked image cannot silently omit that function.
  if (R.Img.Arch == Arch::ARM) {
    for (const Symbol &Sym : R.Img.Symbols) {
      if (!Sym.IsFunc || R.Img.isImportStubAt(Sym.Addr) ||
          (!Opts.OnlyFunctionEntries.empty() &&
           !Opts.OnlyFunctionEntries.count(Sym.Addr)))
        continue;
      if (auto Reason = missingARMModeReason(R.Img, Sym.Addr)) {
        if (S)
          S->setError(*Reason);
        return nullptr;
      }
    }
  }
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);

  if (R.Result.EVM) {
    if (UseLlvmRoute) {
      if (S)
        S->setError("LLVM-to-C route is not supported for EVM; use the "
                    "dedicated C backend");
      return nullptr;
    }
    if (Language == NEVERD_OUTPUT_SOLIDITY) {
      auto Output = evm::emitSolidity(*R.Result.EVM);
      if (!Output) {
        if (S)
          S->setError(llvm::toString(Output.takeError()));
        return nullptr;
      }
      return dupStr(*Output);
    }
    if (Language == NEVERD_OUTPUT_RUST) {
      if (S)
        S->setError("Rust output is supported only for Solana SBF programs");
      return nullptr;
    }
    auto Output = evm::emitC(*R.Result.EVM);
    if (!Output) {
      if (S)
        S->setError(llvm::toString(Output.takeError()));
      return nullptr;
    }
    return dupStr(*Output);
  }

  if (R.Result.SBF) {
    if (UseLlvmRoute) {
      if (S)
        S->setError("LLVM-to-C route is not supported for SBF; use the "
                    "dedicated C or Rust backend");
      return nullptr;
    }
    if (Language == NEVERD_OUTPUT_SOLIDITY) {
      if (S)
        S->setError("Solidity output is supported only for EVM bytecode");
      return nullptr;
    }
    if (Language == NEVERD_OUTPUT_RUST) {
      auto Output = sbf::emitRust(*R.Result.SBF);
      if (!Output) {
        if (S)
          S->setError(llvm::toString(Output.takeError()));
        return nullptr;
      }
      return dupStr(*Output);
    }
    auto Output = sbf::emitC(*R.Result.SBF);
    if (!Output) {
      if (S)
        S->setError(llvm::toString(Output.takeError()));
      return nullptr;
    }
    return dupStr(*Output);
  }

  if (Language == NEVERD_OUTPUT_SOLIDITY || Language == NEVERD_OUTPUT_RUST) {
    if (S)
      S->setError(
          Language == NEVERD_OUTPUT_SOLIDITY
              ? "Solidity output is supported only for EVM bytecode"
              : "Rust output is supported only for Solana SBF programs");
    return nullptr;
  }

  if (UseLlvmRoute) {
    if (!R.Result.LlvmModule)
      return nullptr;
    CEmitterOptions COpts;
    COpts.TheArch = R.Img.Arch;
    COpts.Format = R.Img.Format;
    COpts.UserNames = S ? &S->Renames : nullptr;
    LLVMCEmitter Emitter;
    Emitter.emit(*R.Result.LlvmModule, OS, COpts, R.Dbg.get(), &R.Img);
  } else {
    CEmitterOptions COpts;
    COpts.TheArch = R.Img.Arch;
    COpts.Format = R.Img.Format;
    COpts.Image = &R.Img;
    COpts.UserNames = S ? &S->Renames : nullptr;
    HighCEmitter Emitter;
    Emitter.emit(R.Result.HighFuncs, OS, COpts, R.Dbg.get());
  }
  return dupStr(Buf);
}

const char *neverd_decompile_all(neverd_session_t Sess, const char *InputPath,
                                 int UseLlvmRoute, int NoOpt,
                                 int MaxFunctions) {
  return decompileAllImpl(Sess, InputPath, UseLlvmRoute, NEVERD_OUTPUT_C, NoOpt,
                          MaxFunctions);
}

const char *neverd_decompile_all_ex(neverd_session_t Sess,
                                    const char *InputPath,
                                    neverd_output_language_t Language,
                                    int NoOpt, int MaxFunctions) {
  const bool KnownLanguage = [&] {
    switch (Language) {
#define NEVERD_OUTPUT_LANGUAGE(NAME, VALUE, SPELLING, DISPLAY_NAME)            \
  case NEVERD_OUTPUT_##NAME:                                                   \
    return true;
#include "neverd/OutputLanguages.def"
    }
    return false;
  }();
  if (!KnownLanguage) {
    if (auto *S = static_cast<Session *>(Sess))
      S->setError("unknown output language");
    return nullptr;
  }
  return decompileAllImpl(Sess, InputPath, 0, Language, NoOpt, MaxFunctions);
}

// ===--------------------------------------------------------------------===//
// Inject hello world pass
// ===--------------------------------------------------------------------===//

int neverd_inject_hello(neverd_session_t Sess) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->PipeResult.LlvmModule) {
    if (S)
      S->setError("no LLVM module available");
    return 1;
  }
  Pipeline::runHelloWorldPass(*S->PipeResult.LlvmModule);
  return 0;
}
