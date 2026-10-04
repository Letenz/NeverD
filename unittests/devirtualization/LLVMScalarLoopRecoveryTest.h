//===- LLVMScalarLoopRecoveryTest.h - Shared loop candidate checks --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_LLVM_SCALAR_LOOP_RECOVERY_TEST_H
#define NEVERD_UNITTESTS_LLVM_SCALAR_LOOP_RECOVERY_TEST_H

#include "LLVMScalarEquivalenceTest.h"

#include "neverd/analysis/LLVMScalarLoopRecovery.h"

#include "llvm/IR/Verifier.h"

namespace neverd::analysis::scalar_test {
using RecoveryStatus = LLVMScalarLoopRecoveryStatus;

inline void replace(std::string &IR, llvm::StringRef From, llvm::StringRef To) {
  auto At = IR.find(From.str());
  ASSERT_NE(At, std::string::npos) << From.str();
  IR.replace(At, From.size(), To.str());
}

inline std::string text(const llvm::Module &M) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  M.print(OS, nullptr);
  return S;
}
inline LLVMScalarLoopRecoveryResult
recover(Source &Input, const LLVMScalarLoopRecoveryLimits &L = {}) {
  auto Before = Input.text();
  auto R = recoverLLVMScalarLoops(Input.function(), L);
  EXPECT_EQ(Input.text(), Before);
  EXPECT_LE(R.ConstructionWork, L.MaxConstructionWork);
  EXPECT_LE(R.ProofWork, L.MaxProofWork);
  EXPECT_LE(R.Candidates, L.MaxCandidates);
  if (R.Module) {
    EXPECT_EQ(R.Status, RecoveryStatus::Recovered);
    EXPECT_FALSE(llvm::verifyModule(*R.Module));
    auto Proof = checkLLVMScalarEquivalence(Input.function(),
                                            *R.Module->getFunction("f"));
    EXPECT_EQ(Proof.Status, Status::Proved) << Proof.Diagnostic;
  } else {
    EXPECT_NE(R.Status, RecoveryStatus::Recovered);
  }
  return R;
}
} // namespace neverd::analysis::scalar_test
#endif
