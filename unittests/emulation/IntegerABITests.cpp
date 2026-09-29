//===- IntegerABITests.cpp - Compiler-checked scalar call boundaries ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/IntegerABI.h"

#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"

#include <array>
#include <filesystem>

namespace neverd::emulation {
namespace {
#define NEVERD_ABI_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_ABI_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_ABI_TEST_ARGUMENTS(...)                                         \
  constexpr uint64_t Arguments[] = {__VA_ARGS__};
#include "IntegerABICases.def"
#undef NEVERD_ABI_TEST_ARGUMENTS
#undef NEVERD_ABI_TEST_TEXT
#undef NEVERD_ABI_TEST_VALUE

TEST(IntegerCallLayout, PublishedStackBoundariesAndPayloadStayDisjoint) {
  struct Case {
    IntegerCallingConvention Convention;
    uint64_t Count, Reservation;
  };
  constexpr Case Cases[] = {
#define NEVERD_ABI_TEST_LAYOUT(Name, Count, Reservation)                       \
  {IntegerCallingConvention::Name, Count, Reservation},
#include "IntegerABICases.def"
#undef NEVERD_ABI_TEST_LAYOUT
  };
  for (const auto &C : Cases) {
    auto ABI = llvm::cantFail(IntegerABI::get(C.Convention));
    auto Layout = ABI.layoutCall(Stack, PageSize, C.Count, PayloadSize);
    ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
    EXPECT_EQ(Layout->StackPointer,
              Stack + PageSize - PayloadSize - C.Reservation);
    EXPECT_EQ(Layout->PayloadAddress, Stack + PageSize - PayloadSize);
    EXPECT_LE(Layout->ReturnStackPointer, Layout->PayloadAddress);
    if (C.Count > ABI.info().Arguments.size()) {
      auto Last = llvm::cantFail(
          ABI.argumentLocation(Layout->StackPointer, C.Count - 1));
      EXPECT_LE(Last.Address + sizeof(uint64_t), Layout->PayloadAddress);
    }
  }
}

TEST(IntegerCallLayout, RejectsRangeOverflowAndPreservesTheRedZone) {
  constexpr IntegerCallingConvention Conventions[] = {
#define NEVERD_INTEGER_ABI(Name, ...) IntegerCallingConvention::Name,
#include "neverd/emulation/IntegerABI.def"
#undef NEVERD_INTEGER_ABI
  };
  for (auto Convention : Conventions) {
    auto ABI = llvm::cantFail(IntegerABI::get(Convention));
    std::array Results = {
        ABI.layoutCall(UINT64_MAX - PageSize, PageSize + 1, 0),
        ABI.layoutCall(Stack, PageSize - 1, 0),
        ABI.layoutCall(Stack, PageSize, UINT64_MAX),
        ABI.layoutCall(Stack, PageSize, 0, PayloadSize + 1),
        ABI.layoutCall(Stack, PageSize, 0, PageSize * 2)};
    for (auto &Result : Results) {
      EXPECT_FALSE(bool(Result));
      llvm::consumeError(Result.takeError());
    }
    auto Layout =
        llvm::cantFail(ABI.layoutCall(Stack, PageSize, ArgumentCount));
    auto Overflow = ABI.argumentLocation(Layout.StackPointer, UINT64_MAX);
    EXPECT_FALSE(bool(Overflow));
    llvm::consumeError(Overflow.takeError());
  }
  auto ABI =
      llvm::cantFail(IntegerABI::get(IntegerCallingConvention::SysVAMD64));
  auto TooSmall = ABI.layoutCall(Stack, ABI.info().RedZoneSize, 0);
  EXPECT_FALSE(bool(TooSmall));
  llvm::consumeError(TooSmall.takeError());
  auto Invalid = IntegerABI::get(static_cast<IntegerCallingConvention>(-1));
  EXPECT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
}

struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  IntegerCallingConvention Convention;
  GuestArchitecture ISA;
  ExecutionContract Contract;
  const char *Fixture;
};
constexpr Profile Profiles[] = {
#define NEVERD_ABI_TEST_PROFILE(Name, Backend, Convention, ISA, Contract)      \
  {#Name,                                                                      \
   ExecutionBackendKind::Backend,                                              \
   IntegerCallingConvention::Convention,                                       \
   GuestArchitecture::ISA,                                                     \
   ExecutionContract::Contract,                                                \
   #Convention ".obj"},
#include "IntegerABICases.def"
#undef NEVERD_ABI_TEST_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }

class IntegerCalls : public testing::TestWithParam<Profile> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  IntegerABI ABI =
      llvm::cantFail(IntegerABI::get(IntegerCallingConvention::Win64));

  void SetUp() override {
    ABI = llvm::cantFail(IntegerABI::get(GetParam().Convention));
    auto B = createExecutionBackend(GetParam().Backend, GetParam().Contract,
                                    Limit, GetParam().ISA);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(CPU->map(Stack, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->writeRegister(ABI.info().StackPointer, {Stack, 0}));
  }
  uint64_t reg(CPURegister R) {
    return llvm::cantFail(CPU->readRegister(R))[0];
  }
};

TEST_P(IntegerCalls, ReadsRegisterAndStackArgumentsAndReturnAddress) {
  auto Frame = llvm::cantFail(
      ABI.prepareCall(*CPU, Stack, PageSize, ReturnPC, Arguments, PayloadSize));
  for (size_t I = 0; I < std::size(Arguments); ++I)
    EXPECT_EQ(llvm::cantFail(ABI.readArgument(*CPU, Frame.StackPointer, I)),
              Arguments[I]);
  EXPECT_EQ(llvm::cantFail(ABI.readReturnAddress(*CPU, Frame.StackPointer)),
            ReturnPC);
  EXPECT_EQ(llvm::cantFail(ABI.returnStackPointer(Frame.StackPointer)),
            Frame.ReturnStackPointer);
  EXPECT_EQ(reg(ABI.info().StackPointer), Frame.StackPointer);
}

TEST_P(IntegerCalls, ShortCallClearsOnlyUnusedArgumentRegisters) {
  for (auto R : ABI.info().Arguments)
    llvm::cantFail(CPU->writeRegister(R, {Canary, 0}));
  const auto Preserved = GetParam().ISA == GuestArchitecture::X64
                             ? CPURegister::X64BX
                             : CPURegister::AArch64X18;
  llvm::cantFail(CPU->writeRegister(Preserved, {Canary, 0}));
  llvm::cantFail(
      ABI.prepareCall(*CPU, Stack, PageSize, ReturnPC, {Arguments[0]}));
  EXPECT_EQ(reg(ABI.info().Arguments[0]), Arguments[0]);
  for (auto R : ABI.info().Arguments.drop_front())
    EXPECT_EQ(reg(R), 0u);
  EXPECT_EQ(reg(Preserved), Canary);
}

TEST_P(IntegerCalls, InvalidLayoutAndPermissionsDoNotModifyCPUOrMemory) {
  llvm::cantFail(CPU->writeInteger(Stack + PageSize - sizeof(uint64_t), Canary,
                                   sizeof(uint64_t)));
  llvm::cantFail(CPU->writeRegister(ABI.info().Arguments[0], {Canary, 0}));
  for (uint64_t Payload : {PageSize * 2, PayloadSize + 1}) {
    auto Frame =
        ABI.prepareCall(*CPU, Stack, PageSize, ReturnPC, Arguments, Payload);
    EXPECT_FALSE(bool(Frame));
    llvm::consumeError(Frame.takeError());
  }
  llvm::cantFail(CPU->protect(Stack, PageSize, Read | UserAccessible));
  auto Frame = ABI.prepareCall(*CPU, Stack, PageSize, ReturnPC, Arguments);
  EXPECT_FALSE(bool(Frame));
  llvm::consumeError(Frame.takeError());
  EXPECT_EQ(reg(ABI.info().Arguments[0]), Canary);
  EXPECT_EQ(reg(ABI.info().StackPointer), Stack);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Stack + PageSize - sizeof(uint64_t),
                                            sizeof(uint64_t))),
            Canary);
  EXPECT_FALSE(CPU->fault());
  llvm::cantFail(CPU->protect(Stack, PageSize, Read | Write | UserAccessible));
  auto Valid = ABI.prepareCall(*CPU, Stack, PageSize, ReturnPC, Arguments);
  EXPECT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
}

TEST_P(IntegerCalls,
       RejectsCrossISAAndMisalignedArgumentAccessWithoutFaulting) {
  auto Other =
      llvm::cantFail(IntegerABI::get(GetParam().ISA == GuestArchitecture::X64
                                         ? IntegerCallingConvention::AAPCS64
                                         : IntegerCallingConvention::Win64));
  auto Frame = Other.prepareCall(*CPU, Stack, PageSize, ReturnPC, Arguments);
  EXPECT_FALSE(bool(Frame));
  llvm::consumeError(Frame.takeError());
  auto Result = ABI.readArgument(*CPU, Stack + 1, 0);
  EXPECT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(reg(ABI.info().StackPointer), Stack);
  EXPECT_FALSE(CPU->fault());
}

TEST_P(IntegerCalls,
       CompilerFunctionUsesStackArgumentsLocalsAndReturnsBalanced) {
#ifndef NEVERD_ABI_FIXTURE_DIR
  GTEST_SKIP() << MissingCompiler;
#else
  auto Buffer = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ABI_FIXTURE_DIR) / GetParam().Fixture)
          .string());
  ASSERT_TRUE(bool(Buffer)) << Buffer.getError().message();
  auto Object =
      llvm::object::ObjectFile::createObjectFile((*Buffer)->getMemBufferRef());
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  // The fixture is deliberately one self-contained function. LLVM, rather
  // than a test-local binary parser, provides its symbol and section bytes.
  bool Loaded = false;
  uint64_t Entry = 0;
  llvm::cantFail(CPU->map(Code, PageSize, Read | Write | UserAccessible));
  for (auto Symbol : (*Object)->symbols()) {
    if (llvm::cantFail(Symbol.getName()) != FixtureSymbol)
      continue;
    auto Section = llvm::cantFail(Symbol.getSection());
    ASSERT_NE(Section, (*Object)->section_end());
    ASSERT_TRUE(Section->isText());
    ASSERT_TRUE(Section->relocations().empty());
    auto Bytes = llvm::cantFail(Section->getContents());
    ASSERT_LT(Bytes.size(), ReturnPC - Code);
    llvm::cantFail(CPU->write(
        Code,
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Bytes.data()),
                                Bytes.size())));
    Entry = Code + llvm::cantFail(Symbol.getAddress()) - Section->getAddress();
    Loaded = true;
  }
  ASSERT_TRUE(Loaded);
  llvm::cantFail(CPU->protect(Code, PageSize, Read | Execute | UserAccessible));
  auto Frame = llvm::cantFail(
      ABI.prepareCall(*CPU, Stack, PageSize, ReturnPC, Arguments, PayloadSize));
  llvm::cantFail(
      CPU->writeInteger(Frame.PayloadAddress, Canary, sizeof(uint64_t)));
  bool Returned = false;
  unsigned Count = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    Returned = PC == ReturnPC;
    if (Returned || ++Count == MaxInstructions)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  auto Exit = llvm::cantFail(CPU->runUntilExit(Entry, Timeout));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  ASSERT_TRUE(Returned) << Exit.Diagnostic;
  const auto &A = Arguments;
  const uint64_t Expected =
      (((A[0] ^ A[9]) + A[1] + A[8]) ^ (A[2] * A[7] + A[3] - A[6])) +
      A[4] * A[5];
  EXPECT_EQ(reg(ABI.info().Result), Expected);
  EXPECT_EQ(reg(ABI.info().StackPointer), Frame.ReturnStackPointer);
  EXPECT_EQ(
      llvm::cantFail(CPU->readInteger(Frame.PayloadAddress, sizeof(uint64_t))),
      Canary);
#endif
}

INSTANTIATE_TEST_SUITE_P(Backends, IntegerCalls, testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
} // namespace
} // namespace neverd::emulation
