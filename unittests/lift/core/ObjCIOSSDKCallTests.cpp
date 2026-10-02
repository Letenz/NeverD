#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include <algorithm>
#include <string>

using namespace neverd;
namespace {
struct FixedAPI {
  const char *Framework;
  const char *Selector;
  const char *Encoding;
};
// Independent Apple Clang encodings of the complete, agreeing device and
// simulator declarations in SDK artifact 11077989891. Enum underlying types
// and every aggregate field were also checked against both original ASTs.
constexpr FixedAPI APIs[] = {
    {"UIKit", "CGContext", "^{CGContext=}16@0:8"},
    {"UIKit", "CGPath", "r^{CGPath=}16@0:8"},
    {"UIKit", "addGestureRecognizer:", "v24@0:8@16"},
    {"UIKit", "addLineToPoint:", "v32@0:8{CGPoint=dd}16"},
    // Public target/action contracts independently encoded by Apple Clang for
    // arm64 iOS and arm64 iOS Simulator, separate from the archived SDK ASTs.
    {"UIKit", "addTarget:action:forControlEvents:", "v40@0:8@16:24Q32"},
    {"UIKit", "animatedImageWithImages:duration:", "@32@0:8@16d24"},
    {"UIKit", "attributedText", "@16@0:8"},
    {"UIKit", "becomeFirstResponder", "B16@0:8"},
    {"UIKit",
     "bezierPathWithRect:", "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
    {"UIKit", "cellForItemAtIndexPath:", "@24@0:8@16"},
    {"UIKit", "centerYAnchor", "@16@0:8"},
    {"UIKit", "collectionView", "@16@0:8"},
    {"UIKit", "contentOffset", "{CGPoint=dd}16@0:8"},
    {"UIKit", "contentView", "@16@0:8"},
    {"UIKit",
     "convertPoint:toCoordinateSpace:", "{CGPoint=dd}40@0:8{CGPoint=dd}16@32"},
    {"UIKit", "convertRect:toCoordinateSpace:",
     "{CGRect={CGPoint=dd}{CGSize=dd}}56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@"
     "48"},
    {"UIKit", "currentTraitCollection", "@16@0:8"},
    {"UIKit", "firstBaselineAnchor", "@16@0:8"},
    {"CoreImage",
     "imageByApplyingFilter:withInputParameters:", "@32@0:8@16@24"},
    {"CoreImage", "imageWithColor:", "@24@0:8@16"},
    {"UIKit", "images", "@16@0:8"},
    {"UIKit", "impactOccurred", "v16@0:8"},
    {"UIKit", "initForTextStyle:", "@24@0:8@16"},
    {"CoreImage", "initWithColor:", "@24@0:8@16"},
    {"UIKit", "initWithStyle:", "@24@0:8q16"},
    {"UIKit", "initWithTarget:action:", "@32@0:8@16:24"},
    {"UIKit", "layoutDirection", "q16@0:8"},
    {"UIKit", "layoutFrame", "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},
    {"UIKit", "layoutMargins", "{UIEdgeInsets=dddd}16@0:8"},
    {"UIKit", "leftBarButtonItem", "@16@0:8"},
    {"UIKit", "moveToPoint:", "v32@0:8{CGPoint=dd}16"},
    {"UIKit", "presentationController", "@16@0:8"},
    {"UIKit", "readableContentGuide", "@16@0:8"},
    {"UIKit", "scaledValueForValue:", "d24@0:8d16"},
    {"UIKit", "scrollRangeToVisible:", "v32@0:8{_NSRange=QQ}16"},
    {"UIKit", "selectedBackgroundView", "@16@0:8"},
    {"UIKit", "selectedRange", "{_NSRange=QQ}16@0:8"},
    {"UIKit", "semanticContentAttribute", "q16@0:8"},
    {"UIKit", "setBarTintColor:", "v24@0:8@16"},
    {"UIKit", "setBaseWritingDirection:", "v24@0:8q16"},
    {"UIKit", "setEditable:", "v20@0:8B16"},
    {"UIKit", "setExclusionPaths:", "v24@0:8@16"},
    {"UIKit", "setImage:forState:", "v32@0:8@16Q24"},
    {"UIKit", "setItems:animated:", "v28@0:8@16B24"},
    {"UIKit", "setLeftBarButtonItem:", "v24@0:8@16"},
    {"UIKit", "setLineBreakMode:", "v24@0:8q16"},
    {"UIKit", "setLineFragmentPadding:", "v24@0:8d16"},
    {"UIKit", "setLineHeightMultiple:", "v24@0:8d16"},
    {"UIKit", "setLineSpacing:", "v24@0:8d16"},
    {"UIKit", "setLinkTextAttributes:", "v24@0:8@16"},
    {"UIKit", "setMaximumLineHeight:", "v24@0:8d16"},
    {"UIKit", "setNumberOfLines:", "v24@0:8q16"},
    {"UIKit", "setRightBarButtonItem:", "v24@0:8@16"},
    {"UIKit", "setRightView:", "v24@0:8@16"},
    {"UIKit", "setRightViewMode:", "v24@0:8q16"},
    {"UIKit", "setScrollEnabled:", "v20@0:8B16"},
    {"UIKit", "setScrollIndicatorInsets:", "v48@0:8{UIEdgeInsets=dddd}16"},
    {"UIKit", "setSelectedRange:", "v32@0:8{_NSRange=QQ}16"},
    {"UIKit", "setSemanticContentAttribute:", "v24@0:8q16"},
    {"UIKit", "setTextAlignment:", "v24@0:8q16"},
    {"UIKit", "setTextColor:", "v24@0:8@16"},
    {"UIKit", "setTextContainerInset:", "v48@0:8{UIEdgeInsets=dddd}16"},
    {"UIKit", "startAnimation", "v16@0:8"},
    {"UIKit", "systemImageNamed:", "@24@0:8@16"},
    {"UIKit", "textContainer", "@16@0:8"},
    {"UIKit", "textContainerInset", "{UIEdgeInsets=dddd}16@0:8"},
    {"UIKit", "textContentManager", "@16@0:8"},
    {"UIKit", "textLayoutManager", "@16@0:8"},
    {"UIKit", "translationInView:", "{CGPoint=dd}24@0:8@16"},
    {"UIKit", "velocityInView:", "{CGPoint=dd}24@0:8@16"},
    {"UIKit", "viewControllers", "@16@0:8"},
};
BinaryImage frameworkImage(const FixedAPI &API) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.NeededLibs = {std::string("/System/Library/Frameworks/") +
                              API.Framework + ".framework/" + API.Framework};
  return Image;
}
ExprPtr declaredCall(const FixedAPI &API, const SourceFunctionTypeHint &Type) {
  auto Call = HighExpr::makeCall("objc_msgSend", 0, {});
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint->Selector = API.Selector;
  Hint->Signature = Type;
  Call->SourceCallHint = Hint;
  Call->Type = Type.ReturnType;
  for (const auto &Parameter : Type.Parameters) {
    MedVar Input;
    Input.Id = int(Call->Operands.size()) + 1;
    Input.Size = Parameter.Type->Size;
    Call->Operands.push_back(HighExpr::makeVar(Input, Parameter.Type));
  }
  return Call;
}
class IOSFixedSDKCalls : public ::testing::TestWithParam<FixedAPI> {};
} // namespace

TEST_P(IOSFixedSDKCalls, RevalidatesIndependentCompilerABIAndProvider) {
  const auto &API = GetParam();
  const auto Image = frameworkImage(API);
  auto Expected = parseObjCMethodEncoding(API.Selector, API.Encoding);
  ASSERT_TRUE(Expected);
  Expected->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(*Expected, Image.Arch, Error));
  const auto Actual = objcSelectorSourceTypeHint(Image, API.Selector);
  ASSERT_TRUE(Actual);
  EXPECT_TRUE(equalSourceABIs(*Expected, *Actual));
  const auto Call = declaredCall(API, *Expected);
  ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Image;
    if (Mutation == 0)
      Changed.DynInfo.NeededLibs.clear();
    if (Mutation == 1)
      Changed.DynInfo.NeededLibs[0] =
          std::string("/tmp/") + API.Framework + ".framework/" + API.Framework;
    if (Mutation == 2)
      Changed.Arch = Arch::X64;
    if (Mutation == 3)
      Changed.Bits = Bitness::Bits32;
    if (Mutation == 4)
      Changed.Format = BinaryFormat::ELF;
    EXPECT_FALSE(objcSelectorSourceTypeHint(Changed, API.Selector));
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Changed, {}));
  }
}

TEST_P(IOSFixedSDKCalls, RejectsRuntimeConflictsAndChangedCarriers) {
  const auto &API = GetParam();
  auto Image = frameworkImage(API);
  const auto Type = objcSelectorSourceTypeHint(Image, API.Selector);
  ASSERT_TRUE(Type);
  const auto Call = declaredCall(API, *Type);
  ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
  auto Changed = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
  if (Changed->Signature.Parameters.size() > 2)
    Changed->Signature.Parameters.back().Location.ValueBytes = 3;
  else
    Changed->Signature.ReturnLocation.ValueBytes = 3;
  Call->SourceCallHint = Changed;
  EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
  Changed->Signature = *Type;
  std::string ConflictEncoding(API.Encoding);
  const auto FrameSize = ConflictEncoding.find_first_of("0123456789");
  ASSERT_NE(FrameSize, std::string::npos);
  ConflictEncoding = (Type->ReturnType->Kind == NdTypeKind::Float ? "q" : "d") +
                     ConflictEncoding.substr(FrameSize);
  ObjCMethod Conflict;
  Conflict.Selector = API.Selector;
  Conflict.TypeHint = parseObjCMethodEncoding(API.Selector, ConflictEncoding);
  ASSERT_TRUE(Conflict.TypeHint);
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(*Conflict.TypeHint, Image.Arch, Error));
  Image.ObjCMethods.push_back(Conflict);
  EXPECT_FALSE(objcSelectorSourceTypeHint(Image, API.Selector));
  EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
}

INSTANTIATE_TEST_SUITE_P(CurrentIOSSDK, IOSFixedSDKCalls,
                         ::testing::ValuesIn(APIs),
                         [](const ::testing::TestParamInfo<FixedAPI> &Info) {
                           std::string Name(Info.param.Selector);
                           std::replace(Name.begin(), Name.end(), ':', '_');
                           return Name;
                         });
