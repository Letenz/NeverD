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
/// Visual Studio Code's custom title bar with its command center, which a
/// window off Cocoa (offscreen) draws whole.
constexpr int DrawnTitleBarHeight = 35;
/// The traffic lights' share of the title bar, which Visual Studio Code
/// keeps clear for them.
constexpr int TrafficLightsWidth = 70;
/// The system setting for a double click on a title bar, and its values
/// other than the default, which zooms the window.
NSString *const DoubleClickActionKey = @"AppleActionOnDoubleClick";
NSString *const MinimizeAction = @"Minimize";
NSString *const NoAction = @"None";

/// Whether windows are Cocoa's, whose native handles are NSViews.
bool onCocoa() {
  return QGuiApplication::platformName() == QLatin1String(CocoaPlatform);
}

NSWindow *nativeWindow(QWindow &window) {
  if (!onCocoa())
    return nil;
  return reinterpret_cast<NSView *>(window.winId()).window;
}
} // namespace

FrameKind frameKind() {
  return onCocoa() ? FrameKind::Captionless : FrameKind::System;
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

// Off Cocoa no system frame or traffic lights serve the window, so its title
// bar draws and does everything itself.
Qt::Edges ownResizeEdges() {
  return onCocoa()
             ? Qt::Edges()
             : Qt::LeftEdge | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge;
}

int titleBarHeight() {
  return onCocoa() ? TitleBarHeight : DrawnTitleBarHeight;
}

int systemControlsWidth() { return onCocoa() ? TrafficLightsWidth : 0; }

bool drawsWindowControls() { return !onCocoa(); }

bool showsWindowIcon() { return !onCocoa(); }

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
