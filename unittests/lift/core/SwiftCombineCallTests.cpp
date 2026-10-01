#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "RuntimeFunctionAddressFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

using namespace neverd;

TEST(ObjCCallHints, SwiftPublishedInitializerKeepsOpaqueResultAndTypeMetadata) {
  using namespace runtime_function_address_test;
  const std::string Name = "$s7Combine9PublishedV12initialValueACyxGx_tcfC";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Provider :
         {"/System/Library/Frameworks/Combine.framework/Combine",
          "/System/Library/Frameworks/Combine.framework/Versions/A/Combine"}) {
      auto Image = image(Architecture);
      Image.ImportPtrSlots[Slot] = "_" + Name;
      Image.DyldBindSlots[Slot] = {"_" + Name, 0, Provider, false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
      ASSERT_TRUE(Hint);
      const auto &Signature = Hint->Signature;
      const auto &TRI = getTargetRegInfo(Architecture);
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
      ASSERT_EQ(Signature.Parameters.size(), 3U);
      std::vector<ExprPtr> Arguments;
      for (unsigned I = 0; I < 3; ++I) {
        const auto &Parameter = Signature.Parameters[I];
        EXPECT_EQ(Parameter.Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Parameter.Type->Size, 8U);
        EXPECT_EQ(Parameter.TheRole,
                  I == 0 ? SourceParameterTypeHint::Role::SwiftIndirectResult
                         : SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Parameter.Location.RegisterOffset,
                  I == 0 ? TRI.indirectResultReg() : TRI.IntParamRegs[I - 1]);
        Arguments.push_back(HighExpr::makeConst(0, 8));
      }
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
      auto Call = HighExpr::makeCall("untrusted", Slot, Arguments);
      Call->Type = Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
        if (Mutation == 0)
          Changed->Signature.Parameters[0].TheRole =
              SourceParameterTypeHint::Role::Ordinary;
        if (Mutation == 1)
          Changed->Signature.Parameters[0].Location.RegisterOffset =
              TRI.IntParamRegs[0];
        if (Mutation == 2)
          Changed->Signature.Parameters[2].TheRole =
              SourceParameterTypeHint::Role::SwiftContext;
        if (Mutation == 3)
          Changed->Signature.Parameters.pop_back();
        if (Mutation == 4)
          Changed->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
        Call->SourceCallHint = Changed;
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
      }
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
        auto Wrong = Image;
        if (Mutation == 0)
          Wrong.DyldBindSlots[Slot].Module = "/tmp/Combine.framework/Combine";
        if (Mutation == 1)
          Wrong.DyldBindSlots[Slot].WeakImport = true;
        if (Mutation == 2)
          Wrong.DyldBindSlots[Slot].Addend = 8;
        if (Mutation == 3)
          Wrong.DyldBindSlots[Slot].Name += "invalid";
        if (Mutation == 4)
          Wrong.DyldBindSlots.erase(Slot);
        if (Mutation == 5)
          Wrong.ConflictingImportStorageSlots.insert(Slot);
        EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, Slot)) << Mutation;
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
      }
    }
  }
}
