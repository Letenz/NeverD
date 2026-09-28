#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftVirtualCalls.h"

using namespace neverd;

namespace {
struct Fixture {
  enum class GetterKind { CGFloat, Double, Bool };
  static constexpr va_t Entry = 0x1100;
  static constexpr va_t CallSite = 0x1170;
  static constexpr va_t IvarSlot = 0x2100;
  static constexpr va_t MaskSlot = 0x2180;

  BinaryImage Image;
  LowFunc Function;

  explicit Fixture(GetterKind Kind = GetterKind::CGFloat) {
    const bool Bool = Kind == GetterKind::Bool;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x1000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x1000);
    Text.Data[CallSite - Text.VA + 0] = 0xa0;
    Text.Data[CallSite - Text.VA + 1] = 0x02;
    Text.Data[CallSite - Text.VA + 2] = 0x3f;
    Text.Data[CallSite - Text.VA + 3] = 0xd6;
    Image.Segments.push_back(std::move(Text));
    Segment Data;
    Data.VA = 0x2000;
    Data.Size = Data.FileSz = 0x1000;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(0x1000);
    Image.Segments.push_back(std::move(Data));
    Image.ImportPtrSlots[MaskSlot] = "_swift_isaMask";
    Image.DyldBindSlots[MaskSlot] = {
        "_swift_isaMask", 0, "/usr/lib/swift/libswiftCore.dylib", false};
    Image.ObjCSourceReferences[IvarSlot] = {
        ObjCSourceReference::Kind::IvarOffset, IvarSlot, 8, "_animationView",
        "_TtC6Lottie23CompatibleAnimationView"};
    ObjCMethod Method;
    Method.Implementation = Entry;
    Method.ClassName = "_TtC6Lottie23CompatibleAnimationView";
    Method.Selector = Bool                         ? "shouldRasterizeWhenIdle"
                      : Kind == GetterKind::Double ? "currentTime"
                                                   : "currentProgress";
    Method.TypeEncoding = Bool ? "B16@0:8" : "d16@0:8";
    SourceFunctionTypeHint Signature;
    Signature.ReturnType =
        Bool ? NdType::makeInt(1, false) : NdType::makeFloat(8);
    Signature.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())},
                            {"_cmd", NdType::makePtr(NdType::makeVoid())}};
    std::string Diagnostic;
    EXPECT_TRUE(assignDarwinObjCSourceABI(Signature, Arch::AArch64, Diagnostic))
        << Diagnostic;
    Method.TypeHint = Signature;
    Image.ObjCMethods.push_back(Method);
    Image.Symbols.push_back(
        {Bool ? "_$s6Lottie23CompatibleAnimationViewC23shouldRasterizeWhenIdle"
                "SbvgTo"
         : Kind == GetterKind::Double
             ? "_$s6Lottie23CompatibleAnimationViewC11currentTime"
               "SdvgTo"
             : "_$s6Lottie23CompatibleAnimationViewC15currentProgress"
               "12CoreGraphics7CGFloatVvgTo",
         Entry, 0x80, true});

    Function.Entry = Entry;
    Function.Blocks.resize(1);
    Function.Blocks[0].StartAddr = Entry;
    auto Add = [&](va_t Address, NdOp Code, NdVar Output,
                   std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Addr = Address;
      Op.Opcode = Code;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Function.Blocks[0].Ops.push_back(Op);
    };
    const auto X0 = NdVar::reg(a64reg::X0, 8);
    const auto X8 = NdVar::reg(a64reg::X8, 8);
    const auto X9 = NdVar::reg(a64reg::X9, 8);
    const auto X20 = NdVar::reg(a64reg::X20, 8);
    const auto X21 = NdVar::reg(a64reg::X21, 8);
    Add(0x1110, NdOp::COPY, X8, {NdVar::dataAddress(IvarSlot, 8)});
    Add(0x1114, NdOp::LOAD, X8, {X8});
    Add(0x111c, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8), {X0, X8});
    Add(0x111c, NdOp::LOAD, X20, {NdVar::tmp(TmpBase, 8)});
    Add(0x1120, NdOp::LOAD, X8, {X20});
    Add(0x1124, NdOp::COPY, X9, {NdVar::dataAddress(MaskSlot, 8)});
    Add(0x1128, NdOp::LOAD, X9, {X9});
    Add(0x112c, NdOp::LOAD, X9, {X9});
    Add(0x1130, NdOp::INT_AND, X8, {X8, X9});
    Add(0x1138, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
        {X8, NdVar::scalar(Bool                         ? 600
                           : Kind == GetterKind::Double ? 648
                                                        : 624,
                           8)});
    Add(0x1138, NdOp::LOAD, X21, {NdVar::tmp(TmpBase, 8)});
    Add(0x1140, NdOp::CALL, X0, {NdVar::codeAddress(0x1300, 8)});
    Add(CallSite, NdOp::INDIR_CALL, X0, {X21});
  }
};
} // namespace

TEST(SwiftVirtualCalls, ExactMaskedIsaGetterBindsSwiftContext) {
  Fixture F;
  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->MethodEntry, Fixture::Entry);
  EXPECT_EQ(Hint.Virtual->IsaMaskImport, Fixture::MaskSlot);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 624U);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));
  EXPECT_EQ(Hint.Signature.Parameters.at(0).TheRole,
            SourceParameterTypeHint::Role::SwiftContext);
  auto Global = darwinRuntimeGlobalAddressHint(F.Image, Fixture::MaskSlot);
  ASSERT_TRUE(Global);
  EXPECT_EQ(Global->TargetName, "swift_isaMask");
}

TEST(SwiftVirtualCalls, ExactMaskedIsaBoolGetterBindsSwiftContext) {
  Fixture F(Fixture::GetterKind::Bool);
  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 600U);
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Int);
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 1U);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));

  F.Image.ObjCMethods[0].TypeEncoding = "d16@0:8";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, ExactMaskedIsaDoubleGetterBindsSwiftContext) {
  Fixture F(Fixture::GetterKind::Double);
  const auto Hints = buildSwiftVirtualCallHints(F.Image, F.Function);
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(Fixture::CallSite);
  ASSERT_TRUE(Hint.Virtual);
  EXPECT_EQ(Hint.Virtual->VtableByteOffset, 648U);
  ASSERT_TRUE(Hint.Signature.ReturnType);
  EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Float);
  EXPECT_EQ(Hint.Signature.ReturnType->Size, 8U);
  EXPECT_TRUE(isSwiftVirtualSourceCallHint(F.Image, Hint));

  F.Image.Symbols[0].Name += "Other";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}

TEST(SwiftVirtualCalls, RejectsAlteredCodeImportAndReceiverEvidence) {
  Fixture F;
  F.Image.Segments[0].Data[Fixture::CallSite - 0x1000] = 0x00;
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F = Fixture();
  F.Image.DyldBindSlots[Fixture::MaskSlot].Module =
      "/usr/lib/libSystem.B.dylib";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F = Fixture();
  F.Image.ObjCSourceReferences[Fixture::IvarSlot].ClassName = "Other";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
  F = Fixture();
  F.Image.ObjCMethods[0].TypeEncoding = "q16@0:8";
  EXPECT_TRUE(buildSwiftVirtualCallHints(F.Image, F.Function).empty());
}
