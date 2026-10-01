#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "llvm/Support/Endian.h"

using namespace neverd;
namespace {
BinaryImage image(Arch Architecture = Arch::AArch64) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Segment;
  Segment.VA = 0x1000;
  Segment.Size = Segment.FileSz = 0x2000;
  Segment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Segment.Data.resize(0x2000);
  Image.Segments.push_back(Segment);
  Section Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.push_back(Text);
  Section Data;
  Data.VA = 0x2000;
  Data.FileOff = 0x1000;
  Data.Size = Data.FileSz = 0x1000;
  Data.Flags = SegmentFlags::Readable;
  Image.Sections.push_back(Data);
  Image.ImportPtrSlots[0x2180] = "_objc_msgSend";
  ObjCSourceReference Reference;
  Reference.Address = 0x2100;
  Reference.Name = "scale:";
  Image.ObjCSourceReferences[0x2100] = Reference;
  ObjCMethod Method;
  Method.ClassName = "First";
  Method.Selector = Reference.Name;
  Method.Implementation = 0x1400;
  Method.TypeHint = parseObjCMethodEncoding("use", "v16@0:8");
  std::string Diagnostic;
  EXPECT_TRUE(
      assignDarwinObjCSourceABI(*Method.TypeHint, Architecture, Diagnostic));
  Image.ObjCMethods.push_back(Method);
  // ADRP x1,0x2000; LDR x1,[x1,#0x100]; ADRP x16,0x2000;
  // LDR x16,[x16,#0x180]; BR x16. No symbol table is supplied.
  const uint32_t Stub[] = {0xb0000001, 0xf9408021, 0xb0000010, 0xf940c210,
                           0xd61f0200};
  for (size_t I = 0; I < 5; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x100 + I * 4, Stub[I]);
  return Image;
}

LowOp operation(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
                va_t Address = 0x1200) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Address;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

LowFunc caller(Arch Architecture = Arch::AArch64) {
  LowFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "caller";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x1200;
  Block.EndAddr = 0x1208;
  const auto &TRI = getTargetRegInfo(Architecture);
  Block.Ops.push_back(operation(NdOp::CALL, NdVar::reg(TRI.IntReturnReg, 8),
                                {NdVar::cst(0x1100, 8)}));
  Block.Ops.push_back(
      operation(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 4)}, 0x1204));
  Function.Blocks.push_back(Block);
  return Function;
}

MedFunc convert(const BinaryImage &Image, const LowFunc &Low,
                bool Enable = true) {
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  Converter.setSourceCallHintsEnabled(Enable);
  auto Result = Converter.convert(Low, Image.Arch, BinaryFormat::MachO);
  recoverCallAbi(Result, Image.Arch, {}, &Image);
  return Result;
}

ExprPtr receiverCallExpression(const SourceCallTypeHint &Binding) {
  auto Call = HighExpr::makeCall("objc_msgSend", 0, {});
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Binding);
  Call->Type = Binding.Signature.ReturnType;
  for (const auto &Parameter : Binding.Signature.Parameters)
    Call->Operands.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
  return Call;
}

struct LayoutMember {
  const char *Selector;
  const char *Owner;
  const char *Encoding;
};
// Apple Clang encodings independently obtained from the agreeing device and
// simulator declarations, including their fixed enum underlying types.
constexpr LayoutMember LayoutMembers[] = {
    {"subviews", "UIView", "@16@0:8"},
    {"insertSubview:atIndex:", "UIView", "v32@0:8@16q24"},
    {"setShowsHorizontalScrollIndicator:", "UIScrollView", "v20@0:8B16"},
    {"setShowsVerticalScrollIndicator:", "UIScrollView", "v20@0:8B16"},
    {"setAxis:", "UIStackView", "v24@0:8q16"},
    {"setDistribution:", "UIStackView", "v24@0:8q16"},
    {"setAlignment:", "UIStackView", "v24@0:8q16"},
    {"setSpacing:", "UIStackView", "v24@0:8d16"}};
} // namespace

TEST(ObjCCallHints, IOSScrollPropertiesRequireDeclaredProviderAndReceiver) {
  auto Image = image();
  Image.ObjCMethods.front().Selector = "use";
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/UIKit.framework/UIKit",
      "/System/Library/Frameworks/Foundation.framework/Foundation",
      "/System/Library/Frameworks/QuartzCore.framework/QuartzCore"};
  ObjCClass Class;
  Class.Name = "First";
  Class.InheritanceStatus = "resolved";
  Class.SuperclassName = "UIScrollView";
  Image.ObjCClasses.push_back(Class);
  const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1400);
  ASSERT_TRUE(Receiver);
  for (const char *Selector :
       {"setBounces:", "setContentInset:", "safeAreaInsets",
        "effectiveUserInterfaceLayoutDirection"}) {
    SCOPED_TRACE(Selector);
    const bool Setter = llvm::StringRef(Selector).contains(':');
    const auto Hint = objcSelectorSourceTypeHint(Image, Selector);
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::ObjCSDK);
    ASSERT_EQ(Hint->Parameters.size(), Setter ? 3U : 2U);
    if (Setter)
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Void);
    if (llvm::StringRef(Selector) == "effectiveUserInterfaceLayoutDirection") {
      EXPECT_EQ(Hint->ReturnType->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->ReturnType->Size, 8U);
      EXPECT_TRUE(Hint->ReturnType->IsSigned);
      EXPECT_EQ(Hint->ReturnLocation.RegisterOffset,
                getTargetRegInfo(Arch::AArch64).IntReturnReg);
    }
    if (llvm::StringRef(Selector) == "setBounces:") {
      const auto &Boolean = Hint->Parameters.back();
      EXPECT_EQ(Boolean.Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Boolean.Type->Size, 1U);
      EXPECT_FALSE(Boolean.Type->IsSigned);
      EXPECT_EQ(Boolean.Location.RegisterOffset,
                getTargetRegInfo(Arch::AArch64).IntParamRegs[2]);
      EXPECT_TRUE(Boolean.Location.ExtendTo32Bits);
    }
    const auto Owned = objcReceiverSourceTypeHint(Image, Selector, *Receiver);
    ASSERT_TRUE(Owned.Signature);
    EXPECT_TRUE(equalSourceABIs(*Hint, *Owned.Signature));
    auto ChangedReceiver = *Receiver;
    ChangedReceiver.IsClassMethod = true;
    EXPECT_FALSE(
        objcReceiverSourceTypeHint(Image, Selector, ChangedReceiver).Signature);
    for (const char *Provider :
         {"/tmp/UIKit.framework/UIKit",
          "/System/Library/Frameworks/UIKit.framework/Versions/A/UIKit"}) {
      auto Changed = Image;
      Changed.DynInfo.NeededLibs = {Provider};
      EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, Selector));
      EXPECT_FALSE(
          objcReceiverSourceTypeHint(Changed, Selector, *Receiver).Signature);
    }
    auto Unsupported = Image;
    Unsupported.Arch = Arch::X64;
    EXPECT_FALSE(objcSelectorSourceTypeHint(Unsupported, Selector));
  }
}

TEST(ObjCCallHints, IOSLayoutControlsPreserveDeclaredCarriersAndOwners) {
  for (const auto &Member : LayoutMembers) {
    SCOPED_TRACE(Member.Selector);
    auto Image = image();
    Image.ObjCMethods.front().Selector = "use";
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/UIKit.framework/UIKit",
        "/System/Library/Frameworks/Foundation.framework/Foundation",
        "/System/Library/Frameworks/QuartzCore.framework/QuartzCore"};
    ObjCClass Class;
    Class.Name = "First";
    Class.SuperclassName = Member.Owner;
    Class.InheritanceStatus = "resolved";
    Image.ObjCClasses.push_back(Class);
    const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1400);
    ASSERT_TRUE(Receiver);
    auto Expected = parseObjCMethodEncoding(Member.Selector, Member.Encoding);
    ASSERT_TRUE(Expected);
    Expected->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(*Expected, Image.Arch, Error));
    const auto Global = objcSelectorSourceTypeHint(Image, Member.Selector);
    ASSERT_TRUE(Global);
    EXPECT_TRUE(equalSourceABIs(*Expected, *Global));
    const auto Owned =
        objcReceiverSourceTypeHint(Image, Member.Selector, *Receiver);
    ASSERT_TRUE(Owned.Signature);
    EXPECT_FALSE(Owned.RequiresGlobalAgreement);
    EXPECT_TRUE(equalSourceABIs(*Expected, *Owned.Signature));
    if (llvm::StringRef(Member.Selector) == "subviews")
      EXPECT_EQ(Owned.ReturnClass, "NSArray");

    Image.ObjCSourceReferences.at(0x2100).Name = Member.Selector;
    auto Low = caller();
    Low.Blocks.front().Ops.back().NumInputs = 0;
    const auto Med = convert(Image, Low);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    ASSERT_TRUE(Call.SourceCallHint);
    EXPECT_TRUE(equalSourceABIs(*Expected, Call.SourceCallHint->Signature));
    EXPECT_EQ(Call.Args.size(), Expected->Parameters.size());
    const auto Expression = receiverCallExpression(*Call.SourceCallHint);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    auto Changed = Image;
    Changed.DynInfo.NeededLibs[0] = "/tmp/UIKit.framework/UIKit";
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Changed, {}));
    Changed = Image;
    Changed.Arch = Arch::X64;
    EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, Member.Selector));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Changed, {}));
    Changed = Image;
    Changed.ObjCClasses.front().SuperclassName = "NSString";
    EXPECT_FALSE(objcReceiverSourceTypeHint(Changed, Member.Selector, *Receiver)
                     .Signature);
  }
}

TEST(ObjCCallHints, IOSLayoutControlsRejectChangedWidthsAndRuntimeConflicts) {
  for (const auto &Member : LayoutMembers) {
    SCOPED_TRACE(Member.Selector);
    auto Image = image();
    Image.ObjCMethods.clear();
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/UIKit.framework/UIKit"};
    Image.ObjCSourceReferences.at(0x2100).Name = Member.Selector;
    auto Low = caller();
    Low.Blocks.front().Ops.back().NumInputs = 0;
    const auto Med = convert(Image, Low);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    ASSERT_TRUE(Med.CallInfos.front().SourceCallHint);
    const auto &Binding = *Med.CallInfos.front().SourceCallHint;
    const auto Expression = receiverCallExpression(Binding);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    auto Changed = std::make_shared<SourceCallTypeHint>(Binding);
    if (Changed->Signature.Parameters.size() > 2)
      Changed->Signature.Parameters.back().Location.ValueBytes = 2;
    else
      Changed->Signature.ReturnLocation.ValueBytes = 4;
    Expression->SourceCallHint = Changed;
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    Expression->SourceCallHint = std::make_shared<SourceCallTypeHint>(Binding);
    ObjCMethod Conflict;
    Conflict.Selector = Member.Selector;
    auto ConflictingEncoding = std::string(Member.Encoding);
    ConflictingEncoding.front() = 'd';
    Conflict.TypeHint =
        parseObjCMethodEncoding(Member.Selector, ConflictingEncoding);
    ASSERT_TRUE(Conflict.TypeHint);
    std::string Error;
    ASSERT_TRUE(
        assignDarwinObjCSourceABI(*Conflict.TypeHint, Image.Arch, Error));
    Image.ObjCMethods.push_back(Conflict);
    EXPECT_FALSE(objcSelectorSourceTypeHint(Image, Member.Selector));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
  }
}

TEST(ObjCCallHints, IOSSafeAreaGetterKeepsEveryFloatingResultComponent) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  auto Image = image(Arch::AArch64);
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/UIKit.framework/UIKit"};
  Image.ObjCSourceReferences.at(0x2100).Name = "safeAreaInsets";
  auto Low = caller();
  Low.Blocks.front().Ops.back().NumInputs = 0;
  const auto Med = convert(Image, Low);
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  const auto &Call = Med.CallInfos.front();
  ASSERT_TRUE(Call.SourceCallHint);
  const auto &Binding = *Call.SourceCallHint;
  const auto &Signature = Binding.Signature;
  ASSERT_EQ(Signature.ReturnType->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Signature.ReturnType->Size, 32U);
  ASSERT_EQ(Signature.ReturnType->Fields.size(), 4U);
  ASSERT_EQ(Signature.ReturnComponents.size(), 4U);
  ASSERT_EQ(Call.Args.size(), 2U);
  for (unsigned I = 0; I != 4; ++I) {
    EXPECT_EQ(Signature.ReturnType->Fields[I]->Kind, NdTypeKind::Float);
    EXPECT_EQ(Signature.ReturnType->Fields[I]->Size, 8U);
    EXPECT_EQ(Signature.ReturnComponents[I].Kind,
              SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Signature.ReturnComponents[I].RegisterOffset,
              TRI.FPReturnRegs[I]);
    EXPECT_EQ(Signature.ReturnComponents[I].ValueBytes, 8U);
  }
  EXPECT_EQ(Med.Blocks[Call.BlockId].Ops[Call.OpIdx].Output.Size, 32U);
  auto Expression = receiverCallExpression(Binding);
  ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    auto Changed = std::make_shared<SourceCallTypeHint>(Binding);
    auto &Components = Changed->Signature.ReturnComponents;
    if (Mutation == 0)
      Components.pop_back();
    if (Mutation == 1)
      Components.back().RegisterOffset = TRI.FPReturnRegs[0];
    if (Mutation == 2)
      Components.back().ValueBytes = 4;
    Expression->SourceCallHint = Changed;
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {})) << Mutation;
  }
}

TEST(ObjCCallHints, IOSScrollInsetsKeepEveryFloatingComponent) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  auto Image = image(Arch::AArch64);
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/UIKit.framework/UIKit"};
  Image.ObjCSourceReferences.at(0x2100).Name = "setContentInset:";
  auto Low = caller();
  Low.Blocks.front().Ops.back().NumInputs = 0;
  const auto Med = convert(Image, Low);
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  ASSERT_TRUE(Med.CallInfos.front().SourceCallHint);
  const auto &Binding = *Med.CallInfos.front().SourceCallHint;
  const auto &Signature = Binding.Signature;
  EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Signature.Parameters.size(), 3U);
  for (unsigned I = 0; I != 2; ++I)
    EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
              TRI.IntParamRegs[I]);
  const auto &Insets = Signature.Parameters.back();
  ASSERT_EQ(Insets.Type->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Insets.Type->Size, 32U);
  ASSERT_EQ(Insets.Type->Fields.size(), 4U);
  ASSERT_EQ(Insets.Components.size(), 4U);
  for (unsigned I = 0; I != 4; ++I) {
    EXPECT_EQ(Insets.Type->Fields[I]->Kind, NdTypeKind::Float);
    EXPECT_EQ(Insets.Type->Fields[I]->Size, 8U);
    EXPECT_EQ(Insets.Components[I].Kind,
              SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Insets.Components[I].RegisterOffset, TRI.FPParamRegs[I]);
    EXPECT_EQ(Insets.Components[I].ValueBytes, 8U);
  }
  ASSERT_EQ(Med.CallInfos.front().Args.size(), 6U);
  for (unsigned I = 0; I != 4; ++I) {
    EXPECT_EQ(Med.CallInfos.front().Args[I + 2].RegOff, TRI.FPParamRegs[I]);
    EXPECT_EQ(Med.CallInfos.front().Args[I + 2].Size, 8U);
  }
  auto Expression = receiverCallExpression(Binding);
  Expression->Operands.back()->Type = Insets.Type;
  ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    auto Changed = std::make_shared<SourceCallTypeHint>(Binding);
    auto &Components = Changed->Signature.Parameters.back().Components;
    if (Mutation == 0)
      Components.pop_back();
    if (Mutation == 1)
      Components.back().RegisterOffset = TRI.FPParamRegs[0];
    if (Mutation == 2)
      Components.back().ValueBytes = 4;
    Expression->SourceCallHint = Changed;
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {})) << Mutation;
  }
  Expression->SourceCallHint = std::make_shared<SourceCallTypeHint>(Binding);
  // A conflicting current binary declaration must still veto the SDK fact.
  ObjCMethod Conflict;
  Conflict.Selector = "setContentInset:";
  Conflict.TypeHint = parseObjCMethodEncoding("setContentInset:", "v24@0:8@16");
  ASSERT_TRUE(Conflict.TypeHint);
  std::string Diagnostic;
  ASSERT_TRUE(
      assignDarwinObjCSourceABI(*Conflict.TypeHint, Arch::AArch64, Diagnostic));
  Image.ObjCMethods.push_back(Conflict);
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "setContentInset:"));
  EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
}
