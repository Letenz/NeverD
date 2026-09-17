// clang-format off
// Bounded public UIKit receiver facts, observed in both Xcode 26.5 arm64
// iPhoneOS and iPhoneSimulator Clang ASTs. These are receiver declarations,
// not globally unique selector signatures. The UIImage interface declares
// NSObject as superclass and adopts NSSecureCoding. Both ASTs agree on the
// instance methods below, their zero explicit parameters, non-variadic shape,
// and canonical return types (NSArray *, CIImage *, double, double).
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
// clang-format on
