#pragma once

#include <QMargins>

namespace neverd::gui {

/// Selects the Qt Widgets docking front end and sets the workbench's docking
/// behavior and chrome.  Call once, after the application object exists and
/// before any dock or main window is created.
void configureDocking();

/// The margins around a main window's dock area.
QMargins dockAreaMargins();

} // namespace neverd::gui
