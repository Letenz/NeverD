#pragma once

class QApplication;
class QObject;

namespace neverd::gui {

/// Return and Enter on a check box or radio button of a dialog press the
/// dialog's default button, as in IDA and on Windows, macOS and KDE; Space
/// still toggles the focused button.  Qt's GNOME and other Linux platform
/// themes make Return press the focused button instead, so after a mnemonic
/// checked a box (Alt+O for "Does not return"), Return cleared it again and
/// left the dialog open.  Returns the filter, which \p app owns.
QObject *acceptDialogsOnReturn(QApplication &app);

} // namespace neverd::gui
