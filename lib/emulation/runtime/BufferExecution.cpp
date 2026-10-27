//===- BufferExecution.cpp - Explicit raw CPU workload --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit raw CPU workload.
///

#include "ExecutionJSONParsing.h"

#include "neverd/emulation/ExecutionReport.h"
#include "neverd/emulation/ExecutionRunFactory.h"
#include "neverd/emulation/Limits.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation {
namespace {
namespace j = llvm::json;
using namespace execution_json;
struct BufferInput {
  ExecutionConfiguration CPU;
  uint64_t MemoryLimit = execution_limit::TransferBytes;
  uint64_t Entry = 0, StopPC = 0, StackPointer = 0;
  std::vector<ImageMapping> Regions;
  std::optional<GuestInvocationPlan> Invocation;
};
llvm::Expected<BufferInput> decode(llvm::StringRef JSON) {
  auto V = parse(JSON);
  if (!V)
    return V.takeError();
  const auto *O = V->getAsObject();
  if (!O)
    return invalid("buffer environment must be an object");
  if (auto E = keys(*O, {"kind", "cpu", "memory_limit", "entry", "stop_pc",
                         "stack_pointer", "regions", "invocation"}))
    return std::move(E);
  if (O->getString("kind") != "buffer")
    return invalid("buffer factory requires kind=buffer");
  BufferInput R;
  if (const auto *C = O->get("cpu")) {
    auto CPU = executionConfigurationFromJSON(llvm::formatv("{0}", *C).str());
    if (!CPU)
      return CPU.takeError();
    R.CPU = *CPU;
  }
  if (auto E = positive(*O, "memory_limit", R.MemoryLimit,
                        execution_limit::TransferBytes))
    return std::move(E);
  for (auto [Name, Field] : {std::pair{"entry", &R.Entry},
                             {"stop_pc", &R.StopPC},
                             {"stack_pointer", &R.StackPointer}}) {
    auto N = number(O->get(Name), Name);
    if (!N)
      return N.takeError();
    *Field = *N;
  }
  const auto *A = O->getArray("regions");
  if (!A || A->empty() ||
      A->size() > ::neverd::limits::emulation::kBufferExecutionDecodeASizeLimit)
    return invalid("buffer regions must contain 1..256 explicit RAM regions");
  uint64_t Total = 0;
  for (const auto &Row : *A) {
    const auto *B = Row.getAsObject();
    if (!B)
      return invalid("buffer RAM region must be an object");
    if (auto E = keys(*B, {"address", "size", "permissions", "bytes_hex"}))
      return std::move(E);
    auto Address = number(B->get("address"), "region address");
    if (!Address)
      return Address.takeError();
    auto Size = number(B->get("size"), "region size");
    if (!Size)
      return Size.takeError();
    if (!*Size || *Size % 4096 || *Address % 4096 ||
        *Size > UINT64_MAX - *Address || *Size > R.MemoryLimit - Total)
      return invalid("invalid or excessive page-aligned buffer region");
    Total += *Size;
    auto Permissions = B->getString("permissions");
    if (!Permissions || Permissions->empty())
      return invalid("region requires explicit permissions");
    unsigned Mask = 0;
    for (char C : *Permissions) {
      const unsigned Bit = C == 'r'   ? Read
                           : C == 'w' ? Write
                           : C == 'x' ? Execute
                           : C == 'u' ? UserAccessible
                                      : 0;
      if (!Bit || (Mask & Bit))
        return invalid(
            "region permissions must contain unique r/w/x/u letters");
      Mask |= Bit;
    }
    std::vector<uint8_t> Bytes(*Size, 0);
    if (const auto *H = B->get("bytes_hex")) {
      auto Hex = H->getAsString();
      if (!Hex || Hex->size() % 2 || Hex->size() / 2 > *Size ||
          !llvm::all_of(*Hex, llvm::isHexDigit))
        return invalid("region bytes_hex exceeds its bounds or is invalid");
      auto Input = llvm::fromHex(*Hex);
      std::copy(Input.begin(), Input.end(), Bytes.begin());
    }
    R.Regions.push_back({*Address, Mask, std::move(Bytes)});
  }
  if (const auto *I = O->get("invocation")) {
    const auto *P = I->getAsObject();
    if (!P)
      return invalid("buffer invocation plan must be an object");
    if (auto E = keys(*P, {"stack_base", "stack_size", "return_pc"}))
      return std::move(E);
    GuestInvocationPlan Plan;
    for (auto [Name, Field] : {std::pair{"stack_base", &Plan.StackBase},
                               {"stack_size", &Plan.StackSize},
                               {"return_pc", &Plan.ReturnPC}}) {
      auto N = number(P->get(Name), Name);
      if (!N)
        return N.takeError();
      *Field = *N;
    }
    const auto Region = llvm::find_if(R.Regions, [&](const auto &B) {
      return B.Address == Plan.StackBase && B.Bytes.size() == Plan.StackSize &&
             (B.Permissions & (Read | Write)) == (Read | Write) &&
             !(B.Permissions & Execute);
    });
    const auto InStack = [&](uint64_t PC) {
      return PC >= Plan.StackBase && PC - Plan.StackBase < Plan.StackSize;
    };
    if (Region == R.Regions.end() || InStack(R.Entry) || InStack(R.StopPC) ||
        InStack(R.StackPointer) || InStack(Plan.ReturnPC))
      return invalid(
          "invocation requires a dedicated private RAM stack region");
    R.Invocation = Plan;
  }
  return R;
}
class BufferEnvironment final : public ExecutionEnvironment {
public:
  explicit BufferEnvironment(BufferInput Input)
      : Input(std::move(Input)), PC(this->Input.Entry) {}
  std::string name() const override { return "buffer"; }
  std::vector<std::string> stageNames() const override { return {"stopped"}; }
  SessionModelHooks modelHooks() override {
    SessionModelHooks H;
    H.BeforeInstruction = [this](uint64_t Address, uint32_t) {
      return !CallPC && Address == Input.StopPC ? InstructionAdmission::Stop
                                                : InstructionAdmission::Execute;
    };
    return H;
  }
  llvm::Error initialize(ExecutionSession &S, ModuleBindings &) override {
    std::vector<InitializedMapping> Mappings;
    for (const auto &R : Input.Regions)
      Mappings.push_back({R.Address, R.Permissions, R.Bytes});
    auto Transaction = S.cpu().addressSpace()->prepareMappings(Mappings);
    if (!Transaction)
      return Transaction.takeError();
    if (auto E = (*Transaction)->commit())
      return E;
    Transaction->reset();
    Input.Regions.clear();
    if (auto E = S.cpu().architecture() == GuestArchitecture::X64
                     ? S.cpu().setReg(X64Register::SP, Input.StackPointer)
                     : S.cpu().setReg(AArch64Register::SP, Input.StackPointer))
      return E;
    auto Stop = S.cpu().addressSpace()->canAccess(Input.StopPC, 1, Execute);
    if (!Stop)
      return Stop.takeError();
    if (!*Stop)
      return invalid("buffer stop_pc requires mapped executable RAM");
    return llvm::Error::success();
  }
  llvm::Expected<EnvironmentAction> next(ExecutionSession &,
                                         ModuleBindings &) override {
    if (CallPC)
      return EnvironmentAction{
          EnvironmentActionKind::RunCPU, *CallPC, "invocation", {}};
    if (ReachedStop) {
      if (!StageSent) {
        StageSent = true;
        return EnvironmentAction{
            EnvironmentActionKind::StageBoundary, 0, "stopped", {}};
      }
      return EnvironmentAction{
          EnvironmentActionKind::Completed, 0, "finished", {}};
    }
    return EnvironmentAction{
        EnvironmentActionKind::RunCPU, PC, "executing", {}};
  }
  llvm::Error afterCPUExit(ExecutionSession &S,
                           const SessionExit &Exit) override {
    auto Next = S.cpu().architecture() == GuestArchitecture::X64
                    ? S.cpu().reg(X64Register::PC)
                    : S.cpu().reg(AArch64Register::PC);
    if (!Next)
      return Next.takeError();
    if (CallPC) {
      *CallPC = *Next;
    } else {
      PC = *Next;
      if (Exit.Kind == SessionExitKind::ModelBoundary && PC == Input.StopPC)
        ReachedStop = true;
    }
    return llvm::Error::success();
  }
  bool admitResourcePause(const ResourcePause &) const override { return true; }
  llvm::Expected<ModuleMappingPlan>
  buildModule(const ModuleBinding &B, const modules::ModuleImage &M) override {
    const auto &I = M.image();
    const auto Arch = Input.CPU.Architecture == GuestArchitecture::X64
                          ? neverd::Arch::X64
                          : neverd::Arch::AArch64;
    if (I.Arch != Arch || I.IsRelocatable || !I.LoadsAtLinkAddress ||
        !I.Imports.empty() || !I.Relocations.empty() ||
        !I.BaseRelocations.empty() || I.MachOHasChainedFixups ||
        (B.Base && *B.Base != I.Base))
      return invalid("buffer environment only maps fixed link-address images "
                     "without imports/relocations");
    auto Plan = ImageMappingPlan::create(I, 0, 4096, Input.MemoryLimit, false,
                                         ImagePagePadding::Zero,
                                         ImageByteSource::OriginalFile);
    if (!Plan)
      return Plan.takeError();
    uint64_t End = Plan->Base;
    for (const auto &R : Plan->Regions)
      End = std::max(End, R.Address + R.Bytes.size());
    return ModuleMappingPlan{std::move(*Plan), End - I.Base, {}};
  }
  llvm::Expected<GuestInvocationPlan>
  planInvocation(ExecutionSession &, ModuleBindings &,
                 const GuestInvocationRequest &, uint64_t,
                 llvm::StringRef Stage) override {
    if (!Input.Invocation || Stage != "stopped" || !ReachedStop || CallPC)
      return invalid("buffer invocation requires the stopped stage and a "
                     "private call plan");
    return *Input.Invocation;
  }
  llvm::Error beginInvocation(ExecutionSession &,
                              const GuestInvocationFrame &F) override {
    CallPC = F.Entry;
    return llvm::Error::success();
  }
  llvm::Error finishInvocation(ExecutionSession &,
                               const GuestInvocationResult &) override {
    CallPC.reset();
    return llvm::Error::success();
  }

private:
  BufferInput Input;
  uint64_t PC;
  bool ReachedStop = false, StageSent = false;
  std::optional<uint64_t> CallPC;
};
} // namespace
llvm::Expected<ExecutionRunFactoryResult>
createBufferExecution(ExecutionCreationInput Input,
                      std::shared_ptr<modules::ModuleSet> Sources) {
  if (Input.EnvironmentKind != "buffer" || !Input.Options.Bindings.empty())
    return invalid("buffer creation requires its explicit kind and unresolved "
                   "binding descriptions");
  auto Config = decode(Input.EnvironmentJSON);
  if (!Config)
    return Config.takeError();
  auto Physical = PhysicalMemory::create(Config->MemoryLimit);
  if (!Physical)
    return Physical.takeError();
  auto Space = AddressSpace::create(*Physical, Config->MemoryLimit);
  if (!Space)
    return Space.takeError();
  ExecutionSpaceResolver Resolve = [S = *Space](llvm::StringRef Name)
      -> llvm::Expected<std::shared_ptr<AddressSpace>> {
    if (!Name.empty() && Name != "buffer")
      return invalid("unknown buffer address space");
    return S;
  };
  auto Bindings = resolveExecutionBindings(Input.Bindings, Resolve);
  if (!Bindings)
    return Bindings.takeError();
  Input.Options.Bindings = std::move(*Bindings);
  auto CPU = createExecutionBackend(Config->CPU, *Space);
  if (!CPU)
    return CPU.takeError();
  const auto TransferLimit = Input.Options.MemoryTransferLimit;
  auto Run = ExecutionRun::create(
      std::move(CPU->CPU), std::move(Sources),
      std::make_unique<BufferEnvironment>(std::move(*Config)),
      std::move(Input.Options));
  if (!Run)
    return Run.takeError();
  ExecutionSpaceIdentifier Identify =
      [S = *Space](const auto &Candidate) -> llvm::Expected<std::string> {
    if (Candidate != S)
      return invalid(
          "address space does not belong to this buffer environment");
    return std::string("buffer");
  };
  return ExecutionRunFactoryResult{std::move(*Run), std::move(Resolve),
                                   std::move(Identify), TransferLimit};
}
} // namespace neverd::emulation
