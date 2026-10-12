#pragma once

#include <QMargins>
#include <QtGlobal>

class QWindow;

/// What each platform does for a window that draws its own title bar
/// (WindowFrame, TitleBar).  One file under app/platform implements it per
/// platform, after Visual Studio Code's custom title bars there.
namespace neverd::gui::platform {

/// How a window that draws its own title bar is framed.
enum class FrameKind {
  /// The window manager's frame stays: unknown and offscreen platforms.
  System,
  /// No system frame at all; the window draws and resizes its edges itself
  /// (Linux under Wayland and X11).
  Frameless,
  /// The system frame without its title bar: the system still draws the
  /// edges and the shadow, and snaps and resizes the window (Windows, macOS).
  Captionless,
};
FrameKind frameKind();

/// Whether a frameless window draws its own shadow, as GNOME draws none for
/// one under Wayland.
bool drawsShadow();
/// Tell the compositor that \p margins of \p window are its drawn shadow.
void setShadowMargins(QWindow &window, const QMargins &margins);

/// Remove the system title bar of a captionless \p window while keeping the
/// rest of its frame.  Run again as the window changes screens.
void removeCaption(QWindow &window);
/// What a captionless \p window keeps clear above its title bar while it is
/// maximized, where the system frame reaches past the screen's edges.
QMargins maximizedInsets(QWindow &window);

/// The edges a window resizes from itself; the system frame resizes it from
/// the others.  A corner resizes when one of its edges is listed.
Qt::Edges ownResizeEdges();

/// The title bar's height, in logical pixels.
int titleBarHeight();
/// The width at the title bar's left that the system's own window controls
/// take, as macOS's traffic lights do.
int systemControlsWidth();
/// Whether the title bar draws minimize, maximize and close.
bool drawsWindowControls();
/// Whether the title bar shows the application icon.
bool showsWindowIcon();
/// Run the system's action for a double click on \p window's title bar.
/// Returns false where the title bar maximizes or restores the window itself.
bool doubleClickTitleBar(QWindow &window);

} // namespace neverd::gui::platform
