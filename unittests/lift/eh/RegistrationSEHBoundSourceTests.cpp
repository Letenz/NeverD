//===- RegistrationSEHBoundSourceTests.cpp - Frame observer refusals -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Reject preserved SEH helpers that observe their physical caller frame.

#include "RegistrationSEHJumpNativeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Module.h"

#include <array>

namespace neverd::registration_test {

void checkSEHBoundSourceEdits(const llvm::Function &Parent,
                              const ExceptionFunction &Source,
                              const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  va_t Helper = InvalidVA;
  for (const auto &Function : *Parent.getParent())
    if (Function.getName().starts_with("__nd_seh_bound_")) {
      auto Address = rewrite_source::getOriginalVA(Function);
      ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
      ASSERT_TRUE(*Address);
      ASSERT_EQ(Helper, InvalidVA);
      Helper = **Address;
    }
  ASSERT_NE(Helper, InvalidVA);
  const auto *EH = Image.ExceptionMetadata.findFunction(Helper);
  ASSERT_TRUE(EH);
  std::array<uint8_t, 8> Padding;
  Padding.fill(0x90);
  uint8_t *Window = nullptr;
  for (va_t Address = Helper; Address + Padding.size() <= EH->CodeRange.End;
       ++Address)
    if (const auto *Bytes = Image.readVA(Address, Padding.size());
        Bytes && std::equal(Padding.begin(), Padding.end(), Bytes)) {
      Window = const_cast<uint8_t *>(Bytes);
      break;
    }
  ASSERT_TRUE(Window);
  auto Restore = llvm::scope_exit(
      [&] { std::copy(Padding.begin(), Padding.end(), Window); });
  // Each same-size branch preserves the registration protocol. The source
  // solver must still complete, while native admission rejects dependence on
  // EBP, saved caller EBP, return PC, previous chain, or incoming EBX,
  // including a narrow alias. Both successors contain valid source
  // instructions.
  const std::array<std::array<uint8_t, 8>, 5> Mutations{{
      {{0x85, 0xed, 0x74, 0x02, 0x90, 0x90, 0x90, 0x90}},
      {{0x8b, 0x45, 0x00, 0x3c, 0x00, 0x74, 0x01, 0x90}},
      {{0x8b, 0x45, 0x04, 0x85, 0xc0, 0x74, 0x01, 0x90}},
      {{0x8b, 0x45, 0xf0, 0x85, 0xc0, 0x74, 0x01, 0x90}},
      {{0x85, 0xdb, 0x74, 0x02, 0x90, 0x90, 0x90, 0x90}},
  }};
  for (size_t Index = 0; Index != Mutations.size(); ++Index) {
    SCOPED_TRACE(Index);
    std::copy(Mutations[Index].begin(), Mutations[Index].end(), Window);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    const auto Low =
        CFGBuilder().build(Image, Dec, Source.CodeRange.Begin, "observer");
    ASSERT_TRUE(Low.RegistrationStates);
    const auto &States = *Low.RegistrationStates;
    ASSERT_TRUE(States.Complete);
    EXPECT_TRUE(States.HasBoundSEHCallees);
    EXPECT_FALSE(States.SEHBoundCallsComplete);
    EXPECT_TRUE(States.SEHBoundCalls.empty());
    auto Error = validateCOFFRegistrationIR(Parent, Source, Image);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
#endif
}

} // namespace neverd::registration_test
