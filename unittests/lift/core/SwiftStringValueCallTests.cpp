#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "RuntimeFunctionAddressFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
using namespace neverd;

TEST(ObjCCallHints, SwiftUTF8CStringKeepsBothStringWordsAndBufferResult) {
  using namespace runtime_function_address_test;
  const std::string Name = "$sSS11utf8CStrings15ContiguousArrayVys4Int8VGvg";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.ImportPtrSlots[Slot] = "_" + Name;
    Image.DyldBindSlots[Slot] = {"_" + Name, 0,
                                 "/usr/lib/swift/libswiftCore.dylib", false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
    ASSERT_TRUE(Hint);
    const auto &Signature = Hint->Signature;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_EQ(Signature.Parameters.size(), 2U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);
    for (unsigned I = 0; I < 2; ++I) {
      EXPECT_EQ(Signature.Parameters[I].TheRole,
                SourceParameterTypeHint::Role::Ordinary);
      EXPECT_EQ(Signature.Parameters[I].Type->Size, 8U);
      EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                TRI.IntParamRegs[I]);
    }
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.ReturnType->Size, 8U);
    EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, TRI.IntReturnReg);
    EXPECT_TRUE(Signature.ReturnComponents.empty());
    auto Call =
        HighExpr::makeCall("untrusted", Slot,
                           {HighExpr::makeConst(0, 8),
                            HighExpr::makeConst(0xe000000000000000, 8)});
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Wrong = Image;
      if (Mutation == 0)
        Wrong.DyldBindSlots[Slot].Module = "/tmp/libswiftCore.dylib";
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
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, Slot));
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {}));
    }
    auto Changed = *Hint;
    Changed.Signature.Parameters[0].Type = NdType::makeInt(4);
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Changed);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
  }
}

TEST(ObjCCallHints, SwiftStringValueOperationsKeepContextAndResultCarriers) {
  using namespace runtime_function_address_test;
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Reserve : {false, true}) {
      const std::string Name = Reserve
                                   ? "$sSS15reserveCapacityyySiF"
                                   : "$s10Foundation3URLV14absoluteStringSSvg";
      const std::vector<std::string> Providers =
          Reserve
              ? std::vector<std::string>{"/usr/lib/swift/libswiftCore.dylib"}
              : std::vector<std::string>{
                    "/System/Library/Frameworks/Foundation.framework/"
                    "Foundation",
                    "/System/Library/Frameworks/Foundation.framework/Versions/"
                    "C/Foundation",
                    "/usr/lib/swift/libswiftFoundation.dylib"};
      for (const auto &Provider : Providers) {
        auto Image = image(Architecture);
        Image.ImportPtrSlots[Slot] = "_" + Name;
        Image.DyldBindSlots[Slot] = {"_" + Name, 0, Provider, false};
        const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
        ASSERT_TRUE(Hint);
        const auto &Signature = Hint->Signature;
        const auto &TRI = getTargetRegInfo(Architecture);
        EXPECT_EQ(Signature.Origin,
                  SourceFunctionTypeHint::OriginKind::SwiftSDK);
        EXPECT_EQ(Signature.Convention,
                  SourceFunctionTypeHint::ConventionKind::Swift);
        EXPECT_EQ(Signature.ReturnType->Kind,
                  Reserve ? NdTypeKind::Void : NdTypeKind::Int);
        if (!Reserve) {
          EXPECT_EQ(Signature.ReturnType->Size, 16U);
          EXPECT_EQ(Signature.ReturnLocation.Kind, SourceABICarrierKind::None);
          ASSERT_EQ(Signature.ReturnComponents.size(), 2U);
          EXPECT_EQ(Signature.ReturnComponents[0].RegisterOffset,
                    TRI.IntReturnReg);
          EXPECT_EQ(Signature.ReturnComponents[1].RegisterOffset,
                    Architecture == Arch::AArch64 ? a64reg::X1 : x86reg::RDX);
        }
        ASSERT_EQ(Signature.Parameters.size(), Reserve ? 2U : 1U);
        const auto &Context = Signature.Parameters.back();
        EXPECT_EQ(Context.TheRole, SourceParameterTypeHint::Role::SwiftContext);
        EXPECT_EQ(Context.Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Context.Type->Size, 8U);
        EXPECT_EQ(Context.Location.RegisterOffset,
                  Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
        if (Reserve) {
          EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
          EXPECT_EQ(Signature.Parameters[0].Type->Size, 8U);
          EXPECT_EQ(Signature.Parameters[0].TheRole,
                    SourceParameterTypeHint::Role::Ordinary);
          EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                    TRI.IntParamRegs[0]);
        }
        std::string Diagnostic;
        EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
        auto Call =
            HighExpr::makeCall("untrusted", Slot,
                               std::vector<ExprPtr>(Signature.Parameters.size(),
                                                    HighExpr::makeConst(0, 8)));
        Call->Type = Signature.ReturnType;
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
        for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
          auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
          if (Mutation == 0)
            Changed->Signature.Parameters.back().TheRole =
                SourceParameterTypeHint::Role::Ordinary;
          if (Mutation == 1)
            Changed->Signature.Parameters.back().Location.RegisterOffset =
                TRI.IntParamRegs[0];
          if (Mutation == 2)
            Changed->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
          if (Mutation == 3)
            Changed->Signature.Parameters.pop_back();
          if (Mutation == 4)
            Changed->SwiftStringInputs = {{0, 1}};
          Call->SourceCallHint = Changed;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
        }
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
          auto Wrong = Image;
          if (Mutation == 0)
            Wrong.DyldBindSlots[Slot].Module = "/tmp/Foundation";
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
