//===- StateSourceRecoveryTests.cpp - State/call source semantic oracles
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bank, partial-width, guest-memory and unknown-call checks.

#include "../../LLVMHostFixture.h"
#include "../NeverDLiftFixture.h"
#include "ByteCellScalarizationTest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/pipeline/StateSourceRecovery.h"

using namespace neverd;
namespace {
std::string program() {
  return R"(
declare i64 @gateway(ptr, ptr)
declare i64 @opaque(ptr, ptr)
define i64 @work(ptr %state, ptr %context) {
entry:
  %a = getelementptr i8, ptr %state, i64 8
  %r = getelementptr i8, ptr %state, i64 16
  %sel = getelementptr i8, ptr %state, i64 32
  %off = getelementptr i8, ptr %state, i64 40
  %width = getelementptr i8, ptr %state, i64 48
  %count = getelementptr i8, ptr %state, i64 56
  %x = load i64, ptr %state, align 1
  store i64 %x, ptr %a, align 1
  store i64 7, ptr %sel, align 1
  store i64 16, ptr %off, align 1
  store i64 4, ptr %width, align 1
  store i64 1, ptr %count, align 1
  %state_integer = ptrtoint ptr %state to i64
  %state_pointer = inttoptr i64 %state_integer to ptr
  %status = call i64 @gateway(ptr %state_pointer, ptr %context)
  %bad = icmp ne i64 %status, 0
  br i1 %bad, label %failed, label %next
failed:
  ret i64 %status
next:
  %value = load i32, ptr %r, align 1
  %c1 = icmp slt i32 %value, 1024
  %c2 = icmp ult i32 %value, 100
  %f1 = select i1 %c1, i64 256, i64 0
  %f2 = select i1 %c2, i64 2, i64 0
  %flags = or i64 %f1, %f2
  %bit = and i64 %flags, 256
  %small = icmp ne i64 %bit, 0
  br i1 %small, label %left, label %right
left:
  store i32 %value, ptr %context, align 1
  br label %done
right:
  store i32 123, ptr %context, align 1
  br label %done
done:
  %unknown = call i64 @opaque(ptr %state, ptr %context)
  ret i64 %unknown
})";
}
StateSourceContract contract() {
  StateSourceContract C;
  C.StateBytes = 64;
  C.IsolatedBank = true;
  C.ZeroStatusGateways = true;
  StateSourceGateway G;
  G.Name = "gateway";
  G.Selector = {32, 4};
  G.ArgumentCount = {56, 4};
  G.Arguments = {{8, 8}};
  G.ResultOffset = {40, 8};
  G.ResultBytes = {48, 8};
  G.Writes = {{{56, 8}, 0}};
  G.Imports = {
      {7, "example_query", {StateSourceType::U64}, StateSourceType::I32}};
  C.Gateways = {G};
  return C;
}
void optimize(llvm::Module &M, bool Deep = true) {
  Pipeline::OptimizationOptions O;
  O.Strength = Deep ? Pipeline::OptStrength::Deep : Pipeline::OptStrength::Thin;
  O.LLVMLevel = llvm::OptimizationLevel::O2;
  auto R = Pipeline::optimizeModule(M, O);
  ASSERT_NE(R.Stop, OptimizationStopReason::InputInvalid);
  ASSERT_NE(R.Stop, OptimizationStopReason::VerificationFailed);
}
class StateSourceCompiled : public NeverDLiftTest {};

TEST(StateSource, RejectsMissingAssertionsAndConflictingImportsBeforeMutation) {
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, program());
  ASSERT_TRUE(M);
  auto Before = cell_test::print(*M);
  for (unsigned Case = 0; Case != 7; ++Case) {
    auto Contract = contract();
    if (Case == 0)
      Contract.IsolatedBank = false;
    if (Case == 1)
      Contract.ZeroStatusGateways = false;
    if (Case == 2)
      Contract.Gateways[0].Arguments[0].Offset = 64;
    if (Case == 3)
      Contract.Gateways[0].Imports[0].Name = "work";
    if (Case == 4)
      Contract.Gateways[0].Imports[0].Name = "while";
    if (Case == 5)
      Contract.Gateways[0].Imports[0].Result = StateSourceType(99);
    if (Case == 6) {
      auto I = Contract.Gateways[0].Imports[0];
      I.Selector = 8;
      I.Result = StateSourceType::U32;
      Contract.Gateways[0].Imports.push_back(I);
    }
    auto R = prepareStateSourceRecovery(*M, Contract);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
    EXPECT_EQ(Before, cell_test::print(*M));
  }
}

TEST(StateSource, RefusesObservableDynamicOrOrderedBankAddresses) {
  for (const char *Extra :
       {"store ptr %state, ptr %context, align 1",
        "%escaped = icmp eq ptr %state, %context",
        "%d = getelementptr i8, ptr %state, i64 %x\n  store i8 1, ptr %d",
        "%ordered = load volatile i64, ptr %state, align 1",
        "%end = getelementptr i8, ptr %state, i64 63\n  store i64 1, ptr %end, "
        "align 1"}) {
    llvm::LLVMContext C;
    auto Text = program();
    cell_test::replace(Text, "store i64 %x, ptr %a, align 1",
                       std::string(Extra));
    auto M = cell_test::parse(C, Text);
    ASSERT_TRUE(M);
    auto Before = cell_test::print(*M);
    auto R = prepareStateSourceRecovery(*M, contract());
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
    EXPECT_EQ(Before, cell_test::print(*M));
  }
}

TEST(StateSource, UnreachableGatewayRemovalPreservesSuccessfulRecovery) {
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, R"(
    declare i64 @gateway(ptr, ptr)
    define i64 @work(ptr %state, ptr %context) {
    entry: ret i64 0
    dead:
      %s = call i64 @gateway(ptr %state, ptr %context)
      ret i64 %s
    })");
  ASSERT_TRUE(M);
  auto R = prepareStateSourceRecovery(*M, contract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  const auto ProbeIR = cell_test::print(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  EXPECT_EQ(R->RewrittenCalls, 0u);
  EXPECT_EQ(R->UnresolvedCalls, 0u);
}

TEST(StateSource, KeepsMixedBankAndGuestSelectionsAsPointers) {
  llvm::LLVMContext C;
  auto Text = program();
  cell_test::replace(
      Text, "%value = load i32, ptr %r, align 1",
      "%which = icmp eq i64 %x, 0\n  %address = select i1 %which, ptr %r, ptr "
      "%context\n  %value = load i32, ptr %address, align 1");
  auto M = cell_test::parse(C, Text);
  ASSERT_TRUE(M);
  auto R = prepareStateSourceRecovery(*M, contract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  const auto ProbeIR = cell_test::print(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  EXPECT_EQ(R->RewrittenCalls, 1u) << ProbeIR;
  EXPECT_FALSE(llvm::verifyModule(*M));
}

TEST(StateSource,
     ResolvesCurrentIRConstantsOnWindowsAndELFAndKeepsUnknownSites) {
  for (const char *Triple :
       {"x86_64-pc-windows-msvc", "aarch64-unknown-linux-gnu"})
    for (bool Known : {false, true}) {
      llvm::LLVMContext C;
      auto Text = program();
      if (!Known)
        cell_test::replace(Text, "store i64 7, ptr %sel",
                           "store i64 %x, ptr %sel");
      auto M = cell_test::parse(C, Text);
      ASSERT_TRUE(M);
      M->setTargetTriple(llvm::Triple(Triple));
      auto R = prepareStateSourceRecovery(*M, contract());
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      const auto ProbeIR = cell_test::print(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      EXPECT_EQ(R->RewrittenCalls, unsigned(Known)) << ProbeIR;
      EXPECT_EQ(R->UnresolvedCalls, unsigned(!Known));
      optimize(*M, true);
      auto Printed = cell_test::print(*M);
      EXPECT_EQ(Printed.find("__neverd_state_probe_"), std::string::npos);
      EXPECT_NE(Printed.find("call i64 @opaque"), std::string::npos);
      if (Known) {
        EXPECT_NE(Printed.find("call i32 @example_query"), std::string::npos);
        EXPECT_EQ(Printed.find("call i64 @gateway"), std::string::npos);
        EXPECT_EQ(Printed.find("and i64 %flags"), std::string::npos);
      }
    }
}

TEST(StateSource, PartialSelectorsUseTheOriginalSnapshotOnBothByteOrders) {
  for (bool Big : {false, true})
    for (unsigned Bytes : {1u, 2u, 4u}) {
      llvm::LLVMContext C;
      auto Text = program();
      const auto Delta = Big ? 8 - Bytes : 0;
      cell_test::replace(
          Text, "store i64 7, ptr %sel",
          "%partial = getelementptr i8, ptr %state, i64 " +
              std::to_string(24 + Delta) + "\n  store i" +
              std::to_string(Bytes * 8) + " 7, ptr %partial, align 1" +
              "\n  %whole_base = getelementptr i8, ptr %state, i64 24" +
              "\n  %whole = load i64, ptr %whole_base, align 1" +
              "\n  store i" + std::to_string(Bytes * 8) +
              " 9, ptr %partial, align 1" + "\n  store i64 %whole, ptr %sel");
      auto M = cell_test::parse(C, Text, Big ? "E-p:64:64" : "e-p:64:64");
      ASSERT_TRUE(M);
      auto Contract = contract();
      Contract.Gateways[0].Selector = {32 + Delta, Bytes};
      if (Big)
        Contract.Gateways[0].ArgumentCount = {60, 4};
      auto R = prepareStateSourceRecovery(*M, Contract);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      EXPECT_EQ(R->RewrittenCalls, 1u) << cell_test::print(*M);
    }
}

TEST_F(StateSourceCompiled,
       PreservesCompleteUnalignedBankGuestEffectsAndFailureStatus) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  llvm::LLVMContext C;
  auto Text = program();
  cell_test::replace(Text, "store i64 7, ptr %sel",
                     "%partial = getelementptr i8, ptr %state, i64 24\n  store "
                     "i32 7, ptr %partial, align 1\n  %whole = load i64, ptr "
                     "%partial, align 1\n  store i64 %whole, ptr %sel");
  cell_test::replace(Text, "store i64 1, ptr %count",
                     "%arity = and i64 %x, 1\n  store i64 %arity, ptr %count");
  cell_test::replace(Text, "%value = load i32, ptr %r, align 1",
                     "%choice = and i64 %x, 2\n  %which = icmp eq i64 %choice, "
                     "0\n  %address = select i1 %which, ptr %r, ptr %context\n "
                     " %value = load i32, ptr %address, align 1");
  auto M = cell_test::parse(C, Text);
  ASSERT_TRUE(M);
  M->getFunction("work")->setName("before");
  auto Original = tmpFile("before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  M->getFunction("before")->setName("after");
  auto R = prepareStateSourceRecovery(*M, contract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  const auto ProbeIR = cell_test::print(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  ASSERT_EQ(R->RewrittenCalls, 1u) << ProbeIR;
  ASSERT_EQ(R->GuardedCalls, 1u);
  optimize(*M, true);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  EXPECT_NE(Source.find("int32_t example_query(uint64_t)"), std::string::npos);
  auto After = tmpFile("after.c");
  std::ofstream(After) << Source;
  auto Harness = tmpFile("oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(void *,void *), after(void *,void *);
static uint64_t get(const void *s, unsigned o) { uint64_t v; memcpy(&v,(const char *)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char *)s+o,&v,n); }
static uint32_t *active_guest;
int32_t example_query(uint64_t x) { *active_guest += (uint32_t)x; return (int32_t)(x ^ 0x12345678U); }
uint64_t gateway(void *s,void *ctx) {
  (void)ctx;
  if ((uint32_t)get(s,32)!=7 || (uint32_t)get(s,56)!=1) return 19;
  uint64_t x=get(s,8),off=get(s,40),width=get(s,48);
  put(s,56,0,8);put(s,(unsigned)off,(uint64_t)(int64_t)example_query(x),(unsigned)width);
  return 0;
}
uint64_t opaque(void *s,void *ctx) {
  unsigned char *p=s; for(unsigned i=0;i<64;i++) p[i]^=(unsigned char)(i*11);
  uint32_t v;memcpy(&v,ctx,4);v^=0x98765432U;memcpy(ctx,&v,4);
  return (get(s,0)&1) ? UINT64_C(0xfedcba9876543210) : 0;
}
int main(void) {
 for(unsigned k=0;k<4096;k++) {
  unsigned char a[80],b[80]; for(unsigned i=0;i<80;i++) a[i]=(unsigned char)(k*7+i*13);
  unsigned offset=k&15;uint64_t x=(uint64_t)k*0x192ad76fULL;put(a+offset,0,x,8);
  memcpy(b,a,80);uint32_t ga=k,gb=k;
  active_guest=&ga;uint64_t ra=before(a+offset,&ga);
  active_guest=&gb;uint64_t rb=after(b+offset,&gb);
  if(ra!=rb || ga!=gb || memcmp(a,b,80)) return 1;
 }
 return 0;
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable =
        tmpFile(std::string("oracle") + Level + test::executableSuffix());
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Level, "-fsanitize=undefined",
                       "-fsanitize-trap=all", Original.string(), After.string(),
                       Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}
} // namespace
