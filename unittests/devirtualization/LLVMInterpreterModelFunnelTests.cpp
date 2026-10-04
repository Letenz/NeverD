//===- LLVMInterpreterModelFunnelTests.cpp - Shared intrinsic obligations -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMInterpreterModelTest.h"

namespace neverd::analysis::llvm_model_test {
TEST_F(LLVMModel, FunnelShiftsRetainDistinctZeroCountEndpoints) {
  for (unsigned Bits : {8U, 16U, 32U, 64U}) {
    const std::string T = "i" + std::to_string(Bits);
    const auto Bytes = static_cast<uint16_t>(Bits / 8);
    for (bool Left : {false, true}) {
      const std::string Name =
          std::string("llvm.") + (Left ? "fshl." : "fshr.") + T;
      for (uint64_t Count : {uint64_t{0}, uint64_t{Bits}, uint64_t{Bits * 2}}) {
        SCOPED_TRACE(T + Name + std::to_string(Count));
        parse("%a = load " + T +
                  ", ptr %state, align 1\n"
                  "%p = getelementptr i8, ptr %state, i64 8\n"
                  "%b = load " +
                  T +
                  ", ptr %p, align 1\n"
                  "%v = call noundef " +
                  T + " @" + Name + "(" + T + " %a, " + T + " %b, " + T + " " +
                  std::to_string(Count) +
                  ")\n"
                  "store " +
                  T + " %v, ptr %state, align 1\nret i64 0",
              "declare " + T + " @" + Name + "(" + T + ", " + T + ", " + T +
                  ")");
        // Use the computed result in the actual state observation.
        ASSERT_TRUE(Module);
        expect(Oracle({op(NdOp::COPY, r(0, Bytes), {r(Left ? 0 : 8, Bytes)})}));
      }
    }
  }
}
} // namespace neverd::analysis::llvm_model_test
