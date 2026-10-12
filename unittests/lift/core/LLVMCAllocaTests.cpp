//===- LLVMCAllocaTests.cpp - LLVM allocation C semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute original LLVM and generated C with independently observed storage.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace neverd;

namespace {

std::string moduleText(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream(Text) << Module;
  return Text;
}

void comparePrograms(llvm::StringRef IR, llvm::StringRef Driver,
                     bool PreserveTypes = false,
                     llvm::StringRef SelectedFunction = {}) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(IR, Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  Module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  const std::string Before = moduleText(*Module);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.EmitIncludes = false;
  Options.PreserveLLVMFunctionTypes = PreserveTypes;
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  const auto *Only = SelectedFunction.empty()
                         ? nullptr
                         : Module->getFunction(SelectedFunction);
  ASSERT_TRUE(SelectedFunction.empty() || Only);
  ASSERT_TRUE(
      LLVMCEmitter().emit(*Module, Stream, Options, nullptr, nullptr, Only));
  EXPECT_EQ(moduleText(*Module), Before) << "C projection mutated original IR";
  // Do not execute the known underallocation produced by the old emitter.
  ASSERT_NE(Text.find("__builtin_alloca("), std::string::npos) << Text;
  auto Original = llvm::CloneModule(*Module);
  for (auto &Function : *Original)
    if (!Function.isDeclaration())
      Function.setName("original_" + Function.getName());

  auto Compiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(Compiler) << "clang is required for native allocation oracles";
  llvm::SmallString<128> Input, LLVMInput, Output, Errors;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-alloca", "c", Input));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-alloca", "ll", LLVMInput));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-alloca", "exe", Output));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-alloca", "err", Errors));
  llvm::FileRemover RemoveInput(Input), RemoveLLVM(LLVMInput),
      RemoveOutput(Output), RemoveErrors(Errors);
  std::error_code EC;
  {
    llvm::raw_fd_ostream File(Input, EC);
    ASSERT_FALSE(EC);
    File << "#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n"
         << Text << Driver;
  }
  {
    llvm::raw_fd_ostream File(LLVMInput, EC);
    ASSERT_FALSE(EC);
    File << *Original;
  }
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    const llvm::StringRef Args[] = {*Compiler,
                                    "-std=c11",
                                    Optimization,
                                    "-Werror=incompatible-pointer-types",
                                    "-Werror=int-conversion",
                                    "-fsanitize=undefined",
                                    "-fsanitize-trap=undefined",
                                    Input,
                                    LLVMInput,
                                    "-o",
                                    Output};
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Errors.str()};
    std::string Error;
    const int Status = llvm::sys::ExecuteAndWait(*Compiler, Args, std::nullopt,
                                                 Redirects, 60, 0, &Error);
    auto Log = llvm::MemoryBuffer::getFile(Errors);
    ASSERT_EQ(Status, 0) << Error << (Log ? (*Log)->getBuffer().str() : "")
                         << Text;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}, std::nullopt, {}, 30,
                                        0, &Error),
              0)
        << Error << Text << Driver.str();
  }
}

} // namespace

TEST(LLVMCAlloca, DynamicBytesKeepExtentAlignmentAndIndependentInstances) {
  comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare void @observe_bytes(ptr, i64, i64)
define i64 @dynamic_bytes(i64 %count) {
entry:
  %first = alloca i8, i64 %count, align 64
  %second = alloca i8, i64 %count, align 16
  %last_offset = sub i64 %count, 1
  %last = getelementptr i8, ptr %first, i64 %last_offset
  call void @observe_bytes(ptr %first, i64 %count, i64 64)
  call void @observe_bytes(ptr %second, i64 %count, i64 16)
  %value = load i8, ptr %last, align 1
  %result = zext i8 %value to i64
  ret i64 %result
}
)",
                  R"(
extern uint64_t original_dynamic_bytes(uint64_t);
static unsigned calls, bad;
void observe_bytes(void *address, uint64_t count, uint64_t alignment) {
  uint8_t *bytes = address;
  if (((uintptr_t)address & (alignment-1)) != 0) bad = 1;
  ++calls;
  for (uint64_t i=0; i<count; ++i) bytes[i] = (uint8_t)(i + calls*19);
}
int main(void) {
  const uint64_t sizes[] = {1, 2, 3, 7, 15, 16, 17, 63, 64, 65, 511, 4097};
  for (unsigned i=0; i<sizeof(sizes)/sizeof(sizes[0]); ++i) {
    calls = bad = 0;
    uint64_t expected = original_dynamic_bytes(sizes[i]);
    if (bad || calls != 2) return 1;
    calls = bad = 0;
    uint64_t actual = dynamic_bytes(sizes[i]);
    if (bad || calls != 2 || actual != expected) return 2;
  }
  return 0;
}
)");
}

TEST(LLVMCAlloca, ConstantElementCountsUseAllocationStride) {
  comparePrograms(R"(
target datalayout = "e-p:64:64-i24:32-i64:64-i128:128-n8:16:32:64-S128"
declare void @observe_bytes(ptr, i64, i64)
define i64 @constant_array() {
entry:
  %storage = alloca [3 x i24], i64 7, align 32
  %last = getelementptr i8, ptr %storage, i64 83
  call void @observe_bytes(ptr %storage, i64 84, i64 32)
  %value = load i8, ptr %last, align 1
  %result = zext i8 %value to i64
  ret i64 %result
}
)",
                  R"(
extern uint64_t original_constant_array(void);
static unsigned bad;
void observe_bytes(void *address, uint64_t count, uint64_t alignment) {
  if (((uintptr_t)address & (alignment-1)) != 0) bad = 1;
  for (uint64_t i=0; i<count; ++i) ((uint8_t*)address)[i] = (uint8_t)(i*3+5);
}
int main(void) {
  uint64_t expected = original_constant_array();
  uint64_t actual = constant_array();
  return bad || expected != actual;
}
)");
}

TEST(LLVMCAlloca, LoopAllocationsRetainEarlierStorageThroughReturn) {
  comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare void @observe_loop(ptr, ptr, i64)
define i64 @loop_storage(i64 %count) {
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %loop]
  %previous = phi ptr [null, %entry], [%storage, %loop]
  %storage = alloca i8, i64 %count, align 64
  call void @observe_loop(ptr %previous, ptr %storage, i64 %i)
  %next = add i64 %i, 1
  %more = icmp ult i64 %next, 8
  br i1 %more, label %loop, label %exit
exit:
  %value = load i8, ptr %storage, align 1
  %result = zext i8 %value to i64
  ret i64 %result
}
)",
                  R"(
extern uint64_t original_loop_storage(uint64_t);
static void *saved[8];
static unsigned bad;
void observe_loop(void *previous, void *current, uint64_t iteration) {
  if (((uintptr_t)current & 63) != 0) bad = 1;
  for (uint64_t i=0; i<iteration; ++i)
    if (saved[i] == current || *(uint8_t*)saved[i] != (uint8_t)(i+23)) bad=1;
  if (iteration && previous != saved[iteration-1]) bad=1;
  saved[iteration] = current;
  *(uint8_t*)current = (uint8_t)(iteration+23);
}
int main(void) {
  for (uint64_t count=1; count<80; ++count) {
    bad=0;
    uint64_t expected=original_loop_storage(count);
    if (bad) return 1;
    uint64_t actual=loop_storage(count);
    if (bad || actual != expected) return 2;
  }
  return 0;
}
)");
}

TEST(LLVMCAlloca, CountProducerIsEvaluatedOnceWithNarrowUnsignedCount) {
  for (bool Preserve : {false, true})
    comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare i32 @read_count(i32)
declare void @observe_bytes(ptr, i64, i64)
define i64 @produced_count(i32 %requested) {
entry:
  %count = call i32 @read_count(i32 %requested)
  %storage = alloca i16, i32 %count, align 32
  %wide = zext i32 %count to i64
  %bytes = mul i64 %wide, 2
  call void @observe_bytes(ptr %storage, i64 %bytes, i64 32)
  %offset = sub i64 %bytes, 1
  %last = getelementptr i8, ptr %storage, i64 %offset
  %value = load i8, ptr %last, align 1
  %result = zext i8 %value to i64
  ret i64 %result
}
)",
                    R"(
extern uint64_t original_produced_count(uint32_t);
static unsigned calls, bad;
uint32_t read_count(uint32_t count) { ++calls; return count; }
void observe_bytes(void *address, uint64_t count, uint64_t alignment) {
  if (((uintptr_t)address & (alignment-1)) != 0) bad=1;
  for (uint64_t i=0; i<count; ++i) ((uint8_t*)address)[i]=(uint8_t)(i*7+3);
}
int main(void) {
  for (uint32_t i=1; i<=128; ++i) {
    calls=bad=0;
    uint64_t expected=original_produced_count(i);
    if (calls != 1 || bad) return 1;
    calls=bad=0;
    uint64_t actual=produced_count(i);
    if (calls != 1 || bad || actual != expected) return 2;
  }
  return 0;
}
)",
                    Preserve);
}

TEST(LLVMCAlloca, NonEntryUnitAllocationsKeepDistinctLoopInstances) {
  comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare void @observe_loop(ptr, ptr, i64)
define i64 @unit_storage() {
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %loop]
  %previous = phi ptr [null, %entry], [%storage, %loop]
  %storage = alloca i8, align 64
  call void @observe_loop(ptr %previous, ptr %storage, i64 %i)
  %next = add i64 %i, 1
  %more = icmp ult i64 %next, 8
  br i1 %more, label %loop, label %exit
exit:
  %value = load i8, ptr %storage, align 1
  %result = zext i8 %value to i64
  ret i64 %result
}
)",
                  R"(
extern uint64_t original_unit_storage(void);
static void *saved[8];
static unsigned bad;
void observe_loop(void *previous, void *current, uint64_t iteration) {
  if (((uintptr_t)current & 63) != 0) bad=1;
  for (uint64_t i=0; i<iteration; ++i)
    if (saved[i] == current || *(uint8_t*)saved[i] != (uint8_t)(i+23)) bad=1;
  if (iteration && previous != saved[iteration-1]) bad=1;
  saved[iteration]=current;
  *(uint8_t*)current=(uint8_t)(iteration+23);
}
int main(void) {
  uint64_t expected=original_unit_storage();
  uint64_t actual=unit_storage();
  return bad || actual != expected;
}
)");
}

TEST(LLVMCAlloca, UnsupportedStackContractsRefuseBeforeDeadCodeRemoval) {
  const char *Inputs[] = {
      R"(target datalayout = "e-p:64:64"
define void @bad(i128 %count) {
  %storage = alloca i8, i128 %count
  ret void
})",
      R"(target datalayout = "e-p:64:64"
define void @bad() {
  %storage = alloca i64, i64 18446744073709551615
  ret void
})",
      R"(target datalayout = "e-p:64:64-A5"
define void @bad(i64 %count) {
  %storage = alloca i8, i64 %count, addrspace(5)
  ret void
})",
      R"(target datalayout = "e-p:64:64"
define void @bad(i64 %count) {
  %storage = alloca inalloca i8, i64 %count
  ret void
})",
      R"(target datalayout = "e-p:64:64"
define void @bad() {
  %storage = alloca swifterror ptr
  ret void
})",
      R"(target datalayout = "e-p:64:64"
define void @bad(i64 %count) {
  %storage = alloca <vscale x 4 x i32>, i64 %count
  ret void
})",
      R"(target datalayout = "e-p:64:64"
declare ptr @llvm.stacksave.p0()
define void @bad() {
  %saved = call ptr @llvm.stacksave.p0()
  ret void
})",
      R"(target datalayout = "e-p:64:64"
declare void @llvm.stackrestore.p0(ptr)
define void @bad(ptr %saved) {
  call void @llvm.stackrestore.p0(ptr %saved)
  ret void
})",
      R"(target datalayout = "e-p:64:64"
declare ptr @forged(i64, i64, i64) #0
define void @bad() { ret void }
attributes #0 = { "neverd.c.stack-allocation" })"};
  for (const char *Input : Inputs) {
    SCOPED_TRACE(Input);
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(Input, Diagnostic, Context);
    ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    const auto Before = moduleText(*Module);
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    EXPECT_THROW(LLVMCEmitter().emit(*Module, Stream, Options),
                 std::runtime_error);
    EXPECT_EQ(moduleText(*Module), Before);
    EXPECT_TRUE(Text.empty());
  }
}

TEST(LLVMCAlloca, ZeroExtentAndProviderNameCollisionKeepOrdinaryCalls) {
  comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare i64 @nd_c_stack_allocation(i64)
declare void @observe_zero(ptr, i64)
define i64 @zero_storage(i64 %count) {
entry:
  %storage = alloca i8, i64 %count, align 64
  call void @observe_zero(ptr %storage, i64 %count)
  %result = call i64 @nd_c_stack_allocation(i64 13)
  ret i64 %result
}
)",
                  R"(
extern uint64_t original_zero_storage(uint64_t);
static unsigned calls, bad;
uint64_t nd_c_stack_allocation(uint64_t value) { ++calls; return value+71; }
void observe_zero(void *address, uint64_t count) {
  if (count != 0 || ((uintptr_t)address & 63) != 0) bad=1;
  ++calls;
}
int main(void) {
  calls=bad=0;
  uint64_t expected=original_zero_storage(0);
  if (bad || calls != 2) return 1;
  calls=bad=0;
  uint64_t actual=zero_storage(0);
  return bad || calls != 2 || actual != expected;
}
)",
                  false, "zero_storage");
}
