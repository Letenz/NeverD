#include "Docking.h"

#include "../Config.h"
#include "Theme.h"

#include <QCursor>
#include <QEnterEvent>
#include <QHoverEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QTimer>
#include <QVariantAnimation>
#include <cmath>
#include <kddockwidgets/Config.h>
#include <kddockwidgets/KDDockWidgets.h>
#include <kddockwidgets/core/MainWindow.h>
#include <kddockwidgets/core/Separator.h>
#include <kddockwidgets/qtcommon/View.h>
#include <kddockwidgets/qtwidgets/ViewFactory.h>
#include <kddockwidgets/qtwidgets/views/FloatingWindow.h>
#include <kddockwidgets/qtwidgets/views/Group.h>
#include <kddockwidgets/qtwidgets/views/Separator.h>
#include <kddockwidgets/qtwidgets/views/TabBar.h>

namespace neverd::gui {
namespace {
// A pointer that only crosses a separator does not light it up.
constexpr int SeparatorHoverDelayMs = 300;
constexpr int SeparatorFadeInMs = 150;
constexpr int SeparatorFadeOutMs = 100;
// Docked windows reach the window's sides and bottom.  The gap above the
// first row of tabs keeps inactive tabs apart from the toolbar of their color.
constexpr QMargins DockAreaMargins(0, 5, 0, 0);

/// A docked window draws no frame: neighbors meet at a separator's hairline.
class DockGroup final : public KDDockWidgets::QtWidgets::Group {
public:
  using Group::Group;

protected:
  void paintEvent(QPaintEvent *) override {}
};

/// The hairline between docked windows.  It lights up in the focus color
/// while it is hovered or dragged.
class DockSeparator final : public KDDockWidgets::QtWidgets::Separator {
public:
  DockSeparator(KDDockWidgets::Core::Separator *controller,
                KDDockWidgets::Core::View *parent)
      : Separator(controller, parent) {
    hoverDelay_.setSingleShot(true);
    hoverDelay_.setInterval(SeparatorHoverDelayMs);
    connect(&hoverDelay_, &QTimer::timeout, this,
            [this] { setHighlighted(true); });
    fade_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&fade_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
              highlight_ = value.toReal();
              update();
            });
    connect(&Theme::instance(), &Theme::changed, this, [this] { update(); });
  }

protected:
  void paintEvent(QPaintEvent *) override {
    const auto &theme = Theme::instance();
    const auto &config = KDDockWidgets::Config::self();
    // The layout's gap, on which the grab area is centered.
    const int line = config.layoutSpacing();
    const int inset = (config.separatorThickness() - line) / 2;
    QPainter painter(this);
    painter.fillRect(horizontal() ? QRect(0, inset, width(), line)
                                  : QRect(inset, 0, line, height()),
                     theme.chrome(QStringLiteral("DockSeparator")));
    if (highlight_ <= 0)
      return;
    QColor accent = theme.chrome(QStringLiteral("DockSeparatorHover"));
    accent.setAlphaF(accent.alphaF() * highlight_);
    const qreal start =
        inset +
        (line - ::neverd::limits::gui_config::kDockingSeparatorHighlightWidth) /
            2;
    const QRectF bar =
        horizontal()
            ? QRectF(
                  0, start, width(),
                  ::neverd::limits::gui_config::kDockingSeparatorHighlightWidth)
            : QRectF(
                  start, 0,
                  ::neverd::limits::gui_config::kDockingSeparatorHighlightWidth,
                  height());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawRoundedRect(
        bar, ::neverd::limits::gui_config::kDockingSeparatorHighlightWidth / 2,
        ::neverd::limits::gui_config::kDockingSeparatorHighlightWidth / 2);
  }

  void enterEvent(QEnterEvent *event) override {
    Separator::enterEvent(event);
    // The cursors of Qt's own splitters and of GTK's panes.
    setCursor(horizontal() ? Qt::SplitVCursor : Qt::SplitHCursor);
    if (pressed_)
      setHighlighted(true);
    else
      hoverDelay_.start();
  }

  void leaveEvent(QEvent *event) override {
    Separator::leaveEvent(event);
    hoverDelay_.stop();
    if (!pressed_)
      setHighlighted(false);
  }

  void mousePressEvent(QMouseEvent *event) override {
    Separator::mousePressEvent(event);
    if (event->button() != Qt::LeftButton)
      return;
    pressed_ = true;
    hoverDelay_.stop();
    setHighlighted(true);
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    Separator::mouseMoveEvent(event);
    // The layout ends a drag whose release it never saw.
    if (pressed_ && !KDDockWidgets::Core::Separator::isResizing())
      release(event->position().toPoint());
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    Separator::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton)
      release(event->position().toPoint());
  }

private:
  bool horizontal() const { return width() > height(); }

  void release(QPoint position) {
    pressed_ = false;
    if (!rect().contains(position))
      setHighlighted(false);
  }

  void setHighlighted(bool on) {
    const qreal target = on ? 1 : 0;
    if (target == target_)
      return;
    target_ = target;
    fade_.stop();
    fade_.setStartValue(highlight_);
    fade_.setEndValue(target);
    // A fade that reverses midway takes only as long as the way back.
    const int duration = on ? SeparatorFadeInMs : SeparatorFadeOutMs;
    fade_.setDuration(qRound(duration * std::abs(target - highlight_)));
    fade_.start();
  }

  QTimer hoverDelay_;
  QVariantAnimation fade_;
  qreal highlight_ = 0;
  qreal target_ = 0;
  bool pressed_ = false;
};

/// A row of docked tabs.  Only the current tab and the one under the pointer
/// show their close buttons, as in Visual Studio Code: a cross on every tab
/// competes with the titles.  A hidden button keeps its room, so the tabs stay
/// put when it appears.
class DockTabBar final : public KDDockWidgets::QtWidgets::TabBar {
public:
  DockTabBar(KDDockWidgets::Core::TabBar *controller, QWidget *parent)
      : TabBar(controller, parent) {
    connect(this, &QTabBar::currentChanged, this,
            [this] { updateCloseButtons(); });
  }

protected:
  bool event(QEvent *event) override {
    const bool handled = TabBar::event(event);
    switch (event->type()) {
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
      setHoveredTab(
          tabAt(static_cast<QHoverEvent *>(event)->position().toPoint()));
      break;
    case QEvent::HoverLeave:
      setHoveredTab(-1);
      break;
    case QEvent::Show:
      updateCloseButtons();
      break;
    default:
      break;
    }
    return handled;
  }

  void tabInserted(int index) override {
    TabBar::tabInserted(index);
    updateCloseButtons();
  }

  void tabRemoved(int index) override {
    TabBar::tabRemoved(index);
    // The tab that slid under the pointer is the hovered one now.
    hovered_ = tabAt(mapFromGlobal(QCursor::pos()));
    updateCloseButtons();
  }

private:
  void setHoveredTab(int index) {
    if (index == hovered_)
      return;
    hovered_ = index;
    updateCloseButtons();
  }

  void updateCloseButtons() {
    const auto side = static_cast<QTabBar::ButtonPosition>(style()->styleHint(
        QStyle::SH_TabBar_CloseButtonPosition, nullptr, this));
    for (int i = 0; i < count(); ++i)
      if (QWidget *button = tabButton(i, side))
        button->setVisible(i == currentIndex() || i == hovered_);
  }

  int hovered_ = -1;
};

/// A floating window has no native frame.  Its outline is the border of the
/// workbench's other popups.
class DockFloatingWindow final
    : public KDDockWidgets::QtWidgets::FloatingWindow {
public:
  using FloatingWindow::FloatingWindow;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setPen(
        Theme::instance().chrome(QStringLiteral("FloatingWindowBorder")));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
  }
};

class DockViewFactory final : public KDDockWidgets::QtWidgets::ViewFactory {
public:
  KDDockWidgets::Core::View *
  createGroup(KDDockWidgets::Core::Group *controller,
              KDDockWidgets::Core::View *parent) const override {
    return new DockGroup(controller,
                         KDDockWidgets::QtCommon::View_qt::asQWidget(parent));
  }

  KDDockWidgets::Core::View *
  createSeparator(KDDockWidgets::Core::Separator *controller,
                  KDDockWidgets::Core::View *parent) const override {
    return new DockSeparator(controller, parent);
  }

  KDDockWidgets::Core::View *
  createTabBar(KDDockWidgets::Core::TabBar *controller,
               KDDockWidgets::Core::View *parent) const override {
    return new DockTabBar(controller,
                          KDDockWidgets::QtCommon::View_qt::asQWidget(parent));
  }

  KDDockWidgets::Core::View *
  createFloatingWindow(KDDockWidgets::Core::FloatingWindow *controller,
                       KDDockWidgets::Core::MainWindow *parent,
                       Qt::WindowFlags flags) const override {
    auto *window =
        qobject_cast<QMainWindow *>(KDDockWidgets::QtCommon::View_qt::asQWidget(
            parent ? parent->view() : nullptr));
    return new DockFloatingWindow(controller, window, flags);
  }
};
} // namespace

void configureDocking() {
  KDDockWidgets::initFrontend(KDDockWidgets::FrontendType::QtWidgets);
  auto &config = KDDockWidgets::Config::self();
  // Every docked window carries a closable tab, as in classic disassemblers.
  config.setFlags(KDDockWidgets::Config::Flag_AlwaysShowTabs |
                  KDDockWidgets::Config::Flag_HideTitleBarWhenTabsVisible |
                  KDDockWidgets::Config::Flag_TabsHaveCloseButton |
                  KDDockWidgets::Config::Flag_AllowReorderTabs |
                  KDDockWidgets::Config::Flag_TitleBarIsFocusable |
                  KDDockWidgets::Config::Flag_DoubleClickMaximizes);
  // Setting the thickness resets the spacing too, so it comes first.
  config.setSeparatorThickness(
      ::neverd::limits::gui_config::kDockingSeparatorGrabWidth);
  config.setLayoutSpacing(
      ::neverd::limits::gui_config::kDockingSeparatorLineWidth);
  config.setViewFactory(new DockViewFactory);
}

QMargins dockAreaMargins() { return DockAreaMargins; }

} // namespace neverd::gui
