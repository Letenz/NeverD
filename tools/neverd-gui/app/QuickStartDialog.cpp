#include "QuickStartDialog.h"

#include "Icons.h"
#include "ProjectDatabase.h"
#include "SettingsKeys.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace neverd::gui {
namespace {

constexpr char DarkLogo[] = ":/neverd/icons/app.svg";
constexpr char LightLogo[] = ":/neverd/brand/logo-light.svg";
constexpr QSize DialogSize(900, 560);
constexpr QSize MinimumDialogSize(720, 460);
/// The side pane: the product, above its ways to start.
constexpr int SideWidth = 300;
constexpr int SideTop = 32;
constexpr int LogoSize = 56;
constexpr int ActionHeight = 56;
constexpr int ActionGlyphSlot = 36;
constexpr int GlyphSize = 24;
constexpr qreal RowRadius = 5;
constexpr qreal DisabledOpacity = 0.45;
/// The recent files.
constexpr int RecentRowHeight = 60;
constexpr int BadgeSize = 36;
constexpr int EmptyGlyphSize = 44;
constexpr int EmptyTextWidth = 300;
constexpr int DropGlyphSize = 40;
constexpr int DropInset = 14;
/// Pixel sizes of the dialog's type, from the largest down.
constexpr int BrandPixels = 24;
constexpr int EmptyPixels = 15;
constexpr int TitlePixels = 14;
constexpr int NamePixels = 14;
constexpr int BodyPixels = 12;
constexpr int KeyPixels = 11;
constexpr int BadgePixels = 10;
/// How many bytes of a recent file its format is read from.
constexpr qint64 MagicBytes = 8;

enum RecentRole {
  PathRole = Qt::UserRole,
  LabelRole,
  BadgeRole,
  WhenRole,
  MissingRole,
};

struct RecentFormat {
  QByteArray Magic;
  QString Label;
  QString Badge;
};

const std::vector<RecentFormat> &recentFormats() {
  static const std::vector<RecentFormat> Formats = {
#define NEVERD_RECENT_FORMAT(Magic, Label, Badge)                              \
  {QByteArray(Magic, sizeof(Magic) - 1), QStringLiteral(Label),                \
   QStringLiteral(#Badge)},
#include "RecentFormats.def"
  };
  return Formats;
}

RecentFormat specialFormat(int which) {
  enum { Database, Binary, Missing };
  switch (which) {
#define NEVERD_RECENT_DATABASE(Label, Badge)                                   \
  case Database:                                                               \
    return {{}, QStringLiteral(Label), QStringLiteral(#Badge)};
#define NEVERD_RECENT_BINARY(Label, Badge)                                     \
  case Binary:                                                                 \
    return {{}, QStringLiteral(Label), QStringLiteral(#Badge)};
#define NEVERD_RECENT_MISSING(Label, Badge)                                    \
  case Missing:                                                                \
    return {{}, QStringLiteral(Label), QStringLiteral(#Badge)};
#include "RecentFormats.def"
  }
  return {};
}

/// How the dialog labels \p path: a database, a format its first bytes
/// name, a plain binary file, or a file that is gone.
RecentFormat formatOf(const QString &path) {
  enum { Database, Binary, Missing };
  if (!QFileInfo::exists(path))
    return specialFormat(Missing);
  if (ProjectDatabase::isDatabase(path))
    return specialFormat(Database);
  QFile file(path);
  if (file.open(QIODevice::ReadOnly)) {
    const QByteArray head = file.read(MagicBytes);
    for (const RecentFormat &format : recentFormats())
      if (head.startsWith(format.Magic))
        return format;
  }
  return specialFormat(Binary);
}

QColor chrome(const char *name) {
  return Theme::instance().chrome(QLatin1String(name));
}

/// \p over laid on \p under with \p amount of its strength.
QColor blend(const QColor &under, const QColor &over, qreal amount) {
  return QColor::fromRgbF(
      under.redF() + (over.redF() - under.redF()) * amount,
      under.greenF() + (over.greenF() - under.greenF()) * amount,
      under.blueF() + (over.blueF() - under.blueF()) * amount);
}

QFont sized(QFont font, int pixels, QFont::Weight weight = QFont::Normal) {
  font.setPixelSize(pixels);
  font.setWeight(weight);
  return font;
}

/// \p text without its mnemonic marks, as it is painted.
QString withoutMnemonic(QString text) {
  for (qsizetype At = 0; At < text.size(); ++At)
    if (text.at(At) == u'&')
      text.remove(At, 1);
  return text;
}

/// An SVG resource rendered at \p size logical pixels, crisp at \p ratio.
QPixmap renderSvg(const QString &path, int size, qreal ratio) {
  QSvgRenderer renderer(path);
  QPixmap pixmap(QSize(size, size) * ratio);
  pixmap.fill(Qt::transparent);
  if (!renderer.isValid())
    return pixmap;
  QPainter painter(&pixmap);
  QSizeF shape = renderer.defaultSize();
  shape.scale(QSizeF(pixmap.size()), Qt::KeepAspectRatio);
  renderer.render(&painter,
                  QRectF(QPointF((pixmap.width() - shape.width()) / 2,
                                 (pixmap.height() - shape.height()) / 2),
                         shape));
  pixmap.setDevicePixelRatio(ratio);
  return pixmap;
}

/// A line glyph of the quick start (resources/icons/<name>.svg) at \p size
/// logical pixels in \p color.
QPixmap glyph(const QString &name, int size, const QColor &color, qreal ratio) {
  QPixmap pixmap =
      renderSvg(QStringLiteral(":/neverd/icons/%1.svg").arg(name), size, ratio);
  QPainter painter(&pixmap);
  painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
  painter.fillRect(QRectF(QPointF(), pixmap.deviceIndependentSize()), color);
  return pixmap;
}

/// When a file was last opened, as people say it.
QString whenOpened(const QDateTime &opened) {
  if (!opened.isValid())
    return {};
  const QLocale locale;
  const QString time = locale.toString(opened.time(), QLocale::ShortFormat);
  const QDate today = QDate::currentDate();
  if (opened.date() == today)
    return QCoreApplication::translate("neverd::gui::QuickStartDialog",
                                       "Today, %1")
        .arg(time);
  if (opened.date() == today.addDays(-1))
    return QCoreApplication::translate("neverd::gui::QuickStartDialog",
                                       "Yesterday, %1")
        .arg(time);
  return locale.toString(opened.date(), QLocale::ShortFormat);
}

/// A folder as it reads best: the home folder as ~.
QString folderOf(const QString &path) {
  QString folder = QFileInfo(path).absolutePath();
  const QString home = QDir::homePath();
  if (folder == home || folder.startsWith(home + u'/'))
    folder.replace(0, home.size(), QStringLiteral("~"));
  return QDir::toNativeSeparators(folder);
}

/// A compact, flat keycap.
void drawKey(QPainter &painter, const QRect &cap, const QString &key,
             const QFont &font) {
  const QRectF edge = QRectF(cap).adjusted(0.5, 0.5, -0.5, -0.5);
  painter.setPen(QPen(chrome("QuickStartKeyBorder"), 1));
  painter.setBrush(Qt::NoBrush);
  painter.drawRoundedRect(edge, 3, 3);
  painter.setFont(font);
  painter.setPen(chrome("QuickStartKeyText"));
  painter.drawText(cap, Qt::AlignCenter, key);
}

/// A file outline with a folded corner and a readable format label.
void drawFileBadge(QPainter &painter, const QRect &bounds, const QString &label,
                   const QColor &color, const QFont &font) {
  const QRectF page = QRectF(bounds).adjusted(3.5, 1.5, -3.5, -1.5);
  constexpr qreal fold = 7;
  constexpr qreal corner = 2;
  QPainterPath outline;
  outline.moveTo(page.left() + corner, page.top());
  outline.lineTo(page.right() - fold, page.top());
  outline.lineTo(page.right(), page.top() + fold);
  outline.lineTo(page.right(), page.bottom() - corner);
  outline.quadTo(page.bottomRight(),
                 QPointF(page.right() - corner, page.bottom()));
  outline.lineTo(page.left() + corner, page.bottom());
  outline.quadTo(page.bottomLeft(),
                 QPointF(page.left(), page.bottom() - corner));
  outline.lineTo(page.left(), page.top() + corner);
  outline.quadTo(page.topLeft(), QPointF(page.left() + corner, page.top()));
  outline.closeSubpath();
  painter.setPen(QPen(color, 1));
  painter.setBrush(Qt::NoBrush);
  painter.drawPath(outline);
  QPainterPath crease;
  crease.moveTo(page.right() - fold, page.top());
  crease.lineTo(page.right() - fold, page.top() + fold);
  crease.lineTo(page.right(), page.top() + fold);
  painter.drawPath(crease);
  painter.setPen(color);
  QFont labelFont = sized(font, BadgePixels, QFont::Medium);
  if (QFontMetricsF(labelFont).horizontalAdvance(label) > page.width() - 4)
    labelFont.setPixelSize(BadgePixels - 1);
  painter.setFont(labelFont);
  painter.drawText(page.adjusted(0, fold + 1, 0, -3), Qt::AlignCenter, label);
}

/// The side pane's solid color and the hairline beside the recent files.
class SidePane final : public QWidget {
public:
  using QWidget::QWidget;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), chrome("QuickStartSide"));
    painter.fillRect(QRect(width() - 1, 0, 1, height()),
                     blend(chrome("QuickStartSide"), chrome("Border"), 0.6));
  }
};

/// A way to start: a line glyph, a title over what it does, and its key.
/// The way Enter takes has a subtle background across the row.
class StartButton final : public QPushButton {
public:
  StartButton(QString glyphName, const QString &title,
              const QString &description, QString key, QWidget *parent)
      : QPushButton(title, parent), glyph_(std::move(glyphName)),
        key_(std::move(key)), title_(withoutMnemonic(title)),
        description_(description) {
    setObjectName(QStringLiteral("quickStartAction"));
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(ActionHeight);
    setAccessibleDescription(description);
  }

  QSize sizeHint() const override { return {SideWidth, ActionHeight}; }
  QSize minimumSizeHint() const override { return {0, ActionHeight}; }

protected:
  void enterEvent(QEnterEvent *event) override {
    QPushButton::enterEvent(event);
    update();
  }
  void leaveEvent(QEvent *event) override {
    QPushButton::leaveEvent(event);
    update();
  }

  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
      painter.setOpacity(DisabledOpacity);
    const bool defaultAction = isDefault() && isEnabled();
    const QRectF row = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(Qt::NoPen);
    if (isEnabled() && isDown())
      painter.setBrush(chrome("QuickStartPressed"));
    else if (isEnabled() && underMouse())
      painter.setBrush(chrome("QuickStartActionHover"));
    else if (defaultAction)
      painter.setBrush(chrome("QuickStartDefaultAction"));
    else
      painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(row, RowRadius, RowRadius);
    if (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
      painter.setPen(QPen(chrome("QuickStartFocus"), 1));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(row, RowRadius, RowRadius);
    }

    const int middle = height() / 2;
    const QRect glyphArea(12, middle - ActionGlyphSlot / 2, ActionGlyphSlot,
                          ActionGlyphSlot);
    painter.drawPixmap(glyphArea.left() + (ActionGlyphSlot - GlyphSize) / 2,
                       glyphArea.top() + (ActionGlyphSlot - GlyphSize) / 2,
                       glyph(glyph_, GlyphSize,
                             chrome(defaultAction ? "QuickStartDefaultGlyph"
                                                  : "QuickStartGlyph"),
                             devicePixelRatioF()));

    const QFont keyFont = sized(font(), KeyPixels, QFont::Medium);
    const int capWidth =
        std::max(20, QFontMetrics(keyFont).horizontalAdvance(key_) + 12);
    const QRect cap(width() - 12 - capWidth, middle - 10, capWidth, 20);
    drawKey(painter, cap, key_, keyFont);

    const int left = glyphArea.right() + 1 + 12;
    const int width = cap.left() - 12 - left;
    const QFont titleFont = sized(font(), TitlePixels, QFont::Medium);
    const QFont bodyFont = sized(font(), BodyPixels);
    const QFontMetrics titleMetrics(titleFont), bodyMetrics(bodyFont);
    int top = middle - (titleMetrics.height() + 4 + bodyMetrics.height()) / 2;
    painter.setFont(titleFont);
    painter.setPen(chrome("QuickStartTitle"));
    painter.drawText(QRect(left, top, width, titleMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     titleMetrics.elidedText(title_, Qt::ElideRight, width));
    top += titleMetrics.height() + 4;
    painter.setFont(bodyFont);
    painter.setPen(chrome("QuickStartSecondaryText"));
    painter.drawText(
        QRect(left, top, width, bodyMetrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        bodyMetrics.elidedText(description_, Qt::ElideRight, width));
  }

private:
  QString glyph_, key_, title_, description_;
};

/// A recent file: its format badge, its name over its folder, and when it
/// was last opened.
class RecentDelegate final : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  QSize sizeHint(const QStyleOptionViewItem &,
                 const QModelIndex &) const override {
    return {0, RecentRowHeight};
  }

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const bool missing = index.data(MissingRole).toBool();
    if (selected || hovered) {
      painter->setPen(Qt::NoPen);
      painter->setBrush(selected ? chrome("QuickStartSelection")
                                 : chrome("Hover"));
      painter->drawRoundedRect(QRectF(option.rect).adjusted(0, 2, 0, -2),
                               RowRadius, RowRadius);
    }
    if ((option.state & QStyle::State_HasFocus) && option.widget &&
        option.widget->window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
      painter->setPen(QPen(chrome("QuickStartFocus"), 1));
      painter->setBrush(Qt::NoBrush);
      painter->drawRoundedRect(
          QRectF(option.rect).adjusted(0.5, 2.5, -0.5, -2.5), RowRadius,
          RowRadius);
    }
    const QRect content = option.rect.adjusted(12, 0, -12, 0);
    const int middle = content.center().y();

    const QRect badge(content.left(), middle - BadgeSize / 2, BadgeSize,
                      BadgeSize);
    const QColor badgeColor =
        selected && !missing
            ? chrome("RecentBadgeSelectedText")
            : Theme::instance().chrome(index.data(BadgeRole).toString());
    drawFileBadge(*painter, badge, index.data(LabelRole).toString(), badgeColor,
                  option.font);

    const QColor name = missing    ? chrome("DisabledText")
                        : selected ? chrome("QuickStartSelectionText")
                                   : chrome("QuickStartTitle");
    const QColor secondary = missing ? chrome("DisabledText")
                             : selected
                                 ? blend(chrome("QuickStartSelection"),
                                         chrome("QuickStartSelectionText"), 0.7)
                                 : chrome("QuickStartSecondaryText");
    const int left = badge.right() + 1 + 12;
    const int width = content.right() + 1 - left;
    const QFont nameFont = sized(option.font, NamePixels, QFont::Medium);
    const QFont smallFont = sized(option.font, BodyPixels);
    const QFontMetrics nameMetrics(nameFont), smallMetrics(smallFont);
    int top = middle - (nameMetrics.height() + 4 + smallMetrics.height()) / 2;
    painter->setFont(nameFont);
    painter->setPen(name);
    painter->drawText(
        QRect(left, top, width, nameMetrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                               Qt::ElideMiddle, width));
    top += nameMetrics.height() + 4;
    painter->setFont(smallFont);
    // Metadata shares the second line so the filename keeps the full width.
    const QString when = index.data(WhenRole).toString();
    const int whenWidth =
        when.isEmpty()
            ? 0
            : std::min(smallMetrics.horizontalAdvance(when) + 2, width / 3);
    if (whenWidth) {
      painter->setPen(selected || missing ? secondary
                                          : chrome("QuickStartMetadataText"));
      painter->drawText(
          QRect(left + width - whenWidth, top, whenWidth,
                smallMetrics.height()),
          Qt::AlignRight | Qt::AlignVCenter,
          smallMetrics.elidedText(when, Qt::ElideRight, whenWidth));
    }
    const int folderWidth = width - (whenWidth ? whenWidth + 16 : 0);
    painter->setPen(secondary);
    painter->drawText(
        QRect(left, top, folderWidth, smallMetrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        smallMetrics.elidedText(folderOf(index.data(PathRole).toString()),
                                Qt::ElideMiddle, folderWidth));
    painter->restore();
  }
};

/// The recent files, with a word of what belongs there while it is empty.
/// Delete forgets the selected file.
class RecentList final : public QListWidget {
public:
  using QListWidget::QListWidget;
  std::function<void()> forget;

protected:
  void keyPressEvent(QKeyEvent *event) override {
    if (event->matches(QKeySequence::Delete) && forget) {
      forget();
      event->accept();
      return;
    }
    QListWidget::keyPressEvent(event);
  }

  void paintEvent(QPaintEvent *event) override {
    QListWidget::paintEvent(event);
    if (count())
      return;
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect area = viewport()->rect();
    int top = area.center().y() - 64;
    painter.drawPixmap(area.center().x() - EmptyGlyphSize / 2, top,
                       glyph(QStringLiteral("start_previous"), EmptyGlyphSize,
                             chrome("DisabledText"), devicePixelRatioF()));
    top += EmptyGlyphSize + 16;
    const QFont heading = sized(font(), EmptyPixels, QFont::DemiBold);
    painter.setFont(heading);
    painter.setPen(chrome("QuickStartTitle"));
    painter.drawText(
        QRect(area.left(), top, area.width(), QFontMetrics(heading).height()),
        Qt::AlignCenter,
        QCoreApplication::translate("neverd::gui::QuickStartDialog",
                                    "No recent files"));
    top += QFontMetrics(heading).height() + 6;
    painter.setFont(sized(font(), BodyPixels));
    painter.setPen(chrome("PlaceholderText"));
    painter.drawText(
        QRect(area.center().x() - EmptyTextWidth / 2, top, EmptyTextWidth,
              area.height() - top),
        Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
        QCoreApplication::translate("neverd::gui::QuickStartDialog",
                                    "The files you open appear here. Choose "
                                    "New, or drop a file on this window."));
  }
};

/// What dropping the file dragged over the dialog does: open it.
class DropVeil final : public QWidget {
public:
  explicit DropVeil(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("quickStartDropTarget"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), chrome("Base"));
    QPen frame(chrome("Focus"), 1.5);
    frame.setDashPattern({4, 3});
    painter.setPen(frame);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(
        QRectF(rect()).adjusted(DropInset, DropInset, -DropInset, -DropInset),
        12, 12);
    const QPoint center = rect().center();
    painter.drawPixmap(center.x() - DropGlyphSize / 2,
                       center.y() - DropGlyphSize - 4,
                       glyph(QStringLiteral("start_drop"), DropGlyphSize,
                             chrome("Focus"), devicePixelRatioF()));
    const QFont font = sized(this->font(), EmptyPixels, QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(chrome("QuickStartTitle"));
    painter.drawText(
        QRect(0, center.y() + 12, width(), QFontMetrics(font).height()),
        Qt::AlignCenter,
        QCoreApplication::translate("neverd::gui::QuickStartDialog",
                                    "Drop a file here to open it"));
  }
};

} // namespace

QuickStartDialog::QuickStartDialog(QWidget *parent) : QDialog(parent) {
  setObjectName(QStringLiteral("quickStartDialog"));
  setWindowTitle(tr("Quick start"));
  setWindowIcon(icon(QStringLiteral("app")));
  setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                 Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
  setAcceptDrops(true);
  setMinimumSize(MinimumDialogSize);
  setSizeGripEnabled(true);
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // The product, its ways to start, and whether it greets the next start.
  auto *side = new SidePane(this);
  side->setFixedWidth(SideWidth);
  auto *sideLayout = new QVBoxLayout(side);
  sideLayout->setContentsMargins(16, SideTop, 17, 20);
  sideLayout->setSpacing(0);
  auto *logo = new QLabel(side);
  logo->setFixedSize(LogoSize, LogoSize);
  const auto paintLogo = [logo] {
    logo->setPixmap(renderSvg(
        QString::fromLatin1(Theme::instance().dark() ? DarkLogo : LightLogo),
        LogoSize, qApp->devicePixelRatio()));
  };
  paintLogo();
  sideLayout->addWidget(logo, 0, Qt::AlignHCenter);
  sideLayout->addSpacing(12);
  auto *title = new QLabel(QStringLiteral("NeverD"), side);
  title->setObjectName(QStringLiteral("quickStartTitle"));
  title->setFont(sized(title->font(), BrandPixels, QFont::DemiBold));
  title->setAlignment(Qt::AlignCenter);
  sideLayout->addWidget(title);
  sideLayout->addSpacing(4);
  auto *subtitle =
      new QLabel(tr("Interactive disassembler and decompiler"), side);
  subtitle->setObjectName(QStringLiteral("quickStartSubtitle"));
  subtitle->setFont(sized(subtitle->font(), BodyPixels));
  subtitle->setAlignment(Qt::AlignCenter);
  subtitle->setWordWrap(true);
  sideLayout->addWidget(subtitle);
  sideLayout->addSpacing(2);
  auto *version = new QLabel(
      tr("Version %1").arg(QApplication::applicationVersion()), side);
  version->setObjectName(QStringLiteral("quickStartVersion"));
  version->setFont(sized(version->font(), BodyPixels));
  version->setAlignment(Qt::AlignCenter);
  sideLayout->addWidget(version);
  sideLayout->addSpacing(32);
  auto *newButton =
      new StartButton(QStringLiteral("start_new"), tr("&New"),
                      tr("Disassemble a new file"), QStringLiteral("N"), side);
  auto *goButton =
      new StartButton(QStringLiteral("start_go"), tr("&Go"),
                      tr("Work on your own"), QStringLiteral("G"), side);
  previous_ = new StartButton(QStringLiteral("start_previous"), tr("&Previous"),
                              tr("Load the selected recent file"),
                              QStringLiteral("P"), side);
  sideLayout->addWidget(newButton);
  sideLayout->addSpacing(4);
  sideLayout->addWidget(goButton);
  sideLayout->addSpacing(4);
  sideLayout->addWidget(previous_);
  sideLayout->addStretch(1);
  auto *atStartup = new QCheckBox(tr("&Display at startup"), side);
  atStartup->setObjectName(QStringLiteral("quickStartAtStartup"));
  atStartup->setFont(sized(atStartup->font(), BodyPixels));
  atStartup->setChecked(QSettings().value(settings::QuickStart, true).toBool());
  connect(atStartup, &QCheckBox::toggled, this, [](bool shown) {
    QSettings().setValue(settings::QuickStart, shown);
  });
  auto *startupRow = new QHBoxLayout;
  startupRow->setContentsMargins(12, 0, 0, 0);
  startupRow->addWidget(atStartup);
  sideLayout->addLayout(startupRow);
  layout->addWidget(side);

  // The recent files.
  auto *files = new QWidget(this);
  auto *filesLayout = new QVBoxLayout(files);
  filesLayout->setContentsMargins(20, 32, 20, 20);
  filesLayout->setSpacing(12);
  auto *heading = new QLabel(tr("Recent files"), files);
  heading->setObjectName(QStringLiteral("quickStartSection"));
  heading->setFont(sized(heading->font(), TitlePixels, QFont::DemiBold));
  heading->setContentsMargins(12, 0, 0, 0);
  filesLayout->addWidget(heading);
  auto *recentList = new RecentList(files);
  recentList->forget = [this] { removeSelected(); };
  recent_ = recentList;
  recent_->setObjectName(QStringLiteral("quickStartRecent"));
  recent_->setItemDelegate(new RecentDelegate(recent_));
  recent_->setFrameShape(QFrame::NoFrame);
  recent_->viewport()->setAutoFillBackground(false);
  recent_->viewport()->setAttribute(Qt::WA_Hover);
  recent_->setUniformItemSizes(true);
  recent_->setMouseTracking(true);
  recent_->setAcceptDrops(true);
  recent_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  recent_->setContextMenuPolicy(Qt::CustomContextMenu);
  recent_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *scrollbar = recent_->verticalScrollBar();
  scrollbar->setObjectName(QStringLiteral("quickStartScrollBar"));
  // QAbstractScrollArea polishes its scrollbar before we can name it.
  scrollbar->style()->unpolish(scrollbar);
  scrollbar->style()->polish(scrollbar);
  filesLayout->addWidget(recent_, 1);
  layout->addWidget(files, 1);

  dropTarget_ = new DropVeil(this);
  connect(&Theme::instance(), &Theme::changed, this, [this, paintLogo] {
    paintLogo();
    update();
  });

  connect(newButton, &QPushButton::clicked, this,
          [this] { choose(Start::New); });
  connect(goButton, &QPushButton::clicked, this, &QDialog::reject);
  connect(previous_, &QPushButton::clicked, this,
          &QuickStartDialog::loadSelected);
  connect(recent_, &QListWidget::itemActivated, this,
          &QuickStartDialog::loadSelected);
  connect(recent_, &QListWidget::currentRowChanged, this,
          [this](int row) { previous_->setEnabled(row >= 0); });
  connect(recent_, &QWidget::customContextMenuRequested, this,
          &QuickStartDialog::showRecentMenu);
  // IDA's keys for its buttons, also without Alt.
  for (const auto &[key, button] :
       {std::pair<const char *, QPushButton *>{"N", newButton},
        {"G", goButton},
        {"P", previous_}})
    connect(new QShortcut(QKeySequence(QString::fromLatin1(key)), this),
            &QShortcut::activated, button, &QPushButton::click);

  fillRecent();
  if (recent_->count()) {
    previous_->setDefault(true);
    recent_->setFocus();
  } else {
    newButton->setDefault(true);
    newButton->setFocus();
  }

  QSize preferred =
      QSettings().value(settings::QuickStartSize, DialogSize).toSize();
  if (!preferred.isValid())
    preferred = DialogSize;
  if (const QScreen *display = screen())
    preferred = preferred.boundedTo(display->availableGeometry().size() -
                                    QSize(48, 80));
  resize(preferred.expandedTo(minimumSize()));
  connect(this, &QDialog::finished, this, [this] {
    QSettings().setValue(settings::QuickStartSize,
                         isMaximized() ? normalGeometry().size() : size());
  });
}

void QuickStartDialog::showDropTarget(bool shown) {
  dropTarget_->setGeometry(rect());
  dropTarget_->setVisible(shown);
  if (shown)
    dropTarget_->raise();
}

void QuickStartDialog::resizeEvent(QResizeEvent *event) {
  QDialog::resizeEvent(event);
  dropTarget_->setGeometry(rect());
}

void QuickStartDialog::choose(Start start) {
  start_ = start;
  accept();
}

void QuickStartDialog::loadSelected() {
  const QListWidgetItem *item = recent_->currentItem();
  if (!item)
    return;
  file_ = item->data(PathRole).toString();
  choose(Start::Previous);
}

void QuickStartDialog::removeSelected() {
  QListWidgetItem *item = recent_->currentItem();
  if (!item)
    return;
  const QString path = item->data(PathRole).toString();
  QSettings store;
  QStringList files = store.value(settings::RecentFiles).toStringList();
  files.removeAll(path);
  store.setValue(settings::RecentFiles, files);
  QVariantMap opened = store.value(settings::RecentOpened).toMap();
  opened.remove(path);
  store.setValue(settings::RecentOpened, opened);
  delete recent_->takeItem(recent_->row(item));
  previous_->setEnabled(recent_->currentRow() >= 0);
}

void QuickStartDialog::showRecentMenu(const QPoint &position) {
  QListWidgetItem *item = recent_->itemAt(position);
  if (!item)
    return;
  recent_->setCurrentItem(item);
  QMenu menu(this);
  menu.addAction(tr("&Load"), this, &QuickStartDialog::loadSelected);
  menu.addAction(tr("&Copy path"), this, [item] {
    QApplication::clipboard()->setText(
        QDir::toNativeSeparators(item->data(PathRole).toString()));
  });
  menu.addSeparator();
  menu.addAction(tr("&Remove from list"), this,
                 &QuickStartDialog::removeSelected);
  menu.exec(recent_->viewport()->mapToGlobal(position));
}

void QuickStartDialog::fillRecent() {
  const QSettings store;
  const QVariantMap opened = store.value(settings::RecentOpened).toMap();
  for (const QString &path :
       store.value(settings::RecentFiles).toStringList()) {
    const RecentFormat format = formatOf(path);
    const bool missing = !QFileInfo::exists(path);
    auto *item = new QListWidgetItem(QFileInfo(path).fileName(), recent_);
    item->setData(PathRole, path);
    item->setData(LabelRole, format.Label);
    item->setData(BadgeRole, format.Badge);
    item->setData(MissingRole, missing);
    item->setData(WhenRole, missing
                                ? tr("Missing")
                                : whenOpened(opened.value(path).toDateTime()));
    QString details = QDir::toNativeSeparators(path);
    if (const QString when = item->data(WhenRole).toString(); !when.isEmpty())
      details += u'\n' + (missing ? when : tr("Last opened: %1").arg(when));
    item->setToolTip(details);
  }
  if (recent_->count())
    recent_->setCurrentRow(0);
  previous_->setEnabled(recent_->count() > 0);
}

} // namespace neverd::gui
