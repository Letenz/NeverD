//===- LLVMInterpreterModelGuardTests.cpp - Definedness obligations
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMInterpreterModelTest.h"

namespace neverd::analysis::llvm_model_test {
TEST_F(LLVMModel, ArithmeticFlagsAreProvedRatherThanAssumed) {
  struct Case {
    const char *Operation;
    NdOp Code;
    uint64_t RHS, Good, Bad;
  };
  const Case Cases[] = {
      {"add nsw", NdOp::INT_ADD, 1, 7, INT64_MAX},
      {"add nuw", NdOp::INT_ADD, 1, 7, UINT64_MAX},
      {"sub nsw", NdOp::INT_SUB, 1, 7, uint64_t{1} << 63},
      {"sub nuw", NdOp::INT_SUB, 1, 7, 0},
      {"shl nsw", NdOp::INT_LEFT, 3, 7, uint64_t{1} << 60},
      {"shl nuw", NdOp::INT_LEFT, 3, 7, uint64_t{1} << 61},
      {"lshr exact", NdOp::INT_RIGHT, 3, 16, 9},
      {"ashr exact", NdOp::INT_ASHR, 3, uint64_t(-16), uint64_t(-17)},
      {"or disjoint", NdOp::INT_OR, 3, 4, 1}};
  for (const auto &K : Cases) {
    SCOPED_TRACE(K.Operation);
    parse(std::string("%x = load i64, ptr %state, align 8\n%y = ") +
          K.Operation + " i64 %x, " + std::to_string(K.RHS) +
          "\nstore i64 %y, ptr %state, align 8\nret i64 0");
    ASSERT_TRUE(Module);
    Oracle Reference({op(K.Code, r(0), {r(0), n(K.RHS)})});
    auto C = llvmInterpreterMachineStateContract();
    C.EntryConstants.push_back({r(0), K.Good});
    expect(Reference, Status::Proved, C);
    C.EntryConstants.back().Value = K.Bad;
    expect(Reference, Status::ContractViolation, C);
  }
}

TEST_F(LLVMModel, TruncationFlagsAndNonnegativeExtensionAreChecked) {
  struct Case {
    const char *Expression;
    const char *ResultType;
    uint64_t Good, Bad, Expected;
  };
  const Case Cases[] = {
      {"trunc nuw i64 %x to i8", "i8", 255, 256, 255},
      {"trunc nsw i64 %x to i8", "i8", uint64_t(-128), 128, 128},
      {"trunc nuw i64 %x to i1", "i1", 1, 2, 1},
      {"trunc nsw i64 %x to i1", "i1", UINT64_MAX, 1, 1}};
  for (const auto &K : Cases) {
    SCOPED_TRACE(K.Expression);
    parse(std::string("%x = load i64, ptr %state, align 8\n%v = ") +
          K.Expression + "\n%z = zext " + K.ResultType +
          " %v to i64\nstore i64 %z, ptr %state, align 8\nret i64 0");
    ASSERT_TRUE(Module);
    auto C = llvmInterpreterMachineStateContract();
    C.EntryConstants.push_back({r(0), K.Good});
    Oracle Reference({op(NdOp::COPY, r(0), {n(K.Expected)})});
    expect(Reference, Status::Proved, C);
    C.EntryConstants.back().Value = K.Bad;
    expect(Reference, Status::ContractViolation, C);
  }
  for (unsigned Width : {1U, 8U}) {
    std::string T = "i" + std::to_string(Width);
    parse("%x = load i64, ptr %state, align 8\n%v = trunc i64 %x to " + T +
          "\n%z = zext nneg " + T +
          " %v to i64\nstore i64 %z, ptr %state, align 8\nret i64 0");
    ASSERT_TRUE(Module);
    auto C = llvmInterpreterMachineStateContract();
    C.EntryConstants.push_back({r(0), 0});
    expect(Oracle({op(NdOp::COPY, r(0), {n(0)})}), Status::Proved, C);
    C.EntryConstants.back().Value = uint64_t{1} << (Width - 1);
    expect(Oracle(), Status::ContractViolation, C);
  }
}

TEST_F(LLVMModel, SameSignComparisonAndWrappingReturnRangesAreChecked) {
  parse(R"(
    %x = load i64, ptr %state, align 8
    %b = icmp samesign ult i64 %x, 4
    %v = zext i1 %b to i64
    store i64 %v, ptr %state, align 8
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  auto C = llvmInterpreterMachineStateContract();
  C.EntryConstants.push_back({r(0), 1});
  expect(Oracle({op(NdOp::COPY, r(0), {n(1)})}), Status::Proved, C);
  C.EntryConstants.back().Value = UINT64_MAX;
  expect(Oracle(), Status::ContractViolation, C);

  for (const char *Range : {"range(i64 2, 5)", "range(i64 -2, 2)"}) {
    SCOPED_TRACE(Range);
    parse("%x = load i64, ptr %state, align 8\nret i64 %x", {}, {}, Range);
    ASSERT_TRUE(Module);
    C.EntryConstants.back().Value = Range[10] == '2' ? 3 : UINT64_MAX;
    expect(Oracle({}, r(0)), Status::Proved, C);
    C.EntryConstants.back().Value = 9;
    expect(Oracle({}, r(0)), Status::ContractViolation, C);
  }
}

TEST_F(LLVMModel, CtpopRangeAndSignedOverflowIntrinsics) {
  parse(R"(
    %x = load i64, ptr %state, align 8
    %p = call range(i64 0, 5) i64 @llvm.ctpop.i64(i64 %x)
    store i64 %p, ptr %state, align 8
    ret i64 0
  )",
        "declare i64 @llvm.ctpop.i64(i64)");
  ASSERT_TRUE(Module);
  auto C = llvmInterpreterMachineStateContract();
  C.EntryConstants.push_back({r(0), 0x55});
  expect(Oracle({op(NdOp::COPY, r(0), {n(4)})}), Status::Proved, C);
  C.EntryConstants.back().Value = 0xff;
  expect(Oracle({op(NdOp::COPY, r(0), {n(8)})}), Status::ContractViolation, C);

  for (auto [Name, Code, Overflow] :
       {std::tuple{"sadd", NdOp::INT_ADD, NdOp::INT_SOVF},
        std::tuple{"ssub", NdOp::INT_SUB, NdOp::INT_SBOR}}) {
    SCOPED_TRACE(Name);
    std::string Intrinsic = std::string("@llvm.") + Name + ".with.overflow.i64";
    parse("%x = load i64, ptr %state, align 8\n"
          "%pair = call {i64, i1} " +
              Intrinsic +
              "(i64 %x, i64 1)\n"
              "%sum = extractvalue {i64, i1} %pair, 0\n"
              "%overflow = extractvalue {i64, i1} %pair, 1\n"
              "%status = zext i1 %overflow to i64\n"
              "store i64 %sum, ptr %state, align 8\nret i64 %status",
          "declare {i64, i1} " + Intrinsic + "(i64, i64)");
    ASSERT_TRUE(Module);
    expect(Oracle({op(Overflow, r(256, 1), {r(0), n(1)}),
                   op(NdOp::INT_ZEXT, r(264), {r(256, 1)}),
                   op(Code, r(0), {r(0), n(1)})},
                  r(264)));
  }
}

TEST_F(LLVMModel, StrictDefinednessRejectsDeadOrMaskedPoison) {
  for (const char *Use : {"store i64 0, ptr %state, align 8",
                          "%masked = select i1 false, i64 %bad, i64 0\n"
                          "store i64 %masked, ptr %state, align 8"}) {
    parse(std::string("%x = load i64, ptr %state, align 8\n") +
          "%bad = add nuw i64 %x, 1\n" + Use + "\nret i64 0");
    ASSERT_TRUE(Module);
    auto C = llvmInterpreterMachineStateContract();
    C.EntryConstants.push_back({r(0), UINT64_MAX});
    expect(Oracle({op(NdOp::COPY, r(0), {n(0)})}), Status::ContractViolation,
           C);
  }
}
} // namespace neverd::analysis::llvm_model_test
