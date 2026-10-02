// clang-format off
// Bounded public UIKit receiver facts. The UIImage and renderer declarations
// were observed in both Xcode 26.5 arm64 iPhoneOS and iPhoneSimulator Clang
// ASTs; other owners carry their own evidence below. These are receiver
// declarations, not globally unique selector signatures. The UIImage
// interface declares NSObject as superclass and adopts NSSecureCoding. Both
// ASTs agree on its methods and their non-variadic canonical types. Enum
// UIImageOrientation has underlying type long; CGRect contains CGPoint and
// CGSize, each holding two doubles, in that order. Renderer format ownership
// and NSCopying adoption also agree in the device and simulator artifacts.
// Exact runtime encodings were checked by compiling those canonical method
// declarations with Apple Clang for arm64-apple-ios18.0; this does not claim a
// local compilation of the original UIKit headers. No x86_64 evidence exists.
// Source AST SHA-256:
// iPhoneOS:        79259f6825fd8d118d351d98a4530e426f55faaaa569b172ab14256a6a840ba7
// iPhoneSimulator: 707c4d1d4bf7f49570de9ad5f880a9398cd16b594468177307ba53332ba418b7
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", "C:NSObject|P:NSSecureCoding", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "images", "@16@0:8", "NSArray", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "CIImage", "@16@0:8", "CIImage", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "scale", "d16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "duration", "d16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "imageFlippedForRightToLeftLayoutDirection", "@16@0:8", "UIImage", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "imageOrientation", "q16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", true, "imageWithCGImage:", "@24@0:8^{CGImage=}16", "UIImage", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", true, "imageWithCGImage:scale:orientation:", "@40@0:8^{CGImage=}16d24q32", "UIImage", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "initWithCGImage:scale:orientation:", "@40@0:8^{CGImage=}16d24q32", "", true, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "drawInRect:", "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16", "", false, nullptr, "", false)
// Apple's public Objective-C UIBezierPath/UIColor declarations name these
// receiver owners, result identity, and drawing methods. Apple Clang's arm64
// iOS encoding of the rounded-rectangle factory places CGRect, UIRectCorner,
// and CGSize at runtime offsets 16, 48, and 56, respectively.
// https://developer.apple.com/documentation/uikit/uibezierpath/init%28roundedrect%3Abyroundingcorners%3Acornerradii%3A%29?language=objc
// https://developer.apple.com/documentation/uikit/uicolor/setstroke%28%29
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIBezierPath", "", "C:NSObject|P:NSCopying|P:NSSecureCoding", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIBezierPath", "", true, "bezierPathWithRoundedRect:byRoundingCorners:cornerRadii:", "@72@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16Q48{CGSize=dd}56", "", true, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIBezierPath", "", false, "closePath", "v16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIBezierPath", "", false, "addClip", "v16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIBezierPath", "", false, "stroke", "v16@0:8", "", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIColor", "", "C:NSObject|P:NSCopying", nullptr)
// The complete device/simulator ASTs cited for this factory in
// ObjCIOSFrameworkDeclarations.inc return UIColor*, not instancetype.
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIColor", "", true, "colorWithWhite:alpha:", "@32@0:8d16d24", "UIColor", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIColor", "", false, "setStroke", "v16@0:8", "", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScreen", "", "C:NSObject", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScreen", "", true, "mainScreen", "@16@0:8", "UIScreen", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScreen", "", false, "scale", "d16@0:8", "", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsRendererFormat", "", "C:NSObject|P:NSCopying", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsImageRendererFormat", "", "C:UIGraphicsRendererFormat", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsRendererFormat", "", true, "defaultFormat", "@16@0:8", "", true, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsImageRendererFormat", "", false, "scale", "d16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsImageRendererFormat", "", false, "setScale:", "v24@0:8d16", "", false, nullptr, "", false)
// MacOSX SDK 15.5's public UIKit headers agree across arm64/x86_64 iOS
// simulator and Mac Catalyst profiles on this renderer hierarchy and the
// complete method encodings. The image action parameter is independently
// recorded as nonescaping by ObjCSourceDeclarations.cpp.
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsRenderer", "", "C:NSObject", "C:NSObject")
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsImageRenderer", "", "C:UIGraphicsRenderer", "C:UIGraphicsRenderer")
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsImageRenderer", "", false, "initWithSize:", "@32@0:8{CGSize=dd}16", "", true, "@32@0:8{CGSize=dd}16", "", true)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsImageRenderer", "", false, "initWithSize:format:", "@40@0:8{CGSize=dd}16@32", "", true, "@40@0:8{CGSize=dd}16@32", "", true)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGraphicsImageRenderer", "", false, "imageWithActions:", "@24@0:8@?16", "UIImage", false, "@24@0:8@?16", "UIImage", false)
// UIProgressView receiver closure, observed in the complete Xcode 26.5 ASTs
// from GitHub Actions artifact 10653622473. Every superclass/protocol edge
// agrees; no parent declares setProgress:. Foundation and QuartzCore retain
// ownership of their existing parent declarations. Apple Clang independently
// encodes setProgress:(float) on both arm64 iOS targets as v20@0:8f16.
// iPhoneOS SHA-256: dc99c06a9e4aab8015f8675b08ac55e042201401c21d39a4df4535aa63e0303f
// Simulator SHA-256: c443f389aae208205f42b4a592859ffb45766cc6e2f663b6ef919528e2676b61
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIProgressView", "", "C:UIView|P:NSCoding", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIResponder", "", "C:NSObject|P:UIResponderStandardEditActions", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIView", "", "C:UIResponder|P:NSCoding|P:UIAppearance|P:UIAppearanceContainer|P:UIDynamicItem|P:UITraitEnvironment|P:UICoordinateSpace|P:UIFocusItem|P:UIFocusItemContainer|P:CALayerDelegate", nullptr)
// UIView and its public UIViewHierarchy category declare these properties;
// their iOS declarations and encodings are recorded in the selector catalog.
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIView", "", false, "effectiveUserInterfaceLayoutDirection", "q16@0:8", "", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIView", "UIViewHierarchy", "", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIView", "UIViewHierarchy", false, "safeAreaInsets", "{UIEdgeInsets=dddd}16@0:8", "", false, nullptr, "", false)
// The complete device/simulator ASTs cited for layout controls in the
// selector catalog agree on UIViewHierarchy, UIScrollView and UIStackView.
// UIStackView directly inherits UIView and adopts no additional protocols.
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIView", "UIViewHierarchy", false, "subviews", "@16@0:8", "NSArray", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIView", "UIViewHierarchy", false, "insertSubview:atIndex:", "v32@0:8@16q24", "", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIStackView", "", "C:UIView", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIStackView", "", false, "setAxis:", "v24@0:8q16", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIStackView", "", false, "setDistribution:", "v24@0:8q16", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIStackView", "", false, "setAlignment:", "v24@0:8q16", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIStackView", "", false, "setSpacing:", "v24@0:8d16", "", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIAppearance", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIAppearanceContainer", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UICoordinateSpace", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIDynamicItem", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIFocusEnvironment", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIFocusItem", "", "P:UIFocusEnvironment", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIFocusItemContainer", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIResponderStandardEditActions", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UITraitEnvironment", "", "P:NSObject", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIProgressView", "", false, "setProgress:", "v20@0:8f16", "", false, nullptr, "", false)
// The same complete ASTs declare UIImageView with superclass UIView and no
// directly adopted protocols. This closes the inherited receiver scope for
// local subclasses without assigning a global type to their own selectors.
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImageView", "", "C:UIView", nullptr)
// The same complete ASTs above declare UIImage's bool HDR property and the
// UIActivityIndicatorView initializer, returning instancetype with a long enum.
// Preserve the activity indicator's exact UIView/NSCoding receiver closure.
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIImage", "", false, "isHighDynamicRange", "B16@0:8", "", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIActivityIndicatorView", "", "C:UIView|P:NSCoding", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIActivityIndicatorView", "", false, "initWithActivityIndicatorStyle:", "@24@0:8q16", "", true, nullptr, "", false)
// Navigation, scrolling and application declarations from the same complete
// device/simulator ASTs; retain their exact receiver hierarchy and results.
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIViewController", "", "C:UIResponder|P:NSCoding|P:UIAppearanceContainer|P:UITraitEnvironment|P:UIContentContainer|P:UIFocusEnvironment", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UINavigationController", "", "C:UIViewController", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScrollView", "", "C:UIView|P:NSCoding|P:UIFocusItemScrollableContainer", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIGestureRecognizer", "", "C:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIContentContainer", "", "P:NSObject", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIApplication", "", "C:UIResponder", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "protocol", "UIFocusItemScrollableContainer", "", "P:UIFocusItemContainer", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UINavigationController", "", false, "interactivePopGestureRecognizer", "@16@0:8", "UIGestureRecognizer", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UINavigationController", "", false, "viewControllers", "@16@0:8", "NSArray", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIViewController", "", false, "parentViewController", "@16@0:8", "UIViewController", false, nullptr, "", false)
// The public getter declarations cited in the selector catalog agree with
// the local UIKit header inventory on these owners and result classes.
// Preserve both additional owners of navigationItem/presentedViewController;
// selector agreement alone does not establish a receiver's class.
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIViewController", "", false, "viewIfLoaded", "@16@0:8", "UIView", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIViewController", "", false, "view", "@16@0:8", "UIView", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIViewController", "", false, "presentedViewController", "@16@0:8", "UIViewController", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIViewController", "UINavigationControllerItem", "", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIViewController", "UINavigationControllerItem", false, "navigationItem", "@16@0:8", "UINavigationItem", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIViewController", "UINavigationControllerItem", false, "navigationController", "@16@0:8", "UINavigationController", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UINavigationItem", "", "C:NSObject|P:NSCoding", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UISearchDisplayController", "", "C:NSObject", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UISearchDisplayController", "", false, "navigationItem", "@16@0:8", "UINavigationItem", false, nullptr, "", false)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIPresentationController", "", "C:NSObject|P:UIAppearanceContainer|P:UIContentContainer|P:UIFocusEnvironment|P:UITraitEnvironment", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIPresentationController", "", false, "presentedViewController", "@16@0:8", "UIViewController", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIViewController", "", false, "transitionCoordinator", "@16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScrollView", "", false, "flashScrollIndicators", "v16@0:8", "", false, nullptr, "", false)
// Public UIScrollView property declarations and independent arm64 iOS
// encodings are recorded with the selector facts in ObjCIOSFrameworkDeclarations.inc.
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScrollView", "", false, "setBounces:", "v20@0:8B16", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScrollView", "", false, "setContentInset:", "v48@0:8{UIEdgeInsets=dddd}16", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScrollView", "", false, "setShowsHorizontalScrollIndicator:", "v20@0:8B16", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIScrollView", "", false, "setShowsVerticalScrollIndicator:", "v20@0:8B16", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIApplication", "", false, "endBackgroundTask:", "v24@0:8Q16", "", false, nullptr, "", false)
// The same complete device/simulator ASTs cited for UIButton inset methods
// in the selector catalog retain UIControl -> UIView, UIButton -> UIControl
// plus NSCoding, and the anonymous UIButton category declaring all six APIs.
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIControl", "", "C:UIView", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "class", "UIButton", "", "C:UIControl|P:NSCoding", nullptr)
ND_OBJC_OWNER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIButton", "", "", nullptr)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIButton", "", false, "contentEdgeInsets", "{UIEdgeInsets=dddd}16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIButton", "", false, "setContentEdgeInsets:", "v48@0:8{UIEdgeInsets=dddd}16", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIButton", "", false, "imageEdgeInsets", "{UIEdgeInsets=dddd}16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIButton", "", false, "setImageEdgeInsets:", "v48@0:8{UIEdgeInsets=dddd}16", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIButton", "", false, "titleEdgeInsets", "{UIEdgeInsets=dddd}16@0:8", "", false, nullptr, "", false)
ND_OBJC_MEMBER("UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "category", "UIButton", "", false, "setTitleEdgeInsets:", "v48@0:8{UIEdgeInsets=dddd}16", "", false, nullptr, "", false)
// clang-format on
