//===- NativeWindowStateMac.mm - Cocoa transition completion tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Observe AppKit completion instead of an eagerly updated Qt state flag.
///
//===----------------------------------------------------------------------===//

#include "../NativeWindowState.h"

#import <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QTest>
#include <QWidget>

namespace neverd::gui::test {

bool setNativeFullScreen(QWidget &Window, bool FullScreen) {
  return setNativeWindowState(Window, FullScreen ? Qt::WindowFullScreen
                                                 : Qt::WindowNoState);
}

bool setNativeWindowState(QWidget &Window, Qt::WindowStates States) {
  if (QGuiApplication::platformName() != QLatin1String("cocoa"))
    return false;
  NSWindow *Native = reinterpret_cast<NSView *>(Window.winId()).window;
  if (!Native)
    return false;
  const bool FullScreen = States.testFlag(Qt::WindowFullScreen);
  if (FullScreen == bool(Native.styleMask & NSWindowStyleMaskFullScreen)) {
    Window.setWindowState(States);
    return true;
  }
  bool Finished = false;
  bool *Completion = &Finished;
  NSNotificationCenter *Center = NSNotificationCenter.defaultCenter;
  id Observer = [Center
      addObserverForName:FullScreen ? NSWindowDidEnterFullScreenNotification
                                    : NSWindowDidExitFullScreenNotification
                  object:Native
                   queue:nil
              usingBlock:^(NSNotification *) {
                *Completion = true;
              }];
  Window.setWindowState(States);
  const bool Result = QTest::qWaitFor([&] { return Finished; });
  [Center removeObserver:Observer];
  return Result;
}

} // namespace neverd::gui::test
