#pragma once

class QString;

namespace neverd::gui {

class MainWindow;
class Session;

/// Drive the production window through real keyboard shortcuts, dialogs,
/// docking and views, then exit with 0 or a diagnostic and 1.  With
/// \p binary empty a fixture file is generated for the fixture worker;
/// otherwise \p binary (with a function named main) is analyzed by the real
/// engine.  NEVERD_PROBE_CAPTURE_DIR saves a screenshot per stage.
void startWidgetsProbe(MainWindow &window, Session &session,
                       const QString &binary);

} // namespace neverd::gui
