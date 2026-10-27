//===- GuestInvocationABITests.cpp - Raw CPU call convention coverage
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Raw CPU call convention coverage.
///

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionRun.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t Page = 4096, Code = 0x10000, RootStack = 0x20000,
                   CallStack = 0x30000, Return = Code + 0x100;
llvm::Error unsupported() {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "unsupported raw test module");
}
/// No loader, OS or application model. This scene only exercises the common
/// integer-call and saved-control boundary on the available CPU transports.
class ABIScene final : public ExecutionEnvironment {
public:
  explicit ABIScene(CPURegister PC) : PCRegister(PC) {}
  std::string name() const override { return "raw-abi-boundary"; }
  std::vector<std::string> stageNames() const override { return {"ready"}; }
  llvm::Expected<EnvironmentAction> next(ExecutionSession &,
                                         ModuleBindings &) override {
    if (Active)
      return EnvironmentAction{EnvironmentActionKind::RunCPU, PC, "call", {}};
    if (!Announced) {
      Announced = true;
      return EnvironmentAction{
          EnvironmentActionKind::StageBoundary, 0, "ready", {}};
    }
    return EnvironmentAction{
        EnvironmentActionKind::Completed, 0, "finished", {}};
  }
  llvm::Error afterCPUExit(ExecutionSession &Session,
                           const SessionExit &) override {
    auto Next = Session.cpu().readRegister(PCRegister);
    if (!Next)
      return Next.takeError();
    PC = (*Next)[0];
    return llvm::Error::success();
  }
  llvm::Expected<ModuleMappingPlan>
  buildModule(const ModuleBinding &, const modules::ModuleImage &) override {
    return unsupported();
  }
  llvm::Expected<GuestInvocationPlan>
  planInvocation(ExecutionSession &, ModuleBindings &,
                 const GuestInvocationRequest &, uint64_t Entry,
                 llvm::StringRef Stage) override {
    if (Entry != Code || Stage != "ready")
      return unsupported();
    return GuestInvocationPlan{CallStack, Page, Return};
  }
  llvm::Error beginInvocation(ExecutionSession &,
                              const GuestInvocationFrame &Frame) override {
    PC = Frame.Entry;
    Active = true;
    return llvm::Error::success();
  }
  llvm::Error finishInvocation(ExecutionSession &,
                               const GuestInvocationResult &) override {
    Active = false;
    return llvm::Error::success();
  }

private:
  CPURegister PCRegister;
  uint64_t PC = 0;
  bool Active = false, Announced = false;
};
struct Profile {
  const char *Name;
  IntegerCallingConvention ABI;
  ExecutionContract Contract;
};
class GuestInvocationABI : public testing::TestWithParam<Profile> {};
TEST_P(GuestInvocationABI, ExplicitABIExecutesAndRestoresItsCallerRegisters) {
  const auto ABI = llvm::cantFail(IntegerABI::get(GetParam().ABI));
  const auto Architecture = ABI.info().Architecture;
  const bool ARM = Architecture == GuestArchitecture::AArch64;
  auto Backend =
      createExecutionBackend(ExecutionBackendKind::Unicorn, GetParam().Contract,
                             16 * Page, Architecture);
  if (!Backend) {
    auto E = Backend.takeError();
    const auto Missing = E.isA<BackendUnavailableError>();
    auto Text = llvm::toString(std::move(E));
    if (Missing)
      GTEST_SKIP() << Text;
    FAIL() << Text;
  }
  auto &CPU = *Backend->CPU;
  for (const auto Base : {Code, RootStack, CallStack})
    ASSERT_EQ(llvm::toString(CPU.map(
                  Base, Page, Read | Write | (Base == Code ? Execute : 0))),
              "");
  const auto PC = ARM ? CPURegister::AArch64PC : CPURegister::X64PC;
  ASSERT_EQ(llvm::toString(CPU.writeRegister(PC, {Code + 0x200, 0})), "");
  ASSERT_EQ(llvm::toString(CPU.writeRegister(ABI.info().StackPointer,
                                             {RootStack + Page - 128, 0})),
            "");
  ASSERT_EQ(llvm::toString(CPU.writeRegister(ABI.info().Result, {99, 0})), "");
  if (ARM) {
    const uint32_t Words[] = {0xD2800C73, 0x8B010000,
                              0xD65F03C0}; // X19=99; X0+=X1; RET.
    std::vector<uint8_t> Bytes(sizeof(Words));
    for (size_t I = 0; I < std::size(Words); ++I)
      llvm::support::endian::write32le(Bytes.data() + I * 4, Words[I]);
    ASSERT_EQ(llvm::toString(CPU.write(Code, Bytes)), "");
    ASSERT_EQ(llvm::toString(CPU.writeInteger(Return, 0xD503201F, 4)), "");
    ASSERT_EQ(llvm::toString(CPU.setReg(AArch64Register::X18, 0x1122)), "");
    ASSERT_EQ(llvm::toString(CPU.setReg(AArch64Register::X19, 0x3344)), "");
    ASSERT_EQ(llvm::toString(CPU.setReg(AArch64Register::X30, 0x5566)), "");
  } else {
    const uint8_t Bytes[] = {0x48, 0x89, 0xF8, 0x48,
                             0x01, 0xF0, 0xC3}; // RAX=RDI+RSI; RET.
    ASSERT_EQ(llvm::toString(CPU.write(Code, Bytes)), "");
    ASSERT_EQ(llvm::toString(CPU.writeInteger(Return, 0x90, 1)), "");
  }
  const auto Vector = RegisterValue{0x1234, 0x5678};
  ASSERT_EQ(llvm::toString(CPU.setVector(6, Vector)), "");
  auto Sources = modules::ModuleSet::create({});
  ASSERT_TRUE(bool(Sources));
  ExecutionRunOptions Options;
  Options.EnableResourcePauses = false;
  Options.PauseStages = {"ready"};
  auto Run = ExecutionRun::create(std::move(Backend->CPU), std::move(*Sources),
                                  std::make_unique<ABIScene>(PC), Options);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  auto Event = (*Run)->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_EQ(Event->Reason, ExecutionRunReason::Stage);
  GuestInvocationRequest Request;
  Request.Entry = Code;
  Request.ABI = GetParam().ABI;
  Request.Arguments = {{40, {}}, {2, {}}};
  Request.Limits.Instructions = 3;
  ASSERT_EQ(llvm::toString((*Run)->invoke(Request)), "");
  Event = (*Run)->run();
  ASSERT_TRUE(bool(Event));
  ASSERT_TRUE(Event->Invocation) << Event->Diagnostic;
  EXPECT_EQ(Event->Invocation->Value, 42u);
  EXPECT_EQ(llvm::cantFail(CPU.readRegister(ABI.info().Result))[0], 99u);
  EXPECT_EQ(llvm::cantFail(CPU.readRegister(PC))[0], Code + 0x200);
  EXPECT_EQ(llvm::cantFail(CPU.readRegister(ABI.info().StackPointer))[0],
            RootStack + Page - 128);
  EXPECT_EQ(llvm::cantFail(CPU.vector(6)), Vector);
  if (ARM) {
    EXPECT_EQ(llvm::cantFail(CPU.reg(AArch64Register::X18)), 0x1122u);
    EXPECT_EQ(llvm::cantFail(CPU.reg(AArch64Register::X19)), 0x3344u);
    EXPECT_EQ(llvm::cantFail(CPU.reg(AArch64Register::X30)), 0x5566u);
  }
}
INSTANTIATE_TEST_SUITE_P(
    CPUOnly, GuestInvocationABI,
    testing::Values(Profile{"SoftwareSysV", IntegerCallingConvention::SysVAMD64,
                            ExecutionContract::Software},
                    Profile{"SoftwareAAPCS64",
                            IntegerCallingConvention::AAPCS64,
                            ExecutionContract::Software},
                    Profile{"CheckedAAPCS64", IntegerCallingConvention::AAPCS64,
                            ExecutionContract::CheckedAArch64}),
    [](const testing::TestParamInfo<Profile> &P) { return P.param.Name; });
} // namespace
} // namespace neverd::emulation
