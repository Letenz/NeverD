//===- WindowFrameWindows.cpp - Windows frame integration ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep Windows window management without system-drawn edges or shadows.
//===----------------------------------------------------------------------===//

#include "WindowFramePlatform.h"

#include <QGuiApplication>
#include <QWindow>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The DWM SDK header uses Windows types.
#include <dwmapi.h>

namespace neverd::gui::platform {
void configureFrame(QWindow &window) {
  if (QGuiApplication::platformName() != QLatin1String("windows"))
    return;
  const auto hwnd = reinterpret_cast<HWND>(window.winId());
  const DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
  DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy,
                        sizeof(policy));
  const MARGINS margins{};
  DwmExtendFrameIntoClientArea(hwnd, &margins);
}

void setShadowMargins(QWindow &, const QMargins &) {}
} // namespace neverd::gui::platform
