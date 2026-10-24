//===- FocusSidebar.cpp - Collapsible focus navigation ------------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A small triangle toggles navigation without resizing the analysis panes.
//===----------------------------------------------------------------------===//

#include "FocusSidebar.h"

#include "../Config.h"
#include "Theme.h"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace config = ::neverd::limits::gui_config;

FocusSidebar::FocusSidebar(QWidget &host, QWidget &area, QWidget &content,
                           const QString &title)
    : QWidget(&host), host_(host), area_(&area),
      toggle_(new QToolButton(&host)) {
  setObjectName(QStringLiteral("focusFunctionsSidebar"));
  setAccessibleName(title);
  setFocusPolicy(Qt::NoFocus);
  auto *layout = new QVBoxLayout(this);
  const int border = config::kFocusSidebarBorderWidth;
  // Keep the triangle in the same place when open, clear of the function rows.
  layout->setContentsMargins(config::kFocusSidebarToggleWidth + border, border,
                             border, border);
  layout->setSpacing(0);
  auto *heading = new QLabel(title, this);
  heading->setMargin(config::kFocusSidebarHeadingMargin);
  layout->addWidget(heading);
  layout->addWidget(&content, 1);
  toggle_->setObjectName(QStringLiteral("focusSidebarToggle"));
  toggle_->setAccessibleName(title);
  toggle_->setToolTip(title);
  toggle_->setFocusPolicy(Qt::TabFocus);
  toggle_->setCheckable(true);
  toggle_->setArrowType(Qt::RightArrow);
  connect(toggle_, &QToolButton::toggled, this, [this](bool open) {
    if (open)
      reveal();
    else
      dismiss();
  });
  connect(&Theme::instance(), &Theme::changed, this,
          qOverload<>(&QWidget::update));
  host_.installEventFilter(this);
  area.installEventFilter(this);
  hide();
  place();
  toggle_->show();
  toggle_->raise();
}

FocusSidebar::~FocusSidebar() {
  host_.removeEventFilter(this);
  if (area_)
    area_->removeEventFilter(this);
  delete toggle_.data();
}

void FocusSidebar::place() {
  if (!area_ || !toggle_)
    return;
  const QRect available(area_->mapTo(&host_, QPoint()), area_->size());
  const bool expanded = host_.isMaximized() || host_.isFullScreen();
  // Leave the restored window's resize grip clear of the navigation button.
  const int left = expanded ? 0
                            : std::max(available.left(),
                                       host_.contentsMargins().left() +
                                           config::kWindowFrameResizeInset);
  setGeometry(left, available.top(),
              std::min(config::kFocusSidebarPreferredWidth,
                       available.right() - left + 1),
              available.height());
  toggle_->setGeometry(
      left, available.center().y() - config::kFocusSidebarToggleHeight / 2,
      config::kFocusSidebarToggleWidth, config::kFocusSidebarToggleHeight);
  if (isVisible())
    raise();
  toggle_->raise();
}

void FocusSidebar::reveal() {
  if (!host_.isVisible() || !area_)
    return;
  if (!isVisible())
    previousFocus_ = QApplication::focusWidget();
  place();
  show();
  raise();
  const QSignalBlocker blocker(toggle_);
  toggle_->setChecked(true);
  toggle_->setArrowType(Qt::LeftArrow);
  toggle_->raise();
}

void FocusSidebar::dismiss() {
  auto *focus = QApplication::focusWidget();
  const bool restore = focus && isAncestorOf(focus) && host_.isActiveWindow();
  hide();
  if (toggle_) {
    const QSignalBlocker blocker(toggle_);
    toggle_->setChecked(false);
    toggle_->setArrowType(Qt::RightArrow);
    toggle_->raise();
  }
  if (restore && previousFocus_ && previousFocus_->isVisible())
    previousFocus_->setFocus();
  previousFocus_.clear();
}

bool FocusSidebar::eventFilter(QObject *object, QEvent *event) {
  if (object == &host_ || object == area_) {
    switch (event->type()) {
    case QEvent::Resize:
    case QEvent::Move:
    case QEvent::LayoutRequest:
    case QEvent::WindowStateChange:
      place();
      break;
    default:
      break;
    }
  }
  return QWidget::eventFilter(object, event);
}

void FocusSidebar::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.fillRect(rect(), Theme::instance().chrome(QStringLiteral("Window")));
  painter.setPen(Theme::instance().chrome(QStringLiteral("Border")));
  painter.drawRect(rect().adjusted(0, 0, -config::kFocusSidebarBorderWidth,
                                   -config::kFocusSidebarBorderWidth));
}

} // namespace neverd::gui
