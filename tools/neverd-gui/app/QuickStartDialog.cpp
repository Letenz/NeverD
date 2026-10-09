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
#include <QSettings>
#include <QShortcut>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <functional>
#include <utility>
#include <vector>

namespace neverd::gui {
namespace {

constexpr char DarkLogo[] = ":/neverd/icons/app.svg";
constexpr char LightLogo[] = ":/neverd/brand/logo-light.svg";
constexpr QSize DialogSize(780, 480);
constexpr QSize MinimumSize(660, 430);
constexpr int LogoSize = 52;
constexpr int ActionIconSize = 28;
constexpr int ActionHeight = 62;
constexpr int ActionWidth = 280;
constexpr int BadgeSize = 34;
constexpr qreal BadgeRadius = 7;
constexpr int RecentRowHeight = 54;
constexpr int EmptyGlyphSize = 40;
/// Pixel sizes of the dialog's type, from the largest down.
constexpr int TitlePixels = 22;
constexpr int HeadingPixels = 14;
constexpr int NamePixels = 13;
constexpr int BodyPixels = 12;
constexpr int SmallPixels = 11;
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

QColor chrome(const QString &name) { return Theme::instance().chrome(name); }
QColor chrome(const char *name) { return chrome(QLatin1String(name)); }

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

/// A section heading: small capitals in the secondary color.
QLabel *sectionLabel(const QString &text, QWidget *parent) {
  auto *label = new QLabel(text, parent);
  label->setObjectName(QStringLiteral("quickStartSection"));
  QFont font = sized(label->font(), SmallPixels, QFont::DemiBold);
  font.setCapitalization(QFont::AllUppercase);
  font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
  label->setFont(font);
  return label;
}

/// A way to start: an icon, a title over what it does, and its key.
class StartButton final : public QPushButton {
public:
  StartButton(const QString &iconName, const QString &title,
              const QString &description, const QString &key, QWidget *parent)
      : QPushButton(title, parent), icon_(neverd::gui::icon(iconName)),
        key_(key), title_(withoutMnemonic(title)), description_(description) {
    setObjectName(QStringLiteral("quickStartAction"));
    setCursor(Qt::PointingHandCursor);
    setMinimumSize(ActionWidth, ActionHeight);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setAccessibleDescription(description);
    setToolTip(description);
  }

  QSize sizeHint() const override { return {ActionWidth, ActionHeight}; }

protected:
  void paintEvent(QPaintEvent *) override {
    QStylePainter painter(this);
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    option.icon = QIcon();
    painter.drawControl(QStyle::CE_PushButton, option);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool enabled = isEnabled();
    const int middle = rect().center().y();
    const QRect content = rect().adjusted(14, 0, -12, 0);

    const QRect glyph(content.left(), middle - ActionIconSize / 2,
                      ActionIconSize, ActionIconSize);
    painter.drawPixmap(glyph,
                       icon_.pixmap(QSize(ActionIconSize, ActionIconSize),
                                    devicePixelRatioF(),
                                    enabled ? QIcon::Normal : QIcon::Disabled));

    const QFont keyFont = sized(font(), SmallPixels, QFont::DemiBold);
    const QFontMetrics keyMetrics(keyFont);
    const int capWidth = std::max(22, keyMetrics.horizontalAdvance(key_) + 12);
    const QRect cap(content.right() - capWidth + 1, middle - 11, capWidth, 22);
    painter.setPen(QPen(chrome("Border"), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(cap).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    painter.setFont(keyFont);
    painter.setPen(chrome("PlaceholderText"));
    painter.drawText(cap, Qt::AlignCenter, key_);

    const int left = glyph.right() + 14;
    const int width = cap.left() - 10 - left;
    const QFont titleFont = sized(font(), HeadingPixels, QFont::DemiBold);
    const QFont bodyFont = sized(font(), BodyPixels);
    const QFontMetrics titleMetrics(titleFont), bodyMetrics(bodyFont);
    int top = middle - (titleMetrics.height() + 2 + bodyMetrics.height()) / 2;
    painter.setFont(titleFont);
    painter.setPen(enabled ? chrome("TabActiveText") : chrome("DisabledText"));
    painter.drawText(QRect(left, top, width, titleMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     titleMetrics.elidedText(title_, Qt::ElideRight, width));
    top += titleMetrics.height() + 2;
    painter.setFont(bodyFont);
    painter.setPen(enabled ? chrome("PlaceholderText")
                           : chrome("DisabledText"));
    painter.drawText(
        QRect(left, top, width, bodyMetrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        bodyMetrics.elidedText(description_, Qt::ElideRight, width));
  }

private:
  QIcon icon_;
  QString key_, title_, description_;
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
    QStyleOptionViewItem item(option);
    initStyleOption(&item, index);
    item.text.clear();
    item.icon = QIcon();
    const QWidget *widget = item.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &item, painter, widget);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const bool selected = item.state & QStyle::State_Selected;
    const bool missing = index.data(MissingRole).toBool();
    const QRect row = item.rect.adjusted(12, 0, -14, 0);
    const int middle = row.center().y();

    const QRect badge(row.left(), middle - BadgeSize / 2, BadgeSize, BadgeSize);
    painter->setPen(Qt::NoPen);
    painter->setBrush(chrome(index.data(BadgeRole).toString()));
    painter->drawRoundedRect(badge, BadgeRadius, BadgeRadius);
    painter->setFont(sized(item.font, BadgePixels, QFont::Bold));
    painter->setPen(chrome("RecentBadgeText"));
    painter->drawText(badge, Qt::AlignCenter, index.data(LabelRole).toString());

    const QColor secondary =
        selected ? chrome("HighlightedText") : chrome("PlaceholderText");
    const QFont smallFont = sized(item.font, SmallPixels);
    const QFontMetrics smallMetrics(smallFont);
    const QString when = index.data(WhenRole).toString();
    const int whenWidth =
        when.isEmpty() ? 0 : smallMetrics.horizontalAdvance(when) + 16;
    if (!when.isEmpty()) {
      painter->setFont(smallFont);
      painter->setPen(secondary);
      painter->drawText(QRect(row.right() - whenWidth + 1, row.top(), whenWidth,
                              row.height()),
                        Qt::AlignRight | Qt::AlignVCenter, when);
    }

    const int left = badge.right() + 12;
    const int width = row.right() - whenWidth - left;
    const QFont nameFont = sized(item.font, NamePixels, QFont::DemiBold);
    const QFontMetrics nameMetrics(nameFont);
    int top = middle - (nameMetrics.height() + 2 + smallMetrics.height()) / 2;
    painter->setFont(nameFont);
    painter->setPen(selected  ? chrome("HighlightedText")
                    : missing ? chrome("DisabledText")
                              : chrome("TabActiveText"));
    painter->drawText(
        QRect(left, top, width, nameMetrics.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                               Qt::ElideRight, width));
    top += nameMetrics.height() + 2;
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
    if (count())
      return;
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect area = viewport()->rect();
    int top = area.center().y() - 56;
    painter.drawPixmap(QRect(area.center().x() - EmptyGlyphSize / 2, top,
                             EmptyGlyphSize, EmptyGlyphSize),
                       neverd::gui::icon(QStringLiteral("history"))
                           .pixmap(QSize(EmptyGlyphSize, EmptyGlyphSize),
                                   devicePixelRatioF(), QIcon::Disabled));
    top += EmptyGlyphSize + 14;
    const QFont heading = sized(font(), HeadingPixels, QFont::DemiBold);
    painter.setFont(heading);
    painter.setPen(chrome("WindowText"));
    painter.drawText(
        QRect(area.left(), top, area.width(), QFontMetrics(heading).height()),
        Qt::AlignCenter,
        QCoreApplication::translate("neverd::gui::QuickStartDialog",
                                    "No recent files"));
    top += QFontMetrics(heading).height() + 6;
    painter.setFont(sized(font(), BodyPixels));
    painter.setPen(chrome("PlaceholderText"));
    painter.drawText(
        QRect(area.left() + 32, top, area.width() - 64, area.height() - top),
        Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
        QCoreApplication::translate("neverd::gui::QuickStartDialog",
                                    "The files you open appear here. Choose "
                                    "New, or drop a file on this window."));
  }
};

} // namespace

QuickStartDialog::QuickStartDialog(QWidget *parent) : QDialog(parent) {
  setObjectName(QStringLiteral("quickStartDialog"));
  setWindowTitle(tr("Quick start"));
  setWindowIcon(icon(QStringLiteral("app")));
  setAcceptDrops(true);
  setMinimumSize(MinimumSize);
  resize(DialogSize);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // The product: its mark, name and version.
  auto *header = new QWidget(this);
  header->setObjectName(QStringLiteral("quickStartHeader"));
  header->setAttribute(Qt::WA_StyledBackground);
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(24, 18, 24, 18);
  headerLayout->setSpacing(16);
  auto *logo = new QLabel(header);
  logo->setPixmap(renderSvg(
      QString::fromLatin1(Theme::instance().dark() ? DarkLogo : LightLogo),
      LogoSize, qApp->devicePixelRatio()));
  logo->setFixedSize(LogoSize, LogoSize);
  headerLayout->addWidget(logo);
  auto *names = new QVBoxLayout;
  names->setSpacing(2);
  auto *title = new QLabel(QStringLiteral("NeverD"), header);
  title->setObjectName(QStringLiteral("quickStartTitle"));
  title->setFont(sized(title->font(), TitlePixels, QFont::DemiBold));
  auto *subtitle =
      new QLabel(tr("Interactive disassembler and decompiler"), header);
  subtitle->setObjectName(QStringLiteral("quickStartSubtitle"));
  subtitle->setFont(sized(subtitle->font(), BodyPixels));
  names->addStretch();
  names->addWidget(title);
  names->addWidget(subtitle);
  names->addStretch();
  headerLayout->addLayout(names, 1);
  auto *version = new QLabel(
      tr("Version %1").arg(QApplication::applicationVersion()), header);
  version->setObjectName(QStringLiteral("quickStartVersion"));
  version->setFont(sized(version->font(), SmallPixels));
  headerLayout->addWidget(version, 0, Qt::AlignRight | Qt::AlignTop);
  layout->addWidget(header);

  // How to start, beside the recent files.
  auto *body = new QHBoxLayout;
  body->setContentsMargins(24, 18, 24, 14);
  body->setSpacing(24);
  auto *startColumn = new QVBoxLayout;
  startColumn->setSpacing(6);
  startColumn->addWidget(sectionLabel(tr("Start"), this));
  auto *newButton =
      new StartButton(QStringLiteral("file_new"), tr("&New"),
                      tr("Disassemble a new file"), QStringLiteral("N"), this);
  auto *goButton =
      new StartButton(QStringLiteral("workbench"), tr("&Go"),
                      tr("Work on your own"), QStringLiteral("G"), this);
  previous_ = new StartButton(QStringLiteral("history"), tr("&Previous"),
                              tr("Load the selected recent file"),
                              QStringLiteral("P"), this);
  for (QPushButton *button : {static_cast<QPushButton *>(newButton),
                              static_cast<QPushButton *>(goButton), previous_})
    startColumn->addWidget(button);
  startColumn->addStretch();
  body->addLayout(startColumn);
  auto *recentColumn = new QVBoxLayout;
  recentColumn->setSpacing(8);
  recentColumn->addWidget(sectionLabel(tr("Recent files"), this));
  auto *recentList = new RecentList(this);
  recentList->forget = [this] { removeSelected(); };
  recent_ = recentList;
  recent_->setObjectName(QStringLiteral("quickStartRecent"));
  recent_->setItemDelegate(new RecentDelegate(recent_));
  recent_->setUniformItemSizes(true);
  recent_->setMouseTracking(true);
  recent_->setAcceptDrops(true);
  recent_->setContextMenuPolicy(Qt::CustomContextMenu);
  recent_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  recentColumn->addWidget(recent_, 1);
  body->addLayout(recentColumn, 1);
  layout->addLayout(body, 1);

  // Whether this greets the next start, and what a drop does.
  auto *footer = new QWidget(this);
  footer->setObjectName(QStringLiteral("quickStartFooter"));
  footer->setAttribute(Qt::WA_StyledBackground);
  auto *footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(24, 10, 24, 12);
  auto *atStartup = new QCheckBox(tr("&Display at startup"), footer);
  atStartup->setChecked(QSettings().value(settings::QuickStart, true).toBool());
  connect(atStartup, &QCheckBox::toggled, this, [](bool shown) {
    QSettings().setValue(settings::QuickStart, shown);
  });
  auto *hint = new QLabel(tr("Drop a file here to open it"), footer);
  hint->setObjectName(QStringLiteral("quickStartHint"));
  hint->setFont(sized(hint->font(), SmallPixels));
  footerLayout->addWidget(atStartup);
  footerLayout->addStretch();
  footerLayout->addWidget(hint);
  layout->addWidget(footer);

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
