//===- ComparisonDataFlow.cpp - Bounded per-function SSA capture ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Use the existing native pipeline and verifier. Detached dependency records
/// outlive its contexts; no input program is executed. Pipeline work currently
/// stops between selected functions, with a native instruction admission bound.
//===----------------------------------------------------------------------===//

#include "BinaryComparison.h"
#include "SessionImpl.h"

#include "neverd/Common.h"
#include "neverd/diff/MedDataFlow.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::sdk {
using namespace diff;

std::shared_ptr<const DataFlowEvidence>
readComparisonDataFlow(Session &S, const Program &P,
                       const DataFlowSettings &Settings,
                       const ComparisonControl &Control) {
  if (auto Error = Settings.validate())
    throw std::invalid_argument(*Error);
  if (!Settings.Enabled)
    return {};
  auto Evidence = std::make_shared<DataFlowEvidence>();
  Evidence->Recipe = std::string(VersionString) + ":native-" +
                     DataFlowProfile.str() + ":" + Settings.describe();
  Evidence->EligibleFunctions = llvm::count_if(
      P.Functions, [&](const auto &F) { return P.instructionCount(F) != 0; });
  uint64_t TotalNodes = 0;
  // Shared read-only image facts may be reused between bounded function jobs.
  PipelineAnalysisCache Cache;
  for (const auto &F : P.Functions) {
    const auto Instructions = P.instructionCount(F);
    if (!Instructions)
      continue;
    if (auto Stop = Control.stop()) {
      Evidence->State = *Stop;
      break;
    }
    if (Evidence->Entries.size() == Settings.MaxFunctions ||
        TotalNodes >= Settings.MaxTotalNodes) {
      Evidence->State = ComparisonState::ResourceLimited;
      break;
    }
    if (Control.Progress)
      Control.Progress("dataflow", Evidence->Entries.size(),
                       Evidence->EligibleFunctions);
    if (auto Stop = Control.stop()) {
      Evidence->State = *Stop;
      break;
    }
    DataFlowEntry Entry;
    Entry.Function = F.Address;
    if (Instructions > Settings.MaxNativeInstructions) {
      Entry.Availability = FlowAvailability::ResourceLimited;
      Entry.Reason = "native instruction admission limit exceeded";
    } else if (F.Traits & TraitIncomplete) {
      Entry.Reason = "native instruction/control-flow coverage is incomplete";
    } else {
      try {
        llvm::LLVMContext Context;
        PipelineOptions Options;
        S.applyAnalysisOptions(Options);
        Options.OnlyFunctionEntries = {F.Address};
        Options.MedAnalysisOnly = true;
        Options.EmitDumpOutput = false;
        Options.AnalysisCache = &Cache;
        auto Result =
            Pipeline().run(S.Img, Context, Options, S.decompileDebug());
        if (auto Stop = Control.stop())
          throw DataFlowInterrupted(*Stop);
        const auto Audit =
            llvm::find_if(Result.FunctionAudits,
                          [&](const auto &A) { return A.Entry == F.Address; });
        const auto Function =
            llvm::find_if(Result.MedFuncs, [&](const auto &MF) {
              return MF.Entry == F.Address;
            });
        if (!Result.Success) {
          Entry.Availability = FlowAvailability::Failed;
          Entry.Reason = Result.Error.empty() ? "native SSA pipeline failed"
                                              : Result.Error;
        } else if (Audit == Result.FunctionAudits.end() ||
                   !Audit->MedIRVerified || !Audit->DecodeFailures.empty() ||
                   !Audit->UnsupportedInstructions.empty() ||
                   !Audit->TruncatedPaths.empty() ||
                   !Audit->UnprovenReturns.empty() ||
                   Audit->LiftedInstructions != Audit->DecodedInstructions ||
                   Function == Result.MedFuncs.end() || Function->SkippedSSA) {
          Entry.Reason =
              "pipeline did not produce a fully verified native SSA body";
        } else {
          auto GraphOptions = Settings.Graph;
          GraphOptions.MaxNodes = std::min<uint64_t>(
              GraphOptions.MaxNodes, Settings.MaxTotalNodes - TotalNodes);
          Entry.Graph =
              buildMedDataFlow(*Function, P.Identity.Revision, P.Identity.ABI,
                               true, GraphOptions, Control);
          TotalNodes += Entry.Graph->Nodes.size();
          Entry.Availability = FlowAvailability::Available;
        }
      } catch (const DataFlowInterrupted &Stop) {
        if (Stop.State != ComparisonState::ResourceLimited) {
          Evidence->State = Stop.State;
          break;
        }
        Entry.Availability = FlowAvailability::ResourceLimited;
        Entry.Reason = Stop.what();
      } catch (const std::exception &Error) {
        Entry.Availability = FlowAvailability::Failed;
        Entry.Reason = Error.what();
      }
    }
    Entry.Reason.resize(std::min<size_t>(Entry.Reason.size(),
                                         limits::diff::kMaxFlowReasonBytes));
    if (Entry.Availability == FlowAvailability::ResourceLimited)
      Evidence->State = ComparisonState::ResourceLimited;
    Evidence->Entries.push_back(std::move(Entry));
  }
  if (Control.Progress)
    Control.Progress("dataflow", Evidence->Entries.size(),
                     Evidence->EligibleFunctions);
  if (auto Error = validateDataFlowEvidence(Evidence.get(), Settings, P))
    throw std::invalid_argument(*Error);
  return Evidence;
}
} // namespace neverd::sdk
