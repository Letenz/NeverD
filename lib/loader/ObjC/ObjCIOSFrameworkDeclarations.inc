// clang-format off
// Public UIKit API facts for arm64 iOS. Most entries were observed in both
// Xcode 26.5 iPhoneOS and arm64 iPhoneSimulator ASTs with agreeing desugared
// types, then independently encoded by Apple Clang. The image and layout
// additions below use Apple's published Objective-C declarations with their
// exact arm64 encodings independently checked by Apple Clang. Selectors whose
// owners disagree remain absent or are resolved by receiver evidence.
// The source AST SHA-256 values are:
//   iPhoneOS:       79259f6825fd8d118d351d98a4530e426f55faaaa569b172ab14256a6a840ba7
//   iPhoneSimulator:707c4d1d4bf7f49570de9ad5f880a9398cd16b594468177307ba53332ba418b7
// No x86_64 iPhoneSimulator declaration artifact was available, so that
// architecture remains unsupported instead of borrowing Mac Catalyst facts.
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "contextWithOptions:", "@24@0:8@16", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "createCGImage:fromRect:", "^{CGImage=}56@0:8@16{CGRect={CGPoint=dd}{CGSize=dd}}24", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "detectorOfType:context:options:", "@40@0:8@16@24@32", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "extent", "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", nullptr},
// CIDetector.h in MacOSX SDK 15.5 declares this exact instance method;
// arm64 macOS and Mac Catalyst Clang ASTs agree. The public iOS API has the
// same object result and two object parameters, and Apple Clang emits this
// encoding for an arm64 iOS simulator declaration. No x86_64 iOS fact is used.
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "featuresInImage:options:", "@32@0:8@16@24", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "filterWithName:", "@24@0:8@16", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "imageByCroppingToRect:", "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "outputImage", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGColor", "^{CGColor=}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGImage", "^{CGImage=}16@0:8", nullptr},
// NSValue's Core Graphics rectangle property has a 32-byte struct result.
// Apple Clang emits this encoding for arm64 iOS.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGRectValue", "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGSizeValue", "{CGSize=dd}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CIImage", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "activateConstraints:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "addSubview:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "addArrangedSubview:", "v24@0:8@16", nullptr},
// Public UIControl target/action declaration; UIControlEvents is NSUInteger.
// Apple Clang independently agrees on arm64 iOS device/simulator encodings.
// https://developer.apple.com/documentation/uikit/uicontrol/addtarget(_:action:for:)?language=objc
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "addTarget:action:forControlEvents:", "v40@0:8@16:24Q32", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "alpha", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "blackColor", "@16@0:8", nullptr},
// UIDevice publicly declares these paired void orientation-notification
// methods without parameters; Apple Clang emits v16@0:8 for arm64 iOS.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "beginGeneratingDeviceOrientationNotifications", "v16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "clearColor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "centerXAnchor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "colorWithAlphaComponent:", "@24@0:8d16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "colorWithRed:green:blue:alpha:", "@48@0:8d16d24d32d40", nullptr},
// Xcode 26.5's complete device/simulator Messages ASTs have one owner,
// UIColor, declaring a non-variadic class factory returning UIColor* with
// two CGFloat (double) inputs. Apple Clang independently agrees on this
// encoding for arm64 iOS and arm64 iOS Simulator. Source AST SHA-256:
// iPhoneOS:        f17e93958dc48979a422993dd92139ff1d897a026e123e6b1644447f28ee48e4
// iPhoneSimulator: 078316806bd538d05dcdd08899843095d1dccd3f70c0a3655fcd86bb26467335
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "colorWithWhite:alpha:", "@32@0:8d16d24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "currentDevice", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "currentEDRHeadroom", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "contentLayoutGuide", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "dismissViewControllerAnimated:completion:", "v28@0:8B16@?20", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "endGeneratingDeviceOrientationNotifications", "v16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "displayScale", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "frameLayoutGuide", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "getRed:green:blue:alpha:", "B48@0:8^d16^d24^d32^d40", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "getWhite:alpha:", "B32@0:8^d16^d24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "heightAnchor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageByPreparingForDisplay", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageFlippedForRightToLeftLayoutDirection", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageForState:", "@24@0:8Q16", nullptr},
// Apple's public UIImage declaration has three object inputs and an image
// result. Apple Clang independently emits this exact arm64 encoding.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageNamed:inBundle:withConfiguration:", "@40@0:8@16@24@32", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageOrientation", "q16@0:8", nullptr},
// UIImage's readonly imageRendererFormat property returns an object pointer.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageRendererFormat", "@16@0:8", nullptr},
// All owners in the complete Xcode 26.5 Messages device/simulator ASTs
// agree on these non-variadic NSArray* getters; Apple Clang independently
// encodes their arm64 calls as @16@0:8. AST hashes match colorWithWhite:alpha:.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "images", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "viewControllers", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageWithCGImage:", "@24@0:8^{CGImage=}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageWithCIImage:scale:orientation:", "@40@0:8@16d24q32", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "initWithCGImage:", "@24@0:8^{CGImage=}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "initWithProgressViewStyle:", "@24@0:8q16", nullptr},
// Public UIGestureRecognizer initializer: object result, target object and SEL.
// This is an independently encoded public declaration, not a device SDK AST.
// https://developer.apple.com/documentation/uikit/uigesturerecognizer/init(target:action:)?language=objc
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "initWithTarget:action:", "@32@0:8@16:24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "initWithRed:green:blue:alpha:", "@48@0:8d16d24d32d40", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "insertSubview:belowSubview:", "v32@0:8@16@24", nullptr},
// UICollectionViewLayoutInvalidationContext declares both readonly BOOL
// properties; Apple Clang encodes their arm64 getters as B16@0:8.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "invalidateDataSourceCounts", "B16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "invalidateEverything", "B16@0:8", nullptr},
// NSLayoutAnchor's public fixed factory takes another anchor and returns an
// NSLayoutConstraint. UIView and UILayoutGuide expose these object anchors;
// Apple Clang independently emits their exact arm64 encodings.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "constraintEqualToAnchor:", "@24@0:8@16", nullptr},
// NSLayoutAnchor and NSLayoutDimension accept a CGFloat constant in the first
// floating-point argument register on arm64 iOS.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "constraintEqualToAnchor:constant:", "@32@0:8@16d24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "constraintEqualToConstant:", "@24@0:8d16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "leadingAnchor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "trailingAnchor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "topAnchor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "bottomAnchor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "instantiateWithOwner:options:", "@32@0:8@16@24", nullptr},
// The complete device/simulator ASTs cited for UIProgressView below also agree
// on UIView(UIConstraintBasedLayoutLayering)'s void invalidation method and
// UIButton's UIColor* plus UIControlState (underlying unsigned long) setter.
// These are the only owners of either selector in both complete artifacts.
// Apple Clang independently reproduces both encodings from those canonical
// declaration types for arm64 iOS and arm64 iOS Simulator.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "invalidateIntrinsicContentSize", "v16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "mainScreen", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "nibWithNibName:bundle:", "@32@0:8@16@24", nullptr},
// Public UIViewController object getters, also present in the MacOSX 15.5
// UIKit header inventory. Every SDK owner agrees on the object result and
// absence of explicit parameters; Apple Clang independently encodes these
// declarations for arm64 iOS and arm64 iOS Simulator as @16@0:8.
// https://developer.apple.com/documentation/uikit/uiviewcontroller/viewifloaded?language=objc
// https://developer.apple.com/documentation/uikit/uiviewcontroller/view?language=objc
// https://developer.apple.com/documentation/uikit/uiviewcontroller/navigationitem?language=objc
// https://developer.apple.com/documentation/uikit/uiviewcontroller/navigationcontroller?language=objc
// https://developer.apple.com/documentation/uikit/uiviewcontroller/presentedviewcontroller?language=objc
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "viewIfLoaded", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "view", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "navigationItem", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "navigationController", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "presentedViewController", "@16@0:8", nullptr},
// UIProgressView additions: both complete Xcode 26.5 Messages ASTs from
// GitHub Actions artifact 10653622473 agree on every owner of these selectors.
// iPhoneOS SHA-256: dc99c06a9e4aab8015f8675b08ac55e042201401c21d39a4df4535aa63e0303f
// Simulator SHA-256: c443f389aae208205f42b4a592859ffb45766cc6e2f663b6ef919528e2676b61
// Apple Clang independently encodes the observed NSProgress*, float and bool
// declaration types for both arm64 iOS targets; neither is variadic.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "observedProgress", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "potentialEDRHeadroom", "d16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "resignFirstResponder", "B16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "safeAreaLayoutGuide", "@16@0:8", nullptr},
// UIView's public iOS properties and the local UIKit header inventory agree
// on signed NSInteger layout direction and the four-Double inset result.
// Apple Clang independently emits these encodings on both arm64 iOS targets.
// https://developer.apple.com/documentation/uikit/uiview/effectiveuserinterfacelayoutdirection?language=objc
// https://developer.apple.com/documentation/uikit/uiview/safeareainsets?language=objc
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "effectiveUserInterfaceLayoutDirection", "q16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "safeAreaInsets", "{UIEdgeInsets=dddd}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "scale", "d16@0:8", nullptr},
// UIColor's public no-argument instance method has this exact arm64 ABI;
// unqualified selector use is admitted only when local declarations agree.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setStroke", "v16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "sendActionsForControlEvents:", "v24@0:8Q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setAccessibilityIgnoresInvertColors:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setAttributedText:", "v24@0:8@16", nullptr},
// NSLayoutConstraint.active is BOOL. The arm64 iOS Objective-C method has a
// one-byte Boolean input in the third integer argument register.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setActive:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setActivityIndicatorViewStyle:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setAdjustsFontForContentSizeCategory:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setCenter:", "v32@0:8{CGPoint=dd}16", nullptr},
// UIView and UIAccessibility Boolean setters share this exact fixed ABI.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setClipsToBounds:", "v20@0:8B16", nullptr},
// UIScrollView's public iOS properties declare BOOL and UIEdgeInsets. The
// MacOSX SDK's UIKit inventory agrees on both selectors and their sole owner;
// Apple Clang independently encodes the published types on arm64 iOS and
// arm64 iOS Simulator. UIEdgeInsets keeps top, left, bottom and right doubles.
// https://developer.apple.com/documentation/uikit/uiscrollview/bounces?language=objc
// https://developer.apple.com/documentation/uikit/uiscrollview/contentinset?language=objc
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setBounces:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setContentInset:", "v48@0:8{UIEdgeInsets=dddd}16", nullptr},
// Layout controls: all declaration owners agree in both complete Xcode 26.5
// Messages ASTs from GitHub Actions artifact 11077989891. Their bool, long
// enum, double and object types independently produce these encodings with
// Apple Clang for arm64 iOS and arm64 iOS Simulator. setAlignment: also has
// paragraph/content-configuration owners; setSpacing: has a glass-effect
// owner. All agree on the complete fixed ABI, not merely the selector name.
// iPhoneOS SHA-256: 8836f6b50c979975b1d3e2a365bf23ec1b7304ff42b26393bc713d2c1be1a163
// Simulator SHA-256: a028c7a405bc849dc7f899de99fe1e4d26984b24872088a28ac01746efd868fc
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "subviews", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "insertSubview:atIndex:", "v32@0:8@16q24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setShowsHorizontalScrollIndicator:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setShowsVerticalScrollIndicator:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setAxis:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setDistribution:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setAlignment:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setSpacing:", "v24@0:8d16", nullptr},
// UIView's public layout-priority setters take float UILayoutPriority and a
// signed NSInteger-backed axis. Apple Clang emits this packed arm64 encoding
// for both methods; the ABI keeps s0 and x2 in separate register banks.
// https://developer.apple.com/documentation/uikit/uiview/setcontenthuggingpriority(_:for:)
// https://developer.apple.com/documentation/uikit/uiview/setcontentcompressionresistancepriority(_:for:)
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setContentCompressionResistancePriority:forAxis:", "v28@0:8f16q20", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setContentHuggingPriority:forAxis:", "v28@0:8f16q20", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setIsAccessibilityElement:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setLayoutMargins:", "v48@0:8{UIEdgeInsets=dddd}16", nullptr},
// Both ASTs cited above also declare setProgress: with incompatible owners:
// UIProgressView takes float, while UIPageControl takes an object pointer.
// Keep both alternatives; only receiver evidence may select either one.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setProgress:", "v20@0:8f16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setProgress:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setProgress:animated:", "v24@0:8f16B20", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setProgressViewStyle:", "v24@0:8q16", nullptr},
// The complete device/simulator ASTs cited above each have exactly one
// setShowsCancelButton:animated: owner (UISearchBar), with two BOOL parameters.
// Apple Clang independently encodes this signature as v24@0:8B16B20.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setShowsCancelButton:animated:", "v24@0:8B16B20", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setText:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setTitleColor:forState:", "v32@0:8@16Q24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setTranslatesAutoresizingMaskIntoConstraints:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setUserInteractionEnabled:", "v20@0:8B16", nullptr},
// The device/simulator ASTs agree that both NSTextAttachmentViewProvider and
// UIViewController declare setView: with one nullable UIView* argument.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setView:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "sizeToFit", "v16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "superview", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "systemVersion", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "topViewController", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "traitCollection", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "userInterfaceIdiom", "q16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "window", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "whiteColor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "widthAnchor", "@16@0:8", nullptr},
    // Additional fixed declarations from complete Xcode 26.5 device/simulator ASTs.
// SHA-256: dc99c06a9e4aab8015f8675b08ac55e042201401c21d39a4df4535aa63e0303f
//          c443f389aae208205f42b4a592859ffb45766cc6e2f663b6ef919528e2676b61
// UIImage BOOL is bool; UIActivityIndicatorViewStyle is a signed long enum.
// Both selectors have one agreeing declaration owner in each complete AST.
// Apple Clang encodes these canonical declarations on both arm64 iOS targets.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "isHighDynamicRange", "B16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "initWithActivityIndicatorStyle:", "@24@0:8q16", nullptr},
// The same complete ASTs declare these five fixed selectors without conflicts.
// UIBackgroundTaskIdentifier is unsigned long on both arm64 iOS targets.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "interactivePopGestureRecognizer", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "parentViewController", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "transitionCoordinator", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "flashScrollIndicators", "v16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "endBackgroundTask:", "v24@0:8Q16", nullptr},
// Fixed UIKit/CoreImage APIs observed in both complete Xcode 26.5 Messages
// ASTs from artifact 11077989891. Every owner of each selector agrees on
// the full fixed signature. Enum signedness/width and every aggregate field
// also agree, and independent Apple Clang encoding for both arm64 iOS
// targets produces these rows. No block lifetime or dynamic receiver class
// is inferred from these selector-wide ABI facts.
// iPhoneOS SHA-256: 8836f6b50c979975b1d3e2a365bf23ec1b7304ff42b26393bc713d2c1be1a163
// Simulator SHA-256: a028c7a405bc849dc7f899de99fe1e4d26984b24872088a28ac01746efd868fc
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGContext", "^{CGContext=}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGPath", "r^{CGPath=}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "addGestureRecognizer:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "addLineToPoint:", "v32@0:8{CGPoint=dd}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "animatedImageWithImages:duration:", "@32@0:8@16d24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "attributedText", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "becomeFirstResponder", "B16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "bezierPathWithRect:", "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "cellForItemAtIndexPath:", "@24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "centerYAnchor", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "collectionView", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "contentOffset", "{CGPoint=dd}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "contentView", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "convertPoint:toCoordinateSpace:", "{CGPoint=dd}40@0:8{CGPoint=dd}16@32", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "convertRect:toCoordinateSpace:", "{CGRect={CGPoint=dd}{CGSize=dd}}56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@48", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "currentTraitCollection", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "firstBaselineAnchor", "@16@0:8", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "imageByApplyingFilter:withInputParameters:", "@32@0:8@16@24", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "imageWithColor:", "@24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "impactOccurred", "v16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "initForTextStyle:", "@24@0:8@16", nullptr},
{"CoreImage", "/System/Library/Frameworks/CoreImage.framework/CoreImage", "initWithColor:", "@24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "initWithStyle:", "@24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "layoutDirection", "q16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "layoutFrame", "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "layoutMargins", "{UIEdgeInsets=dddd}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "leftBarButtonItem", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "moveToPoint:", "v32@0:8{CGPoint=dd}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "presentationController", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "readableContentGuide", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "scaledValueForValue:", "d24@0:8d16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "scrollRangeToVisible:", "v32@0:8{_NSRange=QQ}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "selectedBackgroundView", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "selectedRange", "{_NSRange=QQ}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "semanticContentAttribute", "q16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setBarTintColor:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setBaseWritingDirection:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setEditable:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setExclusionPaths:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setImage:forState:", "v32@0:8@16Q24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setItems:animated:", "v28@0:8@16B24", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setLeftBarButtonItem:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setLineBreakMode:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setLineFragmentPadding:", "v24@0:8d16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setLineHeightMultiple:", "v24@0:8d16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setLineSpacing:", "v24@0:8d16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setLinkTextAttributes:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setMaximumLineHeight:", "v24@0:8d16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setNumberOfLines:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setRightBarButtonItem:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setRightView:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setRightViewMode:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setScrollEnabled:", "v20@0:8B16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setScrollIndicatorInsets:", "v48@0:8{UIEdgeInsets=dddd}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setSelectedRange:", "v32@0:8{_NSRange=QQ}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setSemanticContentAttribute:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setTextAlignment:", "v24@0:8q16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setTextColor:", "v24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setTextContainerInset:", "v48@0:8{UIEdgeInsets=dddd}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "startAnimation", "v16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "systemImageNamed:", "@24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "textContainer", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "textContainerInset", "{UIEdgeInsets=dddd}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "textContentManager", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "textLayoutManager", "@16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "translationInView:", "{CGPoint=dd}24@0:8@16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "velocityInView:", "{CGPoint=dd}24@0:8@16", nullptr},
// UIButton's three inset properties belong to its anonymous category in
// both complete Xcode 26.5 Messages ASTs from artifact 11077989891. Each
// UIEdgeInsets record contains top/left/bottom/right doubles; Apple Clang
// independently agrees on the six arm64 device/simulator encodings.
// iPhoneOS: 8836f6b50c979975b1d3e2a365bf23ec1b7304ff42b26393bc713d2c1be1a163
// Simulator: a028c7a405bc849dc7f899de99fe1e4d26984b24872088a28ac01746efd868fc
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "contentEdgeInsets", "{UIEdgeInsets=dddd}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setContentEdgeInsets:", "v48@0:8{UIEdgeInsets=dddd}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "imageEdgeInsets", "{UIEdgeInsets=dddd}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setImageEdgeInsets:", "v48@0:8{UIEdgeInsets=dddd}16", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "titleEdgeInsets", "{UIEdgeInsets=dddd}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setTitleEdgeInsets:", "v48@0:8{UIEdgeInsets=dddd}16", nullptr},
    // clang-format on
