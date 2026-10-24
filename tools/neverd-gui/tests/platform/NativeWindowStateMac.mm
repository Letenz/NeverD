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
  if (QGuiApplication::platformName() != QLatin1String("cocoa"))
    return false;
  NSWindow *Native = reinterpret_cast<NSView *>(Window.winId()).window;
  if (!Native)
    return false;
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
  if (FullScreen)
    Window.showFullScreen();
  else
    Window.showNormal();
  const bool Result = QTest::qWaitFor([&] { return Finished; });
  [Center removeObserver:Observer];
  return Result;
}

} // namespace neverd::gui::test
