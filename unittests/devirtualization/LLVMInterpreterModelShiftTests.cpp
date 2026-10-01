//===- LLVMInterpreterModelShiftTests.cpp - Variable shift obligations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMInterpreterModelTest.h"

namespace neverd::analysis::llvm_model_test {
namespace {
constexpr std::pair<const char *, NdOp> Shifts[] = {{"shl", NdOp::INT_LEFT},
                                                    {"lshr", NdOp::INT_RIGHT},
                                                    {"ashr", NdOp::INT_ASHR}};

std::string shiftBody(const char *Name, unsigned Bits, bool Masked = false) {
  const std::string T = "i" + std::to_string(Bits);
  std::string Body = "%x = load " + T +
                     ", ptr %state, align 1\n"
                     "%p = getelementptr i8, ptr %state, i64 8\n"
                     "%count = load " +
                     T + ", ptr %p, align 1\n";
  if (Masked)
    Body +=
        "%bounded = and " + T + " %count, " + std::to_string(Bits - 1) + "\n";
  return Body + "%result = " + Name + " " + T + " %x, " +
         (Masked ? "%bounded" : "%count") + "\nstore " + T +
         " %result, ptr %state, align 1\nret i64 0";
}
} // namespace

TEST_F(LLVMModel, MaskedVariableShiftsMatchIndependentFullStateOracles) {
  for (unsigned Bits : {8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const auto Bytes = static_cast<uint16_t>(Bits / 8);
    for (auto [Name, Code] : Shifts) {
      SCOPED_TRACE(Name);
      parse(shiftBody(Name, Bits, true));
      ASSERT_TRUE(Module);
      // Every input word is symbolic; partial stores must preserve its high
      // bytes and all other state words, including the original count.
      expect(Oracle(
          {op(NdOp::INT_AND, r(256, Bytes), {r(8, Bytes), n(Bits - 1, Bytes)}),
           op(Code, r(0, Bytes), {r(0, Bytes), r(256, Bytes)})}));
    }
  }
}

TEST_F(LLVMModel, VariableShiftAmountsMustBeWithinSourceWidth) {
  for (unsigned Bits : {8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const auto Bytes = static_cast<uint16_t>(Bits / 8);
    for (auto [Name, Code] : Shifts) {
      SCOPED_TRACE(Name);
      parse(shiftBody(Name, Bits));
      ASSERT_TRUE(Module);
      const Oracle Reference(
          {op(Code, r(0, Bytes), {r(0, Bytes), r(8, Bytes)})});
      auto C = llvmInterpreterMachineStateContract();
      C.EntryConstants.push_back({r(8, Bytes), 0});
      for (uint64_t Count : {uint64_t{0}, uint64_t{Bits - 1}}) {
        SCOPED_TRACE(Count);
        C.EntryConstants.back().Value = Count;
        expect(Reference, Status::Proved, C);
      }
      // Large counts cannot be silently masked to the host/guest ISA width.
      for (uint64_t Count :
           {uint64_t{Bits}, uint64_t{Bits + 1}, UINT64_MAX >> (64 - Bits)}) {
        SCOPED_TRACE(Count);
        C.EntryConstants.back().Value = Count;
        expect(Reference, Status::ContractViolation, C);
      }
    }
  }
}

TEST_F(LLVMModel, VariableShiftFlagsRetainTheirDefinednessObligations) {
  struct Case {
    const char *Name;
    NdOp Code;
    uint64_t Good, Bad;
  };
  for (unsigned Bits : {8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const auto Bytes = static_cast<uint16_t>(Bits / 8);
    const Case Cases[] = {
        {"shl nuw", NdOp::INT_LEFT, 7, uint64_t{1} << (Bits - 3)},
        {"shl nsw", NdOp::INT_LEFT, 7, uint64_t{1} << (Bits - 4)},
        {"lshr exact", NdOp::INT_RIGHT, 16, 9},
        {"ashr exact", NdOp::INT_ASHR, uint64_t(-16), uint64_t(-17)}};
    for (const auto &K : Cases) {
      SCOPED_TRACE(K.Name);
      parse(shiftBody(K.Name, Bits));
      ASSERT_TRUE(Module);
      const Oracle Reference(
          {op(K.Code, r(0, Bytes), {r(0, Bytes), r(8, Bytes)})});
      auto C = llvmInterpreterMachineStateContract();
      C.EntryConstants.push_back({r(8, Bytes), 3});
      C.EntryConstants.push_back(
          {r(0, Bytes), K.Good & (UINT64_MAX >> (64 - Bits))});
      expect(Reference, Status::Proved, C);
      C.EntryConstants.back().Value = K.Bad & (UINT64_MAX >> (64 - Bits));
      expect(Reference, Status::ContractViolation, C);
    }
  }
}

TEST_F(LLVMModel, OutOfRangeDynamicShiftsRemainStrictEvenWhenUnused) {
  for (auto [Name, Code] : Shifts) {
    SCOPED_TRACE(Name);
    for (const char *Use : {"store i64 0, ptr %state, align 8",
                            "%masked = select i1 false, i64 %bad, i64 0\n"
                            "store i64 %masked, ptr %state, align 8"}) {
      parse(std::string("%count = load i64, ptr %state, align 8\n%bad = ") +
            Name + " i64 0, %count\n" + Use + "\nret i64 0");
      ASSERT_TRUE(Module);
      auto C = llvmInterpreterMachineStateContract();
      C.EntryConstants.push_back({r(0), 64});
      expect(Oracle({op(NdOp::COPY, r(0), {n(0)})}), Status::ContractViolation,
             C);
    }
  }
}

TEST_F(LLVMModel, BranchGuardedVariableShiftsProveForEveryEntryCount) {
  for (auto [Name, Code] : Shifts) {
    SCOPED_TRACE(Name);
    parse(std::string(R"(
      %x = load i64, ptr %state, align 8
      %p = getelementptr i8, ptr %state, i64 8
      %count = load i64, ptr %p, align 8
      %valid = icmp ult i64 %count, 64
      br i1 %valid, label %shift, label %large
    shift:
      %shifted = )") +
          Name + R"( i64 %x, %count
      br label %exit
    large:
      br label %exit
    exit:
      %result = phi i64 [%shifted, %shift], [0, %large]
      store i64 %result, ptr %state, align 8
      ret i64 0
    )");
    ASSERT_TRUE(Module);
    expect(Oracle({op(NdOp::INT_AND, r(256), {r(8), n(63)}),
                   op(Code, r(264), {r(0), r(256)}),
                   op(NdOp::INT_LESS, r(272, 1), {r(8), n(64)}),
                   op(NdOp::SELECT, r(0), {r(272, 1), r(264), n(0)})}));
  }
}
} // namespace neverd::analysis::llvm_model_test
