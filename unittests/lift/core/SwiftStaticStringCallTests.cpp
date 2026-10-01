#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "RuntimeFunctionAddressFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftStringCalls.h"

using namespace neverd;
namespace {
constexpr const char *Description = "$ss12StaticStringV11descriptionSSvg";
constexpr const char *Format =
    "$sSS10FoundationE6format9argumentsS2Sh_Says7CVarArg_pGhtcfC";

BinaryImage staticStringImage(Arch Architecture) {
  using namespace runtime_function_address_test;
  auto Image = image(Architecture);
  Image.ImportPtrSlots[Slot] = std::string("_") + Description;
  Image.DyldBindSlots[Slot].Name = Image.ImportPtrSlots[Slot];
  Segment Data;
  Data.VA = 0x3000;
  Data.FileOff = 8;
  Data.Size = Data.FileSz = 32;
  Data.Flags = SegmentFlags::Readable;
  Data.Data = {'a', 0, 'b', 0xc3, 0xa9};
  Data.Data.resize(32);
  Image.Segments.push_back(Data);
  Section Bytes;
  Bytes.VA = Data.VA;
  Bytes.FileOff = Data.FileOff;
  Bytes.Size = Bytes.FileSz = Data.Size;
  Bytes.Flags = Data.Flags;
  Image.Sections.push_back(Bytes);
  return Image;
}

HighFunc staticStringCaller(const BinaryImage &Image, uint64_t Data,
                            uint64_t Count, uint8_t Flags) {
  using namespace runtime_function_address_test;
  const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
  HighFunc Function;
  Function.Name = "static_string_description";
  Function.Entry = 0x1000;
  if (!Hint)
    return Function;
  Function.ReturnType = Hint->Signature.ReturnType;
  auto Call = HighExpr::makeCall("untrusted", Slot,
                                 {HighExpr::makeConst(Data, 8),
                                  HighExpr::makeConst(Count, 8),
                                  HighExpr::makeConst(Flags, 1)});
  Call->Type = Function.ReturnType;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  HighStmt Statement;
  Statement.Kind = StmtKind::Return;
  Statement.RetVal = Call;
  Function.Body = {Statement};
  return Function;
}
} // namespace

TEST(ObjCCallHints, SwiftStaticDescriptionAndFormatPreservePhysicalCarriers) {
  using namespace runtime_function_address_test;
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Formatting : {false, true}) {
      const std::string Name = Formatting ? Format : Description;
      const std::vector<std::string> Providers =
          Formatting
              ? std::vector<
                    std::string>{"/System/Library/Frameworks/"
                                 "Foundation.framework/Foundation",
                                 "/System/Library/Frameworks/"
                                 "Foundation.framework/Versions/C/Foundation",
                                 "/usr/lib/swift/libswiftFoundation.dylib"}
              : std::vector<std::string>{"/usr/lib/swift/libswiftCore.dylib"};
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
        EXPECT_EQ(Signature.ReturnType->Size, 16U);
        ASSERT_EQ(Signature.ReturnComponents.size(), 2U);
        for (unsigned I = 0; I < 2; ++I)
          EXPECT_EQ(Signature.ReturnComponents[I].RegisterOffset,
                    TRI.IntReturnRegs[I]);
        ASSERT_EQ(Signature.Parameters.size(), 3U);
        for (unsigned I = 0; I < 3; ++I) {
          const auto &P = Signature.Parameters[I];
          EXPECT_EQ(P.TheRole, SourceParameterTypeHint::Role::Ordinary);
          EXPECT_EQ(P.Location.RegisterOffset, TRI.IntParamRegs[I]);
          EXPECT_EQ(P.Type->Kind,
                    Formatting && I ? NdTypeKind::Ptr : NdTypeKind::Int);
          EXPECT_EQ(P.Type->Size, !Formatting && I == 2 ? 1U : 8U);
        }
        EXPECT_TRUE(Hint->CanonicalBooleanInputs.empty());
        EXPECT_TRUE(Hint->BorrowedByteInputs.empty());
        EXPECT_EQ(Hint->SwiftStringInputs.size(), Formatting ? 1U : 0U);
        EXPECT_EQ(Hint->SwiftStaticStringInputs.size(), Formatting ? 0U : 1U);
        std::string Error;
        EXPECT_TRUE(validateSourceABI(Signature, Error)) << Error;
        auto Call = HighExpr::makeCall(
            "untrusted", Slot,
            {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
             HighExpr::makeConst(Formatting ? 0 : 2, Formatting ? 8 : 1)});
        Call->Type = Signature.ReturnType;
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
        for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
          auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
          if (Mutation == 0)
            Changed->Signature.ReturnComponents.pop_back();
          if (Mutation == 1)
            Changed->Signature.Parameters[2].TheRole =
                SourceParameterTypeHint::Role::SwiftContext;
          if (Mutation == 2)
            Changed->CanonicalBooleanInputs = {2};
          if (Mutation == 3)
            Changed->BorrowedByteInputs = {{0, 1}};
          if (Mutation == 4)
            Changed->SwiftStaticStringInputs = {{1, 0, 2}};
          if (Mutation == 5)
            Changed->Signature.Parameters[1].Location.RegisterOffset =
                TRI.IntParamRegs[0];
          Call->SourceCallHint = Changed;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
        }
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
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
          EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, Slot)) << Mutation;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
        }
      }
    }
}

TEST(ObjCSourceBindings, SwiftStaticStringRebuildsOnlyTheProvenRepresentation) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = staticStringImage(Architecture);
    for (const auto &[Data, Count, Flags] :
         std::vector<std::tuple<uint64_t, uint64_t, uint8_t>>{
             {0x3000, 3, 2}, {0x3000, 5, 0}, {0x3000, 0, 2}}) {
      auto Function = staticStringCaller(Image, Data, Count, Flags);
      ASSERT_FALSE(Function.Body.empty());
      auto Bound = sdk::bindObjCSourceReferences(Function, Image);
      EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_EQ(Bound.BorrowedBytes,
                (std::set<sdk::BorrowedByteRange>{{Data, uint32_t(Count)}}));
      const auto Call = Bound.Function.Body[0].RetVal;
      EXPECT_EQ(Call->SourceCallHint->TargetName, Description);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      EXPECT_EQ(Call->Operands[2]->ConstVal, Flags);
    }
    // U+3000 coincides with immutable image storage. The scalar form must
    // retain its numeric value and must never borrow even a zero-byte range.
    for (const auto &[Value, Flags] : std::vector<std::pair<uint64_t, uint8_t>>{
             {0, 3}, {65, 3}, {0x3000, 1}, {0x10ffff, 1}}) {
      auto Function = staticStringCaller(Image, Value, 0, Flags);
      auto Bound = sdk::bindObjCSourceReferences(Function, Image);
      EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_TRUE(Bound.BorrowedBytes.empty());
      const auto Word = Bound.Function.Body[0].RetVal->Operands[0];
      EXPECT_EQ(Word->Kind, ExprKind::Const);
      EXPECT_EQ(Word->ConstVal, Value);
      EXPECT_EQ(Word->ConstProvenance, ConstantAddressProvenance::Scalar);
    }
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      auto Function = staticStringCaller(Image, 0x3000, 3, 2);
      auto Call = Function.Body[0].RetVal;
      auto Wrong = Image;
      if (Mutation == 0)
        Call->Operands[2] = HighExpr::makeConst(1, 1);
      if (Mutation == 1)
        Call->Operands[2] = HighExpr::makeConst(4, 1);
      if (Mutation == 2)
        Call->Operands[2]->Kind = ExprKind::Var;
      if (Mutation == 3)
        Call->Operands[2]->MemoryOrdering = NdMemoryOrdering::Acquire;
      if (Mutation == 4)
        Call->Operands[1] = HighExpr::makeConst(UINT64_MAX, 8);
      if (Mutation == 5)
        Call->Operands[1] = HighExpr::makeConst(100, 8);
      if (Mutation == 6)
        Wrong.DyldBindSlots[runtime_function_address_test::Slot].WeakImport =
            true;
      if (Mutation == 7) {
        auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
        Hint->SwiftStaticStringInputs = {{0, 1, 1}};
        Call->SourceCallHint = Hint;
      }
      if (Mutation == 8) {
        auto Cast = std::make_shared<HighExpr>();
        Cast->Kind = ExprKind::Cast;
        Cast->Type = NdType::makeInt(1, false);
        Cast->CastTo = NdType::makeInt(8, false);
        Cast->Operands = {HighExpr::makeConst(2, 1)};
        Call->Operands[2] = Cast;
      }
      const auto Bound = sdk::bindObjCSourceReferences(Function, Wrong);
      EXPECT_TRUE(Bound.BorrowedBytes.empty()) << Mutation;
    }
  }
}

TEST(ObjCCallHints, SwiftStaticStringRejectsMalformedLiteralRepresentations) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = staticStringImage(Architecture);
    EXPECT_TRUE(swiftStaticStringLiteral(Image, 0x3000, 5, 0));
    EXPECT_FALSE(swiftStaticStringLiteral(Image, 0x3000, 5, 2));
    EXPECT_FALSE(swiftStaticStringLiteral(Image, 0x3000, 4, 0));
    EXPECT_FALSE(swiftStaticStringLiteral(Image, 0x3000, 0, 1));
    EXPECT_FALSE(swiftStaticStringLiteral(Image, 0x3000, 3, 4));
    EXPECT_FALSE(swiftStaticStringLiteral(Image, 0x3000, UINT64_MAX, 0));
    EXPECT_FALSE(swiftStaticStringLiteral(Image, 0x3020, 0, 2));
    Image.Segments.back().Flags =
        SegmentFlags::Readable | SegmentFlags::Writable;
    EXPECT_FALSE(swiftStaticStringLiteral(Image, 0x3000, 3, 2));
    EXPECT_TRUE(isCanonicalSwiftStaticStringScalar(0, 0, 3));
    EXPECT_TRUE(isCanonicalSwiftStaticStringScalar(0x10ffff, 0, 1));
    EXPECT_FALSE(isCanonicalSwiftStaticStringScalar(0xd800, 0, 1));
    EXPECT_FALSE(isCanonicalSwiftStaticStringScalar(0xdfff, 0, 1));
    EXPECT_FALSE(isCanonicalSwiftStaticStringScalar(0x110000, 0, 1));
    EXPECT_FALSE(isCanonicalSwiftStaticStringScalar(0x3000, 0, 3));
    EXPECT_FALSE(isCanonicalSwiftStaticStringScalar(0x3000, 1, 1));
  }
}
