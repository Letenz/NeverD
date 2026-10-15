//===- RegistrationSEHBoundCookieTests.cpp - EH4 callee cookie proofs
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationSEHJumpNativeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ScopeExit.h"

#include <vector>

namespace neverd::registration_test {

void checkSEHBoundCookieEdits(const llvm::Function &Parent,
                              const ExceptionFunction &Source,
                              const ExceptionFunction &Helper,
                              const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  if (Helper.Personality != ExceptionPersonality::ExceptHandler4)
    return;
  ASSERT_TRUE(Helper.Registration);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  const auto Low = CFGBuilder().build(Image, Dec, Helper.CodeRange.Begin,
                                      "bound-eh4-cookie");
  struct Edit {
    uint8_t *Byte;
    uint8_t Replacement;
    bool Required = true;
  };
  std::vector<Edit> Edits;
  unsigned CookieLoads = 0, EHStores = 0, GSStores = 0, GSChecks = 0;
  for (const auto &Block : Low.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries) {
      auto *Bytes =
          const_cast<uint8_t *>(Image.readVA(Insn.Address, Insn.Size));
      ASSERT_TRUE(Bytes);
      if (Insn.Size == 5 && Bytes[0] == 0xa1 &&
          readLE<uint32_t>(Bytes + 1) ==
              Image.Base + Image.DynInfo.SecurityCookieRVA) {
        ++CookieLoads;
        // Partial reads, writes and a different global cannot supply a cookie.
        Edits.push_back({Bytes, 0xa0});
        Edits.push_back({Bytes, 0xa3});
        Edits.push_back({Bytes + 1, uint8_t(Bytes[1] ^ 1)});
      }
      if (Insn.Size == 3 && Bytes[0] == 0x89 && Bytes[1] == 0x45 &&
          (Bytes[2] == 0xe4 || Bytes[2] == 0xe0)) {
        (Bytes[2] == 0xe4 ? EHStores : GSStores)++;
        // Initialized bytes beside a cookie do not initialize the cookie.
        Edits.push_back({Bytes + 2, uint8_t(Bytes[2] - 1)});
        Edits.push_back({Bytes + 2, uint8_t(Bytes[2] + 1)});
      }
      if (Insn.Size == 3 && Bytes[0] == 0x31 && Bytes[1] == 0x45 &&
          Bytes[2] == 0xf8)
        Edits.push_back({Bytes, 0x01});
      if (Insn.Size == 2 && Bytes[0] == 0x31 && Bytes[1] == 0xe8)
        Edits.push_back({Bytes + 1, 0xc8});
      if (Insn.Size == 2 && Bytes[0] == 0x31 && Bytes[1] == 0xe9) {
        ++GSChecks;
        // The authenticated checker still needs the actual live ECX value.
        ASSERT_EQ(Helper.Registration->Scopes.size(), 1u);
        // This fixture never activates its sole scope. The ordinary return
        // check is live; a check in its original except body is unreachable.
        Edits.push_back(
            {Bytes + 1, 0xc9,
             Insn.Address < Helper.Registration->Scopes.front().HandlerVA});
      }
    }
  EXPECT_EQ(CookieLoads, 1u);
  EXPECT_EQ(EHStores, 1u);
  const bool GS = Helper.Registration->GSCookieOffset != -2;
  EXPECT_EQ(GSStores, unsigned(GS));
  EXPECT_EQ(GSChecks != 0, GS);
  ASSERT_GE(Edits.size(), 7u);
  for (size_t I = 0; I != Edits.size(); ++I) {
    SCOPED_TRACE(I);
    const auto &Edit = Edits[I];
    const uint8_t Original = *Edit.Byte;
    auto Restore = llvm::scope_exit([&] { *Edit.Byte = Original; });
    *Edit.Byte = Edit.Replacement;
    const auto Changed = CFGBuilder().build(Image, Dec, Source.CodeRange.Begin,
                                            "invalid-bound-cookie");
    ASSERT_TRUE(Changed.RegistrationStates);
    EXPECT_EQ(Changed.RegistrationStates->SEHBoundCallsComplete,
              !Edit.Required);
    EXPECT_EQ(Changed.RegistrationStates->SEHBoundCalls.empty(), Edit.Required);
    auto Error = validateCOFFRegistrationIR(Parent, Source, Image);
    if (Edit.Required) {
      ASSERT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    } else
      EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  }
#endif
}

} // namespace neverd::registration_test
