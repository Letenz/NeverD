// clang-format off
// Compiler-observed public UIKit API facts for arm64 iOS.
// Xcode 26.5 iPhoneOS and arm64 iPhoneSimulator ASTs agree on each owned
// declaration and its desugared types. Exact arm64 Objective-C encodings were
// independently produced from the public UIKit headers by Apple Clang.
// The source AST SHA-256 values are:
//   iPhoneOS:       79259f6825fd8d118d351d98a4530e426f55faaaa569b172ab14256a6a840ba7
//   iPhoneSimulator:707c4d1d4bf7f49570de9ad5f880a9398cd16b594468177307ba53332ba418b7
// No x86_64 iPhoneSimulator declaration artifact was available, so that
// architecture remains unsupported instead of borrowing Mac Catalyst facts.
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "CGImage", "^{CGImage=}16@0:8", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "dismissViewControllerAnimated:completion:", "v28@0:8B16@?20", nullptr},
{"UIKit", "/System/Library/Frameworks/UIKit.framework/UIKit", "setActivityIndicatorViewStyle:", "v24@0:8q16", nullptr},
    // clang-format on
