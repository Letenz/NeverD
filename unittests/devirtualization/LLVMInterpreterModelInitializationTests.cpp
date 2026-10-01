//===- LLVMInterpreterModelInitializationTests.cpp - Byte initialization -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMInterpreterModelTest.h"

namespace neverd::analysis::llvm_model_test {
TEST_F(LLVMModel, InitializedWordCanBeReadAfterItsStore) {
  parse(R"(
    store i64 7, ptr %state, align 8
    %v = load i64, ptr %state, align 8
    %other = getelementptr i8, ptr %state, i64 8
    store i64 %v, ptr %other, align 8
    ret i64 0
  )",
        {}, {}, {}, "initializes((0,8))");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::COPY, r(0), {n(7)}), op(NdOp::COPY, r(8), {n(7)})}));
}

TEST_F(LLVMModel, InitializationReadsCannotBorrowLaterStoresOrCallerBytes) {
  for (const char *Body :
       {"%old = load i64, ptr %state, align 8\n"
        "store i64 7, ptr %state, align 8\nret i64 0",
        "store i32 7, ptr %state, align 4\nret i64 0", "ret i64 0"}) {
    SCOPED_TRACE(Body);
    parse(Body, {}, {}, {}, "initializes((0,8))");
    ASSERT_TRUE(Module);
    reject("initializes contract");
  }
}

TEST_F(LLVMModel, FixedPointerAliasesSharePartialByteInitialization) {
  parse(R"(
    %base = ptrtoint ptr %state to i64
    %one = add i64 %base, 1
    %p = inttoptr i64 %one to ptr
    %two = getelementptr i8, ptr %p, i64 1
    store i16 8721, ptr %state, align 2
    store i16 17459, ptr %two, align 2
    %v = load i32, ptr %state, align 4
    %wide = zext i32 %v to i64
    %out = getelementptr i8, ptr %state, i64 8
    store i64 %wide, ptr %out, align 8
    ret i64 0
  )",
        {}, {}, {}, "initializes((0,4))");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::COPY, r(0, 4), {n(0x44332211, 4)}),
                 op(NdOp::COPY, r(8), {n(0x44332211)})}));
}

TEST_F(LLVMModel, InitializationClaimsCanCoverSeparatedByteRanges) {
  parse(R"(
    store i16 9, ptr %state, align 2
    %four = getelementptr i8, ptr %state, i64 4
    store i16 11, ptr %four, align 2
    %all = load i64, ptr %state, align 8
    %out = getelementptr i8, ptr %state, i64 8
    store i64 %all, ptr %out, align 8
    ret i64 0
  )",
        {}, {}, {}, "initializes((0,2),(4,6))");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::COPY, r(0, 2), {n(9, 2)}),
                 op(NdOp::COPY, r(4, 2), {n(11, 2)}),
                 op(NdOp::COPY, r(8), {r(0)})}));
}

TEST_F(LLVMModel, EveryBranchMustInitializeBeforeTheJoinedRead) {
  const std::string Prefix = R"(
    %input = getelementptr i8, ptr %state, i64 8
    %v = load i64, ptr %input, align 8
    %c = icmp eq i64 %v, 0
    br i1 %c, label %left, label %right
  left:
    store i64 7, ptr %state, align 8
    br label %join
  right:
  )";
  const std::string Suffix = R"(
    br label %join
  join:
    %read = load i64, ptr %state, align 8
    ret i64 0
  )";
  for (bool Both : {true, false}) {
    parse(Prefix + (Both ? "store i64 7, ptr %state, align 8\n" : "") + Suffix,
          {}, {}, {}, "initializes((0,8))");
    ASSERT_TRUE(Module);
    if (Both)
      expect(Oracle({op(NdOp::COPY, r(0), {n(7)})}));
    else
      reject("read before initialization");
  }
}

TEST_F(LLVMModel, InitializationIsRequiredOnEachNormalReturn) {
  parse(R"(
    %input = getelementptr i8, ptr %state, i64 8
    %v = load i64, ptr %input, align 8
    %c = icmp eq i64 %v, 0
    br i1 %c, label %left, label %right
  left:
    store i64 7, ptr %state, align 8
    ret i64 0
  right:
    ret i64 0
  )",
        {}, {}, {}, "initializes((0,8))");
  ASSERT_TRUE(Module);
  reject("undef bytes at return");
}

TEST_F(LLVMModel, BackedgeWritesCannotInitializeTheFirstIteration) {
  parse(R"(
  entry:
    br label %loop
  loop:
    %count = phi i64 [2, %entry], [%next, %loop]
    %old = load i64, ptr %state, align 8
    store i64 7, ptr %state, align 8
    %next = sub i64 %count, 1
    %done = icmp eq i64 %next, 0
    br i1 %done, label %exit, label %loop
  exit:
    ret i64 0
  )",
        {}, {}, {}, "initializes((0,8))");
  ASSERT_TRUE(Module);
  reject("read before initialization");
}

TEST_F(LLVMModel, LoopStoreBeforeReadMeetsInitializationContract) {
  parse(R"(
  entry:
    br label %loop
  loop:
    %count = phi i64 [2, %entry], [%next, %loop]
    store i64 7, ptr %state, align 8
    %value = load i64, ptr %state, align 8
    %next = sub i64 %count, 1
    %done = icmp eq i64 %next, 0
    br i1 %done, label %exit, label %loop
  exit:
    %out = getelementptr i8, ptr %state, i64 8
    store i64 %value, ptr %out, align 8
    ret i64 0
  )",
        {}, {}, {}, "initializes((0,8))");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::COPY, r(0), {n(7)}), op(NdOp::COPY, r(8), {n(7)})}));
}

TEST_F(LLVMModel, GuestWritesAndUnknownStateAliasesDoNotInitializeState) {
  for (const char *Body : {R"(
           %slot = getelementptr i8, ptr %state, i64 32
           %address = load i64, ptr %slot, align 8
           %guest = inttoptr i64 %address to ptr
           store i64 7, ptr %guest, align 1
           ret i64 0
         )",
                           R"(
           %p = select i1 true, ptr %state, ptr %state
           store i64 7, ptr %p, align 8
           ret i64 0
         )"}) {
    parse(Body, {}, {}, {}, "initializes((0,8))");
    ASSERT_TRUE(Module);
    reject("undef bytes at return");
  }
  // Even after a normal initializing store, an unknown state-derived
  // pointer must not become an ordinary guest pointer in the importer.
  parse(R"(
    store i64 7, ptr %state, align 8
    %p = select i1 true, ptr %state, ptr %state
    %v = load i64, ptr %p, align 8
    ret i64 0
  )",
        {}, {}, {}, "initializes((0,8))");
  ASSERT_TRUE(Module);
  reject();
}

TEST_F(LLVMModel, SpecialWritesAndOutOfObjectInitializationAreRefused) {
  for (const char *Body :
       {"store volatile i64 7, ptr %state, align 8\nret i64 0",
        "store atomic i64 7, ptr %state monotonic, align 8\nret i64 0"}) {
    parse(Body, {}, {}, {}, "initializes((0,8))");
    ASSERT_TRUE(Module);
    reject("initializes store effect");
  }
  for (const char *Ranges : {"initializes((-1,8))", "initializes((128,137))",
                             "initializes((0,1000000000))"}) {
    parse("ret i64 0", {}, {}, {}, Ranges);
    ASSERT_TRUE(Module);
    reject("inside the state object");
  }
}

TEST_F(LLVMModel, InitializationRangeListsAreChargedBeforeVerification) {
  std::string Attribute = "initializes(";
  for (unsigned I = 0; I != 1024; ++I) {
    if (I)
      Attribute += ',';
    Attribute +=
        '(' + std::to_string(I * 2) + ',' + std::to_string(I * 2 + 1) + ')';
  }
  Attribute += ')';
  parse("ret i64 0", {}, {}, {}, Attribute);
  ASSERT_TRUE(Module);
  LLVMInterpreterModelLimits Limits;
  Limits.MaxInputItems = 128;
  auto M = model(Limits);
  ASSERT_FALSE(bool(M));
  EXPECT_NE(llvm::toString(M.takeError()).find("budget"), std::string::npos);
}

TEST_F(LLVMModel, InitializationDataflowUsesTheSharedWorkBudget) {
  const std::string Body = "store i64 7, ptr %state, align 8\nret i64 0";
  parse(Body);
  ASSERT_TRUE(Module);
  uint64_t FirstSuccess = 0;
  for (uint64_t Work = 1; Work != 1024; ++Work) {
    LLVMInterpreterModelLimits Limits;
    Limits.MaxWork = Work;
    auto M = model(Limits);
    if (M) {
      FirstSuccess = Work;
      break;
    }
    llvm::consumeError(M.takeError());
  }
  ASSERT_NE(FirstSuccess, 0U);
  parse(Body, {}, {}, {}, "initializes((0,8))");
  ASSERT_TRUE(Module);
  LLVMInterpreterModelLimits Limits;
  Limits.MaxWork = FirstSuccess + 2; // Covers the extra attribute metadata.
  auto M = model(Limits);
  ASSERT_FALSE(bool(M));
  EXPECT_NE(llvm::toString(M.takeError()).find("budget"), std::string::npos);
  expect(Oracle({op(NdOp::COPY, r(0), {n(7)})}));
}
} // namespace neverd::analysis::llvm_model_test
