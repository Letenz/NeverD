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
  auto Contract = contract();
  Contract.Gateways[0].Imports[0].Parameters = {StateSourceType::I32};
  auto R = prepareStateSourceRecovery(*M, Contract);
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
  EXPECT_NE(Source.find("int32_t example_query(int32_t)"), std::string::npos);
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
int32_t example_query(int32_t x) { *active_guest += (uint32_t)x; return (int32_t)(x ^ 0x12345678U); }
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

std::string stringProgram() {
  return R"(
declare i64 @gateway(ptr, ptr)
define i64 @work(ptr %state, ptr %context) {
  %base = load i64, ptr %state, align 1
  %name = add i64 %base, 3
  %arg = getelementptr i8, ptr %state, i64 8
  %p = getelementptr i8, ptr %state, i64 16
  %sel = getelementptr i8, ptr %state, i64 32
  %off = getelementptr i8, ptr %state, i64 40
  %width = getelementptr i8, ptr %state, i64 48
  %key = getelementptr i8, ptr %state, i64 64
  store i64 16, ptr %arg, align 1
  store i64 %name, ptr %p, align 1
  store i64 7, ptr %sel, align 1
  store i64 24, ptr %off, align 1
  store i64 8, ptr %width, align 1
  store i64 90, ptr %key, align 1
  %status = call i64 @gateway(ptr %state, ptr %context)
  ret i64 %status
})";
}
StateSourceContract stringContract() {
  auto C = contract();
  C.StateBytes = 80;
  auto &G = C.Gateways[0];
  G.ArgumentCount.reset();
  G.Arguments = {{8, 8}, {24, 8}};
  G.IndirectArguments = {0};
  G.Writes.push_back({{16, 8}, 0});
  G.Imports = {{7,
                "lookup_item",
                {StateSourceType::Pointer, StateSourceType::U64},
                StateSourceType::Pointer}};
  G.Strings = {{0, 0, true, StateSourceSlot{56, 8}, StateSourceSlot{64, 8}, 1}};
  G.ExcludedArguments = {{1, UINT64_MAX}};
  StateSourceImage I;
  I.Base = {0, 8};
  I.Readable = true;
  I.Bytes = {1, 2, 3};
  for (unsigned char B : std::string("example.dll"))
    I.Bytes.push_back(B ^ 90);
  I.Bytes.push_back(90);
  C.Images = {I};
  return C;
}

TEST(StateSource, RejectsIncompleteStringAndIndirectContractsBeforeMutation) {
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, stringProgram());
  ASSERT_TRUE(M);
  const auto Before = cell_test::print(*M);
  for (unsigned Case = 0; Case < 9; ++Case) {
    auto Contract = stringContract();
    auto &G = Contract.Gateways[0];
    if (Case == 0)
      Contract.Images[0].Readable = false;
    if (Case == 1)
      Contract.Images[0].Base.Bytes = 4;
    if (Case == 2)
      Contract.Images[0].Bytes.clear();
    if (Case == 3)
      G.Strings[0].ByValue = false;
    if (Case == 4)
      G.Strings[0].Mode.reset();
    if (Case == 5)
      G.Strings[0].Image = 1;
    if (Case == 6)
      G.IndirectArguments = {0, 0};
    if (Case == 7)
      G.ExcludedArguments[0].Argument = 2;
    if (Case == 8)
      G.Imports[0].Parameters[0] = StateSourceType::U64;
    auto R = prepareStateSourceRecovery(*M, Contract);
    ASSERT_FALSE(bool(R)) << Case;
    llvm::consumeError(R.takeError());
    EXPECT_EQ(cell_test::print(*M), Before) << Case;
  }
}

TEST(StateSource, StringCandidatesRequireProvenOffsetsKeysAndTerminators) {
  for (bool Big : {false, true})
    for (unsigned Case = 0; Case < 8; ++Case) {
      llvm::LLVMContext C;
      auto Text = stringProgram();
      auto Contract = stringContract();
      if (Big)
        Contract.Gateways[0].Selector = {36, 4};
      if (Case == 1)
        cell_test::replace(Text, "store i64 16, ptr %arg",
                           "store i64 %base, ptr %arg");
      if (Case == 2)
        cell_test::replace(Text, "store i64 16, ptr %arg",
                           "store i64 79, ptr %arg");
      if (Case == 3)
        cell_test::replace(Text, "store i64 90, ptr %key",
                           "store i64 %base, ptr %key");
      if (Case == 4)
        Contract.Images[0].Bytes.back() = 91;
      if (Case == 5)
        cell_test::replace(Text, "%name = add i64 %base, 3",
                           "%name = add i64 %base, 99");
      if (Case >= 6) {
        for (size_t I = 3; I < Contract.Images[0].Bytes.size(); ++I)
          Contract.Images[0].Bytes[I] ^= 90;
        if (Case == 6)
          cell_test::replace(Text, "store i64 90, ptr %key",
                             "store i64 0, ptr %key");
        else
          cell_test::replace(Text, "store i64 90, ptr %key",
                             "%mode = getelementptr i8, ptr %state, i64 56\n"
                             "  store i64 0, ptr %mode, align 1\n"
                             "  store i64 90, ptr %key");
      }
      auto M = cell_test::parse(C, Text, Big ? "E-p:64:64" : "e-p:64:64");
      ASSERT_TRUE(M);
      M->setTargetTriple(llvm::Triple(Big ? "aarch64_be-unknown-linux-gnu"
                                          : "x86_64-pc-windows-msvc"));
      auto R = prepareStateSourceRecovery(*M, Contract);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      EXPECT_EQ(R->RewrittenCalls, unsigned(Case == 0 || Case >= 6))
          << cell_test::print(*M);
      EXPECT_EQ(R->GuardedCalls, unsigned(Case == 0 || Case >= 6));
      optimize(*M);
      const auto IR = cell_test::print(*M);
      EXPECT_NE(IR.find("call i64 @gateway"), std::string::npos);
      EXPECT_EQ(IR.find("__neverd_state_probe_"), std::string::npos);
      if (Case == 0 || Case >= 6) {
        EXPECT_NE(IR.find("example.dll"), std::string::npos);
        EXPECT_NE(IR.find("call ptr @lookup_item"), std::string::npos);
      }
    }
}

TEST_F(StateSourceCompiled,
       StringGuardsKeepChangedBytesModesHandlesAndFullState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, stringProgram());
  ASSERT_TRUE(M);
  M->getFunction("work")->setName("before");
  auto Original = tmpFile("string-before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  M->getFunction("before")->setName("after");
  auto R = prepareStateSourceRecovery(*M, stringContract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  ASSERT_EQ(R->RewrittenCalls, 1u) << cell_test::print(*M);
  ASSERT_EQ(R->GuardedCalls, 1u);
  optimize(*M);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  EXPECT_NE(Source.find("lookup_item(\"example.dll\""), std::string::npos)
      << Source;
  auto After = tmpFile("string-after.c");
  std::ofstream(After) << Source;
  auto Harness = tmpFile("string-oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(void *, void *), after(void *, void *);
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v) { memcpy((char*)s+o,&v,8); }
static unsigned calls, gateways;
static uint64_t trace;
static unsigned char result, *guest;
void *lookup_item(const char *s, uint64_t handle) {
  ++calls;trace=handle;
  while (*s) trace=trace*131+(unsigned char)*s++;
  guest[0]^=(unsigned char)trace;
  return handle & 1 ? &result : 0;
}
uint64_t gateway(void *s,void *context) {
  (void)context;++gateways;
  uint64_t handle=get(s,24);
  if (handle==UINT64_MAX) { put(s,72,12345);return UINT64_C(0xfedcba9876543210); }
  const char *p=(const char*)(uintptr_t)get(s,(unsigned)get(s,8));
  char text[64];unsigned i=0;
  unsigned char key=get(s,56)==1 && get(s,64) ? (unsigned char)get(s,64) : 0;
  do { text[i]=(unsigned char)p[i]^key; } while (text[i++] && i<sizeof(text));
  uint64_t offset=get(s,40);
  put(s,56,0);put(s,16,0);
  put(s,(unsigned)offset,(uintptr_t)lookup_item(text,handle));
  return 0;
}
int main(void) {
 unsigned fast=0,slow=0;
 for (unsigned k=0;k<1536;++k) {
  unsigned mode=k%3, encoding=(k/3)%2, changed=(k/6)%16, off=(k/96)%16;
  unsigned char image[40]={1,2,3}, a[96],b[96], ga[8],gb[8];
  memset(image+3,'X',36);image[39]=0;
  memcpy(image+3,"example.dll",12);
  if (changed<12) image[3+changed]^=1;
  if (encoding) for(unsigned j=3;j<40;++j) image[j]^=90;
  /* Always supply a terminator for the gateway's selected normalization. */
  image[39]=mode==1 ? 90 : 0;
  for(unsigned j=0;j<96;++j) a[j]=(unsigned char)(j*13+k);
  memset(ga,17,8);memcpy(gb,ga,8);
  put(a+off,0,(uintptr_t)image);put(a+off,56,mode);
  put(a+off,24,k%5==0 ? UINT64_MAX : (uint64_t)k*913);
  memcpy(b,a,96);
  guest=ga;calls=gateways=0;trace=0;
  uint64_t ra=before(a+off,0), ta=trace;unsigned ca=calls;
  guest=gb;calls=gateways=0;trace=0;
  uint64_t rb=after(b+off,0);
  if(ra!=rb || ta!=trace || ca!=calls || memcmp(a,b,96) || memcmp(ga,gb,8)) return 1;
  if(gateways) ++slow;else ++fast;
 }
 return !(fast && slow);
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("string-oracle") + Level +
                              test::executableSuffix());
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
