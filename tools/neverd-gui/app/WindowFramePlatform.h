//===- WindowFramePlatform.h - Native frame integration -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Platform integration for application-painted top-level windows.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_WINDOWFRAMEPLATFORM_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_WINDOWFRAMEPLATFORM_H

#include <QMargins>

class QWindow;

namespace neverd::gui::platform {
/// Disable platform decoration after creating or moving a native window.
void configureFrame(QWindow &window);
/// Tell the compositor which part of the surface is our shadow.
void setShadowMargins(QWindow &window, const QMargins &margins);
} // namespace neverd::gui::platform

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_WINDOWFRAMEPLATFORM_H
