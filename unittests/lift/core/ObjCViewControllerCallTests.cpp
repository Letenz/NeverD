#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

using namespace neverd;
namespace {
struct Getter {
  const char *Selector;
  const char *Result;
};
constexpr Getter Getters[] = {
    {"viewIfLoaded", "UIView"},
    {"navigationItem", "UINavigationItem"},
    {"navigationController", "UINavigationController"},
    {"presentedViewController", "UIViewController"},
    {"view", "UIView"}};

BinaryImage controllerImage(const char *Parent = "UIViewController") {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/UIKit.framework/UIKit",
      "/System/Library/Frameworks/Foundation.framework/Foundation",
      "/System/Library/Frameworks/QuartzCore.framework/QuartzCore"};
  ObjCClass Class;
  Class.Name = "LocalController";
  Class.SuperclassName = Parent;
  Class.InheritanceStatus = "resolved";
  Image.ObjCClasses.push_back(Class);
  ObjCMethod Method;
  Method.ClassName = Class.Name;
  Method.Selector = "use";
  Method.Implementation = 0x1000;
  Method.TypeHint = parseObjCMethodEncoding("use", "v16@0:8");
  std::string Error;
  EXPECT_TRUE(assignDarwinObjCSourceABI(*Method.TypeHint, Image.Arch, Error));
  Image.ObjCMethods.push_back(Method);
  return Image;
}

ExprPtr sourceCall(const Getter &Getter, const SourceFunctionTypeHint &Type) {
  auto Call = HighExpr::makeCall("objc_msgSend", 0, {});
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint->Selector = Getter.Selector;
  Hint->Signature = Type;
  Call->SourceCallHint = std::move(Hint);
  Call->Type = Type.ReturnType;
  for (const auto &Parameter : Type.Parameters)
    Call->Operands.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
  return Call;
}
} // namespace

TEST(ObjCCallHints, IOSControllerGettersKeepObjectABIAndDeclaredResults) {
  auto Image = controllerImage();
  const auto Receiver = objcMethodReceiverTypeHint(Image, 0x1000);
  ASSERT_TRUE(Receiver);
  const auto &TRI = getTargetRegInfo(Image.Arch);
  for (const auto &Getter : Getters) {
    SCOPED_TRACE(Getter.Selector);
    const auto Type = objcSelectorSourceTypeHint(Image, Getter.Selector);
    ASSERT_TRUE(Type);
    EXPECT_EQ(Type->Origin, SourceFunctionTypeHint::OriginKind::ObjCSDK);
    ASSERT_EQ(Type->Parameters.size(), 2U);
    EXPECT_EQ(Type->ReturnType->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Type->ReturnType->Size, 8U);
    EXPECT_EQ(Type->ReturnLocation.Kind, SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(Type->ReturnLocation.RegisterOffset, TRI.IntReturnReg);
    EXPECT_EQ(Type->ReturnLocation.ValueBytes, 8U);
    for (unsigned I = 0; I != 2; ++I) {
      EXPECT_EQ(Type->Parameters[I].Type->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(Type->Parameters[I].Location.RegisterOffset,
                TRI.IntParamRegs[I]);
    }
    const auto Owned =
        objcReceiverSourceTypeHint(Image, Getter.Selector, *Receiver);
    ASSERT_TRUE(Owned.Signature);
    EXPECT_FALSE(Owned.RequiresGlobalAgreement);
    EXPECT_TRUE(equalSourceABIs(*Type, *Owned.Signature));
    EXPECT_EQ(Owned.ReturnClass, Getter.Result);
    EXPECT_TRUE(
        sdk::objcSourceCallBound(*sourceCall(Getter, *Type), Image, {}));
  }
  // Both other SDK owners of these selectors have the same object ABI and
  // result class. Their evidence must not turn unrelated objects into views.
  for (const auto &[Parent, Index] :
       {std::pair{"UISearchDisplayController", 1U},
        std::pair{"UIPresentationController", 3U}}) {
    const auto Other = controllerImage(Parent);
    const auto Input = objcMethodReceiverTypeHint(Other, 0x1000);
    ASSERT_TRUE(Input);
    const auto Owned =
        objcReceiverSourceTypeHint(Other, Getters[Index].Selector, *Input);
    ASSERT_TRUE(Owned.Signature);
    EXPECT_EQ(Owned.ReturnClass, Getters[Index].Result);
  }
}

TEST(ObjCCallHints, IOSControllerGettersRequireCurrentProviderAndReceiver) {
  for (const auto &Getter : Getters) {
    SCOPED_TRACE(Getter.Selector);
    const auto Image = controllerImage();
    const auto Type = objcSelectorSourceTypeHint(Image, Getter.Selector);
    ASSERT_TRUE(Type);
    const auto Call = sourceCall(Getter, *Type);
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Changed = Image;
      if (Mutation == 0)
        Changed.DynInfo.NeededLibs.clear();
      if (Mutation == 1)
        Changed.DynInfo.NeededLibs[0] = "/tmp/UIKit.framework/UIKit";
      if (Mutation == 2)
        Changed.DynInfo.NeededLibs[0] =
            "/System/Library/Frameworks/UIKit.framework/Versions/A/UIKit";
      if (Mutation == 3)
        Changed.Arch = Arch::X64;
      if (Mutation == 4)
        Changed.Bits = Bitness::Bits32;
      if (Mutation == 5)
        Changed.ObjCMethods.front().IsClassMethod = true;
      if (Mutation == 6)
        Changed.ObjCClasses.front().SuperclassName = "NSString";
      const auto Receiver = objcMethodReceiverTypeHint(Changed, 0x1000);
      if (Receiver)
        EXPECT_FALSE(
            objcReceiverSourceTypeHint(Changed, Getter.Selector, *Receiver)
                .Signature);
      if (Mutation < 5) {
        EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, Getter.Selector));
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Changed, {}));
      }
    }
  }
}

TEST(ObjCCallHints, IOSControllerGettersRejectConflictingOrChangedCallTypes) {
  for (const auto &Getter : Getters) {
    SCOPED_TRACE(Getter.Selector);
    auto Image = controllerImage();
    const auto Type = objcSelectorSourceTypeHint(Image, Getter.Selector);
    ASSERT_TRUE(Type);
    const auto Call = sourceCall(Getter, *Type);
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      auto ChangedCall = sourceCall(Getter, *Type);
      auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
      ChangedCall->SourceCallHint = Hint;
      if (Mutation == 0)
        Hint->Signature.ReturnLocation.RegisterOffset =
            getTargetRegInfo(Image.Arch).IntParamRegs[1];
      if (Mutation == 1)
        Hint->Signature.ReturnType = NdType::makeInt(8);
      if (Mutation == 2)
        ChangedCall->Operands.push_back(HighExpr::makeConst(7, 8));
      EXPECT_FALSE(sdk::objcSourceCallBound(*ChangedCall, Image, {}));
    }
    ObjCMethod Conflict;
    Conflict.ClassName = "UnrelatedOwner";
    Conflict.Selector = Getter.Selector;
    Conflict.TypeHint = parseObjCMethodEncoding(Getter.Selector, "d16@0:8");
    std::string Error;
    ASSERT_TRUE(
        assignDarwinObjCSourceABI(*Conflict.TypeHint, Image.Arch, Error));
    Image.ObjCMethods.push_back(Conflict);
    EXPECT_FALSE(objcSelectorSourceTypeHint(Image, Getter.Selector));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
  }
}
