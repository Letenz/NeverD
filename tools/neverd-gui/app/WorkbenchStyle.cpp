#include "WorkbenchStyle.h"

#include "Icons.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStyleFactory>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTimer>

namespace neverd::gui {
namespace {
/// The object name of Qt's file dialog places pane.
constexpr char FileDialogPlaces[] = "sidebar";
/// The size of icons drawn on the 16 pixel grid of IconSet.def.
constexpr int SmallIconSize = 16;

bool drawsMnemonics(const QWidget *widget) {
  return qobject_cast<const QMenuBar *>(widget) ||
         qobject_cast<const QMenu *>(widget) ||
         qobject_cast<const QAbstractButton *>(widget) ||
         qobject_cast<const QLabel *>(widget) ||
         qobject_cast<const QGroupBox *>(widget) ||
         qobject_cast<const QTabBar *>(widget);
}
} // namespace

WorkbenchStyle::WorkbenchStyle()
    : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

int WorkbenchStyle::styleHint(StyleHint hint, const QStyleOption *option,
                              const QWidget *widget,
                              QStyleHintReturn *returnData) const {
  switch (hint) {
  case SH_UnderlineShortcut:
    return cues_;
  // A combo box's list highlights the item under the pointer, as a menu does.
  case SH_ComboBox_ListMouseTracking:
    return true;
  default:
    return QProxyStyle::styleHint(hint, option, widget, returnData);
  }
}

QIcon WorkbenchStyle::standardIcon(StandardPixmap pixmap,
                                   const QStyleOption *option,
                                   const QWidget *widget) const {
  switch (pixmap) {
  // A file dialog's tool buttons and the places it lists.
  case SP_ArrowBack:
  case SP_FileDialogBack:
    return icon(QStringLiteral("back"));
  case SP_ArrowForward:
    return icon(QStringLiteral("forward"));
  case SP_FileDialogToParent:
    return icon(QStringLiteral("up"));
  case SP_FileDialogNewFolder:
    return icon(QStringLiteral("folder_new"));
  case SP_FileDialogListView:
    return icon(QStringLiteral("view_list"));
  case SP_FileDialogDetailedView:
    return icon(QStringLiteral("view_details"));
  case SP_DirIcon:
  case SP_DirClosedIcon:
    return icon(QStringLiteral("folder"));
  case SP_DirOpenIcon:
    return icon(QStringLiteral("open"));
  case SP_FileIcon:
    return icon(QStringLiteral("file"));
  case SP_ComputerIcon:
    return icon(QStringLiteral("computer"));
  case SP_DriveHDIcon:
    return icon(QStringLiteral("drive"));
  case SP_MessageBoxInformation:
    return icon(QStringLiteral("message_information"));
  case SP_MessageBoxWarning:
    return icon(QStringLiteral("message_warning"));
  case SP_MessageBoxCritical:
    return icon(QStringLiteral("message_critical"));
  case SP_MessageBoxQuestion:
    return icon(QStringLiteral("help"));
  default:
    return QProxyStyle::standardIcon(pixmap, option, widget);
  }
}

void WorkbenchStyle::polish(QWidget *widget) {
  QProxyStyle::polish(widget);
  // The combo box's own delegates draw items themselves; a styled delegate
  // draws them as the style sheet's item rules say, like a menu's.
  if (auto *combo = qobject_cast<QComboBox *>(widget);
      combo && !qobject_cast<QStyledItemDelegate *>(combo->itemDelegate()))
    combo->setItemDelegate(new QStyledItemDelegate(combo));
  // Qt's own file dialog lists folders and files with the icon theme's
  // icons unless it is given others.
  if (auto *files = qobject_cast<QFileDialog *>(widget);
      files && files->iconProvider() != fileIcons())
    files->setIconProvider(fileIcons());
  // Its places pane draws them at 24 pixels, where icons drawn on the 16
  // pixel grid blur; at 16 they are as sharp as in the file list.
  if (auto *places = qobject_cast<QAbstractItemView *>(widget);
      places && places->objectName() == QLatin1String(FileDialogPlaces) &&
      qobject_cast<QFileDialog *>(places->window()))
    places->setIconSize(QSize(SmallIconSize, SmallIconSize));
}

void WorkbenchStyle::polish(QApplication *application) {
  QProxyStyle::polish(application);
  application->installEventFilter(this);
}

void WorkbenchStyle::unpolish(QApplication *application) {
  application->removeEventFilter(this);
  QProxyStyle::unpolish(application);
}

bool WorkbenchStyle::eventFilter(QObject *watched, QEvent *event) {
  switch (event->type()) {
  case QEvent::KeyPress:
    if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Alt) {
      heldByMenu_ = false;
      setKeyboardCues(true);
      break;
    }
    [[fallthrough]];
  case QEvent::MouseButtonPress:
  case QEvent::MouseMove:
    // The release of an Alt that the window manager took never arrives.
    if (cues_ && !heldByMenu_ &&
        !(static_cast<QInputEvent *>(event)->modifiers() & Qt::AltModifier))
      setKeyboardCues(false);
    break;
  case QEvent::KeyRelease:
    if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Alt) {
      heldByMenu_ = QApplication::activePopupWidget() != nullptr;
      if (!heldByMenu_)
        setKeyboardCues(false);
    }
    break;
  case QEvent::Hide:
    // The popup list changes after the menu hides.
    if (heldByMenu_ && qobject_cast<QMenu *>(watched))
      QTimer::singleShot(0, this, &WorkbenchStyle::releaseAfterMenu);
    break;
  case QEvent::ApplicationStateChange:
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
      heldByMenu_ = false;
      setKeyboardCues(false);
    }
    break;
  default:
    break;
  }
  return QProxyStyle::eventFilter(watched, event);
}

void WorkbenchStyle::releaseAfterMenu() {
  if (!heldByMenu_ || QApplication::activePopupWidget())
    return;
  heldByMenu_ = false;
  setKeyboardCues(false);
}

void WorkbenchStyle::setKeyboardCues(bool on) {
  if (cues_ == on)
    return;
  cues_ = on;
  for (QWidget *widget : QApplication::allWidgets())
    if (widget->isVisible() && drawsMnemonics(widget))
      widget->update();
}

} // namespace neverd::gui
