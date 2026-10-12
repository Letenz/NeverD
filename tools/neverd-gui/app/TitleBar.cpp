#include "TitleBar.h"

#include "Theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace neverd::gui {
namespace {
// Visual Studio Code's custom title bar, in logical pixels.
constexpr int BarHeight = 35;
constexpr int IconSlot = 35;
constexpr int IconSize = 16;
constexpr int ButtonWidth = 46;
/// The window controls draw their glyphs in a square of this side.
constexpr int GlyphSize = 10;
constexpr int CenterHeight = 22;
constexpr int CenterMaxWidth = 600;
/// The command center takes this share of the bar's width.
constexpr int CenterWidthPercent = 38;
/// Narrower than this, the command center gives way to the menus.
constexpr int CenterMinWidth = 120;
constexpr int CenterRadius = 6;
constexpr int CenterPadding = 8;
/// The magnifier's square and the space after it.
constexpr int LensSize = 12;
constexpr int LensGap = 6;
/// The room kept clear between the menus, the command center and the
/// buttons.
constexpr int Gap = 8;
/// An inactive window's icon fades as its text dims.
constexpr double InactiveOpacity = 0.6;
/// The style sheet dims the menus of an inactive window by this property.
constexpr char ActiveProperty[] = "active";
/// A dialog's title keeps this far from the bar's edge and the buttons.
constexpr int TitlePadding = 12;

/// The window controls a window of \p flags offers.  Without
/// CustomizeWindowHint a dialog offers Close and other windows all three, as
/// window managers do.
struct Controls {
  bool minimize;
  bool maximize;
  bool close;
};

Controls controlsFor(Qt::WindowFlags flags) {
  if (!(flags & Qt::CustomizeWindowHint)) {
    const Qt::WindowFlags type = flags & Qt::WindowType_Mask;
    const bool window = type != Qt::Dialog && type != Qt::Tool;
    return {window, window, true};
  }
  return {bool(flags & Qt::WindowMinimizeButtonHint),
          bool(flags & Qt::WindowMaximizeButtonHint),
          bool(flags & Qt::WindowCloseButtonHint)};
}

QColor chrome(const char *name) {
  return Theme::instance().chrome(QLatin1String(name));
}

QColor titleText(const QWidget &window) {
  return chrome(window.isActiveWindow() ? "TitleBarText"
                                        : "TitleBarInactiveText");
}

/// A button that repaints as the pointer enters and leaves it.
class HoverButton : public QAbstractButton {
public:
  HoverButton(QWidget &window, QWidget *parent)
      : QAbstractButton(parent), window_(window) {
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
  }

protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::HoverEnter ||
        event->type() == QEvent::HoverLeave)
      update();
    return QAbstractButton::event(event);
  }
  bool lit() const { return underMouse() || isDown(); }

  QWidget &window_;
};

/// A window control: a light wash under the pointer, red behind Close.
class WindowButton final : public HoverButton {
public:
  enum class Kind { Minimize, Maximize, Close };
  WindowButton(Kind kind, QWidget &window, QWidget *parent)
      : HoverButton(window, parent), kind_(kind) {}
  QSize sizeHint() const override { return {ButtonWidth, BarHeight}; }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    const bool close = kind_ == Kind::Close;
    if (isDown())
      painter.fillRect(rect(), chrome(close ? "WindowClosePressed"
                                            : "WindowControlPressed"));
    else if (underMouse())
      painter.fillRect(
          rect(), chrome(close ? "WindowCloseHover" : "WindowControlHover"));
    const QColor glyph =
        close && lit() ? chrome("WindowCloseText") : titleText(window_);
    painter.setPen(QPen(glyph, 1));
    // Whole pixels, so the glyph's one-pixel strokes stay sharp.
    const double left = std::round((width() - GlyphSize) / 2.0);
    const double top = std::round((height() - GlyphSize) / 2.0);
    switch (kind_) {
    case Kind::Minimize:
      painter.drawLine(QPointF(left, top + 5.5),
                       QPointF(left + GlyphSize, top + 5.5));
      break;
    case Kind::Maximize:
      if (window_.isMaximized()) {
        // Restore: the window, and the one behind it.
        painter.drawRect(QRectF(left + 0.5, top + 2.5, 7, 7));
        const QPointF back[] = {{left + 2.5, top + 2.5},
                                {left + 2.5, top + 0.5},
                                {left + 9.5, top + 0.5},
                                {left + 9.5, top + 7.5},
                                {left + 7.5, top + 7.5}};
        painter.drawPolyline(back, std::size(back));
      } else {
        painter.drawRect(QRectF(left + 0.5, top + 0.5, 9, 9));
      }
      break;
    case Kind::Close:
      painter.setRenderHint(QPainter::Antialiasing);
      painter.drawLine(QPointF(left, top),
                       QPointF(left + GlyphSize, top + GlyphSize));
      painter.drawLine(QPointF(left + GlyphSize, top),
                       QPointF(left, top + GlyphSize));
      break;
    }
  }

private:
  const Kind kind_;
};

/// The command center: a quiet field with a magnifier and the open file's
/// name, which runs a command when clicked.
class CommandCenter final : public HoverButton {
public:
  using HoverButton::HoverButton;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(
        chrome(lit() ? "CommandCenterHoverBorder" : "CommandCenterBorder"), 1));
    painter.setBrush(chrome(lit() ? "CommandCenterHover" : "CommandCenter"));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            CenterRadius, CenterRadius);
    // The magnifier and the name, centered together; a long name keeps both
    // of its ends.
    const QFontMetrics metrics(font());
    const QString name =
        metrics.elidedText(text(), Qt::ElideMiddle,
                           width() - 2 * CenterPadding - LensSize - LensGap);
    const int content = LensSize + LensGap + metrics.horizontalAdvance(name);
    const double left = std::round((width() - content) / 2.0);
    const double middle = height() / 2.0;
    const QColor ink = titleText(window_);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 1.2));
    const QRectF lens(left + 0.5, middle - 5.5, 8, 8);
    painter.drawEllipse(lens);
    painter.drawLine(QPointF(lens.right() - 1.2, lens.bottom() - 1.2),
                     QPointF(left + LensSize - 0.5, middle + 5.5));
    painter.setPen(ink);
    painter.drawText(QRectF(left + LensSize + LensGap, 0, width(), height()),
                     Qt::AlignLeft | Qt::AlignVCenter, name);
  }
};
} // namespace

TitleBar::TitleBar(QWidget &window, QMenuBar *menuBar)
    : QWidget(&window), window_(window), menuBar_(menuBar),
      minimize_(new WindowButton(WindowButton::Kind::Minimize, window, this)),
      maximize_(new WindowButton(WindowButton::Kind::Maximize, window, this)),
      close_(new WindowButton(WindowButton::Kind::Close, window, this)) {
  setObjectName(QStringLiteral("titleBar"));
  if (menuBar) {
    menuBar->setParent(this);
    commandCenter_ = new CommandCenter(window, this);
    commandCenter_->setObjectName(QStringLiteral("commandCenter"));
    connect(commandCenter_, &QAbstractButton::clicked, this, [this] {
      if (command_)
        command_->trigger();
    });
  }
  minimize_->setObjectName(QStringLiteral("minimizeButton"));
  maximize_->setObjectName(QStringLiteral("maximizeButton"));
  close_->setObjectName(QStringLiteral("closeButton"));
  connect(minimize_, &QAbstractButton::clicked, &window,
          &QWidget::showMinimized);
  connect(maximize_, &QAbstractButton::clicked, this,
          &TitleBar::toggleMaximized);
  connect(close_, &QAbstractButton::clicked, &window, &QWidget::close);
  connect(&Theme::instance(), &Theme::changed, this, &TitleBar::repaintAll);
  window.installEventFilter(this);
  retranslateUi();
  updateActive();
  updateWindowState();
}

void TitleBar::setCommandCenterText(const QString &text,
                                    const QString &toolTip) {
  if (!commandCenter_)
    return;
  commandCenter_->setText(text);
  commandCenter_->setToolTip(toolTip);
  commandCenter_->setAccessibleName(text);
  commandCenter_->update();
}

void TitleBar::setCommandCenterAction(QAction *action) { command_ = action; }

QSize TitleBar::sizeHint() const {
  const int buttons = 3 * ButtonWidth;
  if (!menuBar_)
    return {2 * TitlePadding +
                fontMetrics().horizontalAdvance(window_.windowTitle()) +
                buttons,
            BarHeight};
  return {IconSlot + menuBar_->sizeHint().width() + 2 * Gap + CenterMaxWidth +
              buttons,
          BarHeight};
}

QSize TitleBar::minimumSizeHint() const {
  return {(menuBar_ ? IconSlot : TitlePadding) + 3 * ButtonWidth, BarHeight};
}

bool TitleBar::event(QEvent *event) {
  // The menus' width changes with their language and the window's menus.
  if (event->type() == QEvent::LayoutRequest)
    arrange();
  return QWidget::event(event);
}

bool TitleBar::eventFilter(QObject *object, QEvent *event) {
  if (object == &window_) {
    if (event->type() == QEvent::ActivationChange)
      updateActive();
    else if (event->type() == QEvent::WindowStateChange)
      updateWindowState();
    else if (event->type() == QEvent::WindowTitleChange)
      update();
  }
  return QWidget::eventFilter(object, event);
}

void TitleBar::changeEvent(QEvent *event) {
  if (event->type() == QEvent::LanguageChange)
    retranslateUi();
  QWidget::changeEvent(event);
}

void TitleBar::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton)
    pressAt_ = event->position().toPoint();
  QWidget::mousePressEvent(event);
}

void TitleBar::mouseMoveEvent(QMouseEvent *event) {
  // The window moves once the pointer leaves the press's click distance, so
  // a double click still reaches the bar.
  if (pressAt_ && event->buttons() & Qt::LeftButton &&
      (event->position().toPoint() - *pressAt_).manhattanLength() >=
          QApplication::startDragDistance()) {
    pressAt_.reset();
    if (QWindow *handle = window_.windowHandle())
      handle->startSystemMove();
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void TitleBar::mouseReleaseEvent(QMouseEvent *event) {
  pressAt_.reset();
  QWidget::mouseReleaseEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton || !maximizable() ||
      window_.isFullScreen()) {
    QWidget::mouseDoubleClickEvent(event);
    return;
  }
  pressAt_.reset();
  toggleMaximized();
}

void TitleBar::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.fillRect(rect(), chrome("TitleBar"));
  if (!menuBar_) {
    // A dialog's title, centered in the bar while it fits beside the
    // buttons.
    const int room = buttonsLeft_ - 2 * TitlePadding;
    const QString title =
        fontMetrics().elidedText(window_.windowTitle(), Qt::ElideRight, room);
    const int titleWidth = fontMetrics().horizontalAdvance(title);
    const int x = std::clamp(
        (width() - titleWidth) / 2, TitlePadding,
        std::max(TitlePadding, buttonsLeft_ - TitlePadding - titleWidth));
    painter.setPen(titleText(window_));
    painter.drawText(QRect(x, 0, titleWidth, height()),
                     Qt::AlignLeft | Qt::AlignVCenter, title);
    return;
  }
  if (window_.isFullScreen())
    return;
  if (!window_.isActiveWindow())
    painter.setOpacity(InactiveOpacity);
  window_.windowIcon().paint(&painter, QRect((IconSlot - IconSize) / 2,
                                             (height() - IconSize) / 2,
                                             IconSize, IconSize));
}

void TitleBar::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  arrange();
}

void TitleBar::arrange() {
  int right = width();
  for (QAbstractButton *button : {close_, maximize_, minimize_}) {
    if (button->isHidden())
      continue;
    right -= ButtonWidth;
    button->setGeometry(right, 0, ButtonWidth, height());
  }
  buttonsLeft_ = right;
  if (!menuBar_)
    return;
  const int left = window_.isFullScreen() ? 0 : IconSlot;
  const QSize menus = menuBar_->sizeHint();
  const int menuWidth =
      std::clamp(menus.width(), 0, std::max(0, right - left - Gap));
  menuBar_->setGeometry(left, (height() - menus.height()) / 2, menuWidth,
                        menus.height());
  const int start = left + menuWidth + Gap;
  const int end = right - Gap;
  const int centerWidth = std::min(
      {CenterMaxWidth, width() * CenterWidthPercent / 100, end - start});
  if (centerWidth < CenterMinWidth) {
    commandCenter_->hide();
    return;
  }
  const int x =
      std::clamp((width() - centerWidth) / 2, start, end - centerWidth);
  commandCenter_->setGeometry(x, (height() - CenterHeight) / 2, centerWidth,
                              CenterHeight);
  commandCenter_->show();
}

bool TitleBar::maximizable() const {
  return controlsFor(window_.windowFlags()).maximize &&
         window_.minimumSize() != window_.maximumSize();
}

void TitleBar::toggleMaximized() {
  if (window_.isMaximized())
    window_.showNormal();
  else
    window_.showMaximized();
}

void TitleBar::updateWindowState() {
  const bool fullScreen = window_.isFullScreen();
  const Controls controls = controlsFor(window_.windowFlags());
  minimize_->setVisible(controls.minimize && !fullScreen);
  maximize_->setVisible(maximizable() && !fullScreen);
  close_->setVisible(controls.close && !fullScreen);
  retranslateUi();
  arrange();
  repaintAll();
}

void TitleBar::updateActive() {
  setProperty(ActiveProperty, window_.isActiveWindow());
  // The menus' color hangs on this property in the style sheet.
  if (menuBar_) {
    menuBar_->style()->unpolish(menuBar_);
    menuBar_->style()->polish(menuBar_);
  }
  repaintAll();
}

void TitleBar::retranslateUi() {
  const auto describe = [](QAbstractButton *button, const QString &text) {
    button->setToolTip(text);
    button->setAccessibleName(text);
  };
  describe(minimize_, tr("Minimize"));
  describe(maximize_, window_.isMaximized() ? tr("Restore") : tr("Maximize"));
  describe(close_, tr("Close"));
}

void TitleBar::repaintAll() {
  update();
  for (QWidget *child : findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
    child->update();
}

} // namespace neverd::gui
