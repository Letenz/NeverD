// macOS: the window keeps its frame and its traffic lights; the title bar
// turns transparent and the window's content reaches under it, as Visual
// Studio Code's custom title bar does there.

#include "WindowFramePlatform.h"

#import <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QWindow>

namespace neverd::gui::platform {
namespace {
constexpr char CocoaPlatform[] = "cocoa";
/// A standard title bar, in whose height the traffic lights are centered.
constexpr int TitleBarHeight = 28;
/// The traffic lights' share of the title bar, which Visual Studio Code
/// keeps clear for them.
constexpr int TrafficLightsWidth = 70;
/// The system setting for a double click on a title bar, and its values
/// other than the default, which zooms the window.
NSString *const DoubleClickActionKey = @"AppleActionOnDoubleClick";
NSString *const MinimizeAction = @"Minimize";
NSString *const NoAction = @"None";

NSWindow *nativeWindow(QWindow &window) {
  return reinterpret_cast<NSView *>(window.winId()).window;
}
} // namespace

FrameKind frameKind() {
  return QGuiApplication::platformName() == QLatin1String(CocoaPlatform)
             ? FrameKind::Captionless
             : FrameKind::System;
}

bool drawsShadow() { return false; }

void setShadowMargins(QWindow &, const QMargins &) {}

void removeCaption(QWindow &window) {
  NSWindow *native = nativeWindow(window);
  if (!native)
    return;
  native.styleMask |= NSWindowStyleMaskFullSizeContentView;
  native.titlebarAppearsTransparent = YES;
  native.titleVisibility = NSWindowTitleHidden;
}

QMargins maximizedInsets(QWindow &) { return {}; }

Qt::Edges ownResizeEdges() { return {}; }

int titleBarHeight() { return TitleBarHeight; }

int systemControlsWidth() { return TrafficLightsWidth; }

bool drawsWindowControls() { return false; }

bool showsWindowIcon() { return false; }

bool doubleClickTitleBar(QWindow &window) {
  NSWindow *native = nativeWindow(window);
  if (!native)
    return false;
  NSString *action =
      [NSUserDefaults.standardUserDefaults stringForKey:DoubleClickActionKey];
  if ([action isEqualToString:MinimizeAction])
    [native performMiniaturize:nil];
  else if (![action isEqualToString:NoAction])
    [native performZoom:nil];
  return true;
}

} // namespace neverd::gui::platform
