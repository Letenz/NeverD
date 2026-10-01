#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/RuntimeFunctionAddress.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

#include "llvm/BinaryFormat/MachO.h"

using namespace neverd;
namespace {
BinaryImage runtimeImage(llvm::StringRef Import, Arch Architecture) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  for (unsigned I = 0; I != 2; ++I) {
    Segment Segment;
    Segment.VA = 0x1000 + I * 0x1000;
    Segment.FileOff = I * 0x1000;
    Segment.Size = Segment.FileSz = 0x1000;
    Segment.Flags = SegmentFlags::Readable;
    if (!I)
      Segment.Flags = Segment.Flags | SegmentFlags::Executable;
    Segment.Data.resize(0x1000);
    Image.Segments.push_back(Segment);
    Section Section;
    Section.VA = Segment.VA;
    Section.Size = Section.FileSz = Segment.Size;
    Section.FileOff = Segment.FileOff;
    Section.Flags = Segment.Flags;
    if (I) {
      Section.Name = "__got";
      Section.Type = llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS;
    }
    Image.Sections.push_back(Section);
  }
  Image.ImportPtrSlots[0x2180] = Import.str();
  return Image;
}
} // namespace

TEST(ObjCCallHints, SwiftTypeNameInputsRequireCanonicalBooleanValues) {
  constexpr llvm::StringLiteral Provider = "/usr/lib/swift/libswiftCore.dylib";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const bool Dynamic : {false, true}) {
      const std::string Name = Dynamic ? "swift_getDynamicType"
                                       : "$ss9_typeName_9qualifiedSSypXp_SbtF";
      SCOPED_TRACE(Name + ":" + std::to_string(static_cast<int>(Architecture)));
      const std::string Import = "_" + Name;
      auto Image = runtimeImage(Import, Architecture);
      Image.DyldBindSlots[0x2180] = {Import, 0, Provider.str(), false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      const auto &Signature = Hint->Signature;
      const unsigned BooleanIndex = Dynamic ? 2 : 1;
      EXPECT_EQ(Hint->CanonicalBooleanInputs,
                (std::vector<unsigned>{BooleanIndex}));
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                Dynamic ? SourceFunctionTypeHint::ConventionKind::C
                        : SourceFunctionTypeHint::ConventionKind::Swift);
      EXPECT_EQ(Signature.ReturnType->Kind,
                Dynamic ? NdTypeKind::Ptr : NdTypeKind::Int);
      EXPECT_EQ(Signature.ReturnType->Size, Dynamic ? 8U : 16U);
      ASSERT_EQ(Signature.Parameters.size(), BooleanIndex + 1U);
      const auto &TRI = getTargetRegInfo(Architecture);
      for (unsigned I = 0; I <= BooleanIndex; ++I) {
        const auto &Parameter = Signature.Parameters[I];
        EXPECT_EQ(Parameter.Type->Kind,
                  I == BooleanIndex ? NdTypeKind::Int : NdTypeKind::Ptr);
        EXPECT_EQ(Parameter.Type->Size, I == BooleanIndex ? 1U : 8U);
        EXPECT_EQ(Parameter.TheRole, SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Parameter.Location.RegisterOffset, TRI.IntParamRegs[I]);
        EXPECT_EQ(Parameter.Location.ExtendTo32Bits, I == BooleanIndex);
      }
      if (!Dynamic) {
        ASSERT_EQ(Signature.ReturnComponents.size(), 2U);
        for (unsigned I = 0; I != 2; ++I)
          EXPECT_EQ(Signature.ReturnComponents[I].RegisterOffset,
                    TRI.IntReturnRegs[I]);
      }
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
      std::vector<ExprPtr> Arguments;
      for (unsigned I = 0; I < BooleanIndex; ++I)
        Arguments.push_back(HighExpr::makeConst(0x4560 + 8 * I, 8));
      Arguments.push_back(HighExpr::makeConst(1, 4));
      auto Call = HighExpr::makeCall("untrusted_name", 0x2180, Arguments);
      Call->Type = Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      const auto CastByte = [](uint64_t Value, unsigned Size) {
        auto Cast = std::make_shared<HighExpr>();
        Cast->Kind = ExprKind::Cast;
        Cast->Type = Cast->CastTo = NdType::makeInt(1, false);
        Cast->Operands = {HighExpr::makeConst(Value, Size)};
        return Cast;
      };
      for (const auto Value : {0U, 1U}) {
        Call->Operands[BooleanIndex] = HighExpr::makeConst(Value, 4);
        EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        Call->Operands[BooleanIndex] = CastByte(Value, 4);
        EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        auto Extract =
            HighExpr::makeBinop(NdOp::SUBBYTES, HighExpr::makeConst(Value, 4),
                                HighExpr::makeConst(0, 4));
        Extract->Type = NdType::makeInt(1, false);
        Call->Operands[BooleanIndex] = Extract;
        EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        Extract->Operands[1] = HighExpr::makeConst(1, 4);
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      }
      for (const auto Value : {2ULL, 255ULL, 257ULL, ~0ULL}) {
        Call->Operands[BooleanIndex] = HighExpr::makeConst(Value, 8);
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
        Call->Operands[BooleanIndex] = CastByte(Value, 8);
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
        auto Extract =
            HighExpr::makeBinop(NdOp::SUBBYTES, HighExpr::makeConst(Value, 8),
                                HighExpr::makeConst(0, 4));
        Extract->Type = NdType::makeInt(1, false);
        Call->Operands[BooleanIndex] = Extract;
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      }
      Call->Operands[BooleanIndex] = HighExpr::makeConst(1, 1);
      Call->Operands[BooleanIndex]->Type = NdType::makeFloat(4);
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      Call->Operands[BooleanIndex] = HighExpr::makeConst(1, 1);
      Call->Operands[BooleanIndex]->MemoryOrdering = NdMemoryOrdering::Acquire;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      Call->Operands[BooleanIndex] = HighExpr::makeConst(1, 1);
      Call->Operands[BooleanIndex]->Kind = ExprKind::Var;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      Call->Operands[BooleanIndex] = HighExpr::makeConst(1, 4);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      auto Overwide = HighExpr::makeBinop(
          NdOp::SUBBYTES, HighExpr::makeConst(1, 1), HighExpr::makeConst(0, 4));
      Overwide->Type = NdType::makeInt(8, false);
      Call->Operands[BooleanIndex] = Overwide;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      Call->Operands[BooleanIndex] = HighExpr::makeConst(1, 1);
      EXPECT_FALSE(runtimeCFunctionAddressHint(Image, 0x2180));
      auto Ordinary = Image;
      Ordinary.ImportPtrSlots[0x2180] = "_swift_getObjectType";
      Ordinary.DyldBindSlots[0x2180].Name = "_swift_getObjectType";
      EXPECT_TRUE(runtimeCFunctionAddressHint(Ordinary, 0x2180));
      for (const auto Inputs :
           {std::vector<unsigned>{}, std::vector<unsigned>{0},
            std::vector<unsigned>{99}}) {
        auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
        Changed->CanonicalBooleanInputs = Inputs;
        Call->SourceCallHint = std::move(Changed);
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
      }
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
        auto Wrong = Image;
        if (Mutation == 0)
          Wrong.DyldBindSlots[0x2180].Module = "/tmp/libswiftCore.dylib";
        else if (Mutation == 1)
          Wrong.DyldBindSlots[0x2180].Addend = 1;
        else if (Mutation == 2)
          Wrong.DyldBindSlots[0x2180].WeakImport = true;
        else if (Mutation == 3)
          Wrong.DyldBindSlots[0x2180].Name += "invalid";
        else if (Mutation == 4)
          Wrong.DyldBindSlots.erase(0x2180);
        else
          Wrong.ConflictingImportStorageSlots.insert(0x2180);
        EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, 0x2180)) << Mutation;
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
      }
    }
  }
}
