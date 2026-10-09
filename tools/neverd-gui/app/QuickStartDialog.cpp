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
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QRadialGradient>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
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
constexpr QSize DialogSize(820, 500);
/// The side pane: the product, above its ways to start.
constexpr int SideWidth = 300;
constexpr int SideTop = 44;
constexpr int LogoSize = 64;
/// The glow the logo sits in, and its strength in the dark and the light
/// theme.
constexpr int GlowRadius = 150;
constexpr qreal DarkGlow = 0.11;
constexpr qreal LightGlow = 0.07;
constexpr int ActionHeight = 56;
constexpr int TileSize = 36;
constexpr qreal TileRadius = 9;
constexpr int GlyphSize = 20;
constexpr qreal RowRadius = 8;
constexpr qreal DisabledOpacity = 0.45;
/// The recent files.
constexpr int RecentRowHeight = 58;
constexpr int BadgeSize = 36;
constexpr qreal BadgeRadius = 8;
/// How strongly a badge is tinted with its format's color, in the dark and
/// the light theme.
constexpr qreal DarkBadgeTint = 0.16;
constexpr qreal LightBadgeTint = 0.11;
/// How far the list fades out at an edge more files lie beyond.
constexpr int FadeHeight = 28;
constexpr int EmptyGlyphSize = 44;
constexpr int EmptyTextWidth = 300;
constexpr int DropGlyphSize = 40;
constexpr int DropInset = 14;
constexpr qreal DropVeilOpacity = 0.97;
/// Pixel sizes of the dialog's type, from the largest down.
constexpr int BrandPixels = 26;
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

/// A key the way a keyboard shows it: a small cap with a deeper lower edge.
void drawKey(QPainter &painter, const QRect &cap, const QString &key,
             const QFont &font) {
  const QRectF edge = QRectF(cap).adjusted(0.5, 0.5, -0.5, -0.5);
  painter.setPen(QPen(chrome("QuickStartKeyBorder"), 1));
  painter.setBrush(chrome("QuickStartKey"));
  painter.drawRoundedRect(edge, 4, 4);
  painter.setPen(QPen(chrome("QuickStartKeyBorder").darker(115), 1));
  painter.drawLine(QPointF(edge.left() + 3, edge.bottom()),
                   QPointF(edge.right() - 3, edge.bottom()));
  painter.setFont(font);
  painter.setPen(chrome("QuickStartKeyText"));
  painter.drawText(cap, Qt::AlignCenter, key);
}

/// The side pane's color, a soft glow around the logo, and the hairline
/// between the pane and the recent files.
class SidePane final : public QWidget {
public:
  using QWidget::QWidget;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), chrome("QuickStartSide"));
    const QPointF logo(width() / 2.0, SideTop + LogoSize / 2.0);
    QColor glow = chrome("QuickStartGlyph");
    glow.setAlphaF(Theme::instance().dark() ? DarkGlow : LightGlow);
    QRadialGradient gradient(logo, GlowRadius);
    gradient.setColorAt(0, glow);
    glow.setAlphaF(0);
    gradient.setColorAt(1, glow);
    painter.fillRect(rect(), gradient);
    painter.fillRect(QRect(width() - 1, 0, 1, height()),
                     blend(chrome("QuickStartSide"), chrome("Border"), 0.6));
  }
};

/// A way to start: a tile with its glyph, a title over what it does, and
/// its key.  The way Enter takes has its tile outlined.
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
    const QRectF row = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(Qt::NoPen);
    if (isEnabled() && isDown())
      painter.setBrush(chrome("QuickStartPressed"));
    else if (isEnabled() && underMouse())
      painter.setBrush(chrome("Hover"));
    else
      painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(row, RowRadius, RowRadius);
    if (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
      painter.setPen(QPen(chrome("Focus"), 1));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(row, RowRadius, RowRadius);
    }

    const int middle = height() / 2;
    const QRect tile(12, middle - TileSize / 2, TileSize, TileSize);
    painter.setPen(Qt::NoPen);
    painter.setBrush(chrome("QuickStartTile"));
    painter.drawRoundedRect(QRectF(tile), TileRadius, TileRadius);
    if (isDefault() && isEnabled()) {
      painter.setPen(QPen(chrome("QuickStartGlyph"), 1.5));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(QRectF(tile).adjusted(0.75, 0.75, -0.75, -0.75),
                              TileRadius - 0.75, TileRadius - 0.75);
    }
    painter.drawPixmap(tile.left() + (TileSize - GlyphSize) / 2,
                       tile.top() + (TileSize - GlyphSize) / 2,
                       glyph(glyph_, GlyphSize, chrome("QuickStartGlyph"),
                             devicePixelRatioF()));

    const QFont keyFont = sized(font(), KeyPixels, QFont::Medium);
    const int capWidth =
        std::max(20, QFontMetrics(keyFont).horizontalAdvance(key_) + 12);
    const QRect cap(width() - 14 - capWidth, middle - 10, capWidth, 20);
    drawKey(painter, cap, key_, keyFont);

    const int left = tile.right() + 1 + 14;
    const int width = cap.left() - 12 - left;
    const QFont titleFont = sized(font(), TitlePixels, QFont::DemiBold);
    const QFont bodyFont = sized(font(), BodyPixels);
    const QFontMetrics titleMetrics(titleFont), bodyMetrics(bodyFont);
    int top = middle - (titleMetrics.height() + 2 + bodyMetrics.height()) / 2;
    painter.setFont(titleFont);
    painter.setPen(chrome("QuickStartTitle"));
    painter.drawText(QRect(left, top, width, titleMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     titleMetrics.elidedText(title_, Qt::ElideRight, width));
    top += titleMetrics.height() + 2;
    painter.setFont(bodyFont);
    painter.setPen(chrome("PlaceholderText"));
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
      painter->drawRoundedRect(QRectF(option.rect).adjusted(0, 1, 0, -1),
                               RowRadius, RowRadius);
    }
    const QRect content = option.rect.adjusted(11, 0, -14, 0);
    const int middle = content.center().y();

    const QRect badge(content.left(), middle - BadgeSize / 2, BadgeSize,
                      BadgeSize);
    const QColor hue =
        Theme::instance().chrome(index.data(BadgeRole).toString());
    QColor tint = hue;
    tint.setAlphaF(Theme::instance().dark() ? DarkBadgeTint : LightBadgeTint);
    painter->setPen(Qt::NoPen);
    painter->setBrush(tint);
    painter->drawRoundedRect(QRectF(badge), BadgeRadius, BadgeRadius);
    QFont badgeFont = sized(option.font, BadgePixels, QFont::Bold);
    badgeFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.4);
    painter->setFont(badgeFont);
    painter->setPen(hue);
    painter->drawText(badge, Qt::AlignCenter, index.data(LabelRole).toString());

    const QColor name = missing    ? chrome("DisabledText")
                        : selected ? chrome("QuickStartSelectionText")
                                   : chrome("QuickStartTitle");
    const QColor secondary = missing ? chrome("DisabledText")
                             : selected
                                 ? blend(chrome("QuickStartSelection"),
                                         chrome("QuickStartSelectionText"), 0.7)
                                 : chrome("PlaceholderText");
    const int left = badge.right() + 1 + 14;
    const int width = content.right() + 1 - left;
    const QFont nameFont = sized(option.font, NamePixels, QFont::Medium);
    const QFont smallFont = sized(option.font, BodyPixels);
    const QFontMetrics nameMetrics(nameFont), smallMetrics(smallFont);
    int top = middle - (nameMetrics.height() + 3 + smallMetrics.height()) / 2;
    const QString when = index.data(WhenRole).toString();
    const int whenWidth =
        when.isEmpty() ? 0 : smallMetrics.horizontalAdvance(when);
    if (whenWidth) {
      painter->setFont(smallFont);
      painter->setPen(secondary);
      painter->drawText(
          QRect(left + width - whenWidth, top, whenWidth, nameMetrics.height()),
          Qt::AlignRight | Qt::AlignVCenter, when);
    }
    const int nameWidth = width - (whenWidth ? whenWidth + 16 : 0);
    painter->setFont(nameFont);
    painter->setPen(name);
    painter->drawText(
        QRect(left, top, nameWidth, nameMetrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                               Qt::ElideRight, nameWidth));
    top += nameMetrics.height() + 3;
    painter->setFont(smallFont);
    painter->setPen(secondary);
    painter->drawText(
        QRect(left, top, width, smallMetrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        smallMetrics.elidedText(folderOf(index.data(PathRole).toString()),
                                Qt::ElideMiddle, width));
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
    QPainter painter(viewport());
    if (count()) {
      fadeEdges(painter);
      return;
    }
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

private:
  /// Fades the list into the dialog at each edge more files lie beyond.
  void fadeEdges(QPainter &painter) {
    const QScrollBar *bar = verticalScrollBar();
    const QRect area = viewport()->rect();
    QColor solid = chrome("Base");
    QColor clear = solid;
    clear.setAlphaF(0);
    if (bar->value() > bar->minimum()) {
      QLinearGradient top(0, area.top(), 0, area.top() + FadeHeight);
      top.setColorAt(0, solid);
      top.setColorAt(1, clear);
      painter.fillRect(QRect(area.left(), area.top(), area.width(), FadeHeight),
                       top);
    }
    if (bar->value() < bar->maximum()) {
      QLinearGradient bottom(0, area.bottom() + 1 - FadeHeight, 0,
                             area.bottom() + 1);
      bottom.setColorAt(0, clear);
      bottom.setColorAt(1, solid);
      painter.fillRect(QRect(area.left(), area.bottom() + 1 - FadeHeight,
                             area.width(), FadeHeight),
                       bottom);
    }
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
    QColor veil = chrome("Base");
    veil.setAlphaF(DropVeilOpacity);
    painter.fillRect(rect(), veil);
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
  // A dialog of fixed size: its title bar has nothing but Close.
  setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                 Qt::WindowCloseButtonHint);
  setAcceptDrops(true);
  setFixedSize(DialogSize);
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // The product, its ways to start, and whether it greets the next start.
  auto *side = new SidePane(this);
  side->setFixedWidth(SideWidth);
  auto *sideLayout = new QVBoxLayout(side);
  sideLayout->setContentsMargins(16, SideTop, 17, 16);
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
  sideLayout->addSpacing(14);
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
  sideLayout->addSpacing(34);
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
  sideLayout->addSpacing(2);
  sideLayout->addWidget(goButton);
  sideLayout->addSpacing(2);
  sideLayout->addWidget(previous_);
  sideLayout->addStretch(1);
  auto *atStartup = new QCheckBox(tr("&Display at startup"), side);
  atStartup->setObjectName(QStringLiteral("quickStartAtStartup"));
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
  filesLayout->setContentsMargins(16, 28, 16, 16);
  filesLayout->setSpacing(10);
  auto *heading = new QLabel(tr("Recent files"), files);
  heading->setObjectName(QStringLiteral("quickStartSection"));
  heading->setFont(sized(heading->font(), TitlePixels, QFont::DemiBold));
  heading->setContentsMargins(11, 0, 0, 0);
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
  // Scrolling moves the faded edges with the files; paint them anew.
  connect(recent_->verticalScrollBar(), &QScrollBar::valueChanged,
          recent_->viewport(), qOverload<>(&QWidget::update));
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
    item->setToolTip(QDir::toNativeSeparators(path));
  }
  if (recent_->count())
    recent_->setCurrentRow(0);
  previous_->setEnabled(recent_->count() > 0);
}

} // namespace neverd::gui
