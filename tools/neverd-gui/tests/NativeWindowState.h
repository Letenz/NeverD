//===- NativeWindowState.h - Native window transition tests ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Wait for native window transitions before checking their final geometry.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_TESTS_NATIVEWINDOWSTATE_H
#define NEVERD_TOOLS_NEVERD_GUI_TESTS_NATIVEWINDOWSTATE_H

#include <QtCore/qnamespace.h>
#include <functional>

class QWidget;

namespace neverd::gui::test {

bool setNativeFullScreen(QWidget &Window, bool FullScreen);
bool setNativeWindowState(QWidget &Window, Qt::WindowStates States,
                          const std::function<void()> &Apply = {});

} // namespace neverd::gui::test

#endif // NEVERD_TOOLS_NEVERD_GUI_TESTS_NATIVEWINDOWSTATE_H
