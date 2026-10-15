#include "LocalTypesView.h"

#include "Icons.h"
#include "ItemViewActivation.h"
#include "Session.h"
#include "ShrinkableRow.h"
#include "Theme.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QSyntaxHighlighter>
#include <QTabBar>
#include <QTextBlock>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace neverd::gui {

/// Colors the C, C++, Rust or Go a local type renders as: keywords and
/// builtins, the program's types, numbers, directives and comments, the
/// offsets a comment gives standing out from its text.
class TypeSourceHighlighter final : public QSyntaxHighlighter {
public:
  explicit TypeSourceHighlighter(QTextDocument *document)
      : QSyntaxHighlighter(document) {}

  void setLanguage(const QString &language) {
    language_ = language;
    keywords_.clear();
    builtins_.clear();
    const QString family =
        language == QLatin1String("cpp") ? QStringLiteral("c") : language;
#define NEVERD_TYPE_SOURCE_KEYWORD(Language, Word)                             \
  if (family == QLatin1String(Language) ||                                     \
      language == QLatin1String(Language))                                     \
    keywords_.insert(QStringLiteral(Word));
#define NEVERD_TYPE_SOURCE_BUILTIN(Language, Word)                             \
  if (family == QLatin1String(Language))                                       \
    builtins_.insert(QStringLiteral(Word));
#include "TypeSourceVocabulary.def"
    rehighlight();
  }
  void setTypeNames(const QSet<QString> &names) {
    names_ = names;
    rehighlight();
  }
  /// The program's type \p word names: itself, or the type an anonymous
  /// record's generated name (_KTHREAD_0_1) belongs to.
  QString typeOf(QString word) const {
    static const QRegularExpression suffix(QStringLiteral("_[0-9]+$"));
    while (!word.isEmpty()) {
      if (names_.contains(word))
        return word;
      const auto match = suffix.match(word);
      if (!match.hasMatch())
        break;
      word.chop(match.capturedLength());
    }
    return {};
  }

protected:
  void highlightBlock(const QString &text) override {
    const auto &theme = Theme::instance();
    const auto format = [&](ColorRole role) {
      QTextCharFormat result;
      result.setForeground(theme.color(role));
      return result;
    };
    int comment = text.indexOf(QLatin1String("//"));
    if (comment < 0)
      comment = int(text.size());
    const QString code = text.left(comment);
    const QString trimmed = code.trimmed();
    if (trimmed.startsWith(QLatin1Char('#'))) {
      // A directive, or a Rust attribute.
      setFormat(0, comment, format(ColorRole::ListingDirective));
    } else {
      static const QRegularExpression token(QStringLiteral(
          R"((0x[0-9A-Fa-f]+|\b[0-9]+\b)|("[^"]*")|([A-Za-z_][A-Za-z0-9_]*))"));
      for (auto it = token.globalMatch(code); it.hasNext();) {
        const auto match = it.next();
        if (match.capturedLength(1))
          setFormat(match.capturedStart(), match.capturedLength(),
                    format(ColorRole::ListingNumber));
        else if (match.capturedLength(2))
          setFormat(match.capturedStart(), match.capturedLength(),
                    format(ColorRole::ListingString));
        else if (const QString word = match.captured(3);
                 keywords_.contains(word) || builtins_.contains(word))
          setFormat(match.capturedStart(), match.capturedLength(),
                    format(ColorRole::ListingKeyword));
        else if (!typeOf(word).isEmpty())
          setFormat(match.capturedStart(), match.capturedLength(),
                    format(ColorRole::ListingLibraryName));
      }
    }
    if (comment < text.size()) {
      setFormat(comment, int(text.size()) - comment,
                format(ColorRole::ListingComment));
      // The offset and bits a comment gives are what a reader looks for.
      static const QRegularExpression where(QStringLiteral(
          R"(0x[0-9A-Fa-f]+|\+[0-9]+|\bbits? [0-9]+(\.\.[0-9]+)?)"));
      for (auto it = where.globalMatch(text, comment); it.hasNext();) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(),
                  format(ColorRole::ListingNumber));
      }
    }
  }

private:
  QString language_;
  QSet<QString> keywords_, builtins_, names_;
};

namespace {

// The list's columns, and the roles the list and the layout keep.
enum ListColumn {
  OrdinalColumn,
  NameColumn,
  KindColumn,
  SizeColumn,
  MembersColumn,
  ListColumns
};
// The member first, where the tree indents what a record holds.
enum LayoutColumn {
  MemberColumn,
  OffsetColumn,
  SizeOfColumn,
  BarColumn,
  TypeColumn,
  LayoutColumns
};
/// How wide the bar of where a member lies is drawn.
constexpr int BarWidth = 96;
constexpr int SortRole = Qt::UserRole;
constexpr int NameRole = Qt::UserRole + 1;
constexpr int BarStartRole = Qt::UserRole + 2;
constexpr int BarBitsRole = Qt::UserRole + 3;
constexpr int BarKindRole = Qt::UserRole + 4;
constexpr int RefersRole = Qt::UserRole + 5;
enum BarKind { FieldBar, RecordBar, PaddingBar };

/// The languages a type renders in, in tab order after the layout.
struct SourceLanguage {
  const char *id;
  const char *title;
  const char *filter;
  const char *suffix;
};
constexpr SourceLanguage Languages[] = {
    {"c", "C",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "C header (*.h)"), "h"},
    {"cpp", "C++",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "C++ header (*.hpp)"),
     "hpp"},
    {"rust", "Rust",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Rust (*.rs)"), "rs"},
    {"go", "Go", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Go (*.go)"),
     "go"},
};

/// How C and C++ spell builtins (the engine's spellings).
struct Spelling {
  const char *id;
  const char *title;
};
constexpr Spelling Spellings[] = {
    {"windows",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Windows names (ULONG)")},
    {"ida", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView",
                              "IDA names (unsigned int)")},
    {"keywords", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView",
                                   "C keywords (unsigned long)")},
    {"portable", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView",
                                   "Exact widths (uint32_t)")},
};

constexpr char LanguageSetting[] = "localTypes/page";
constexpr char SpellingSetting[] = "localTypes/spelling";
constexpr char HexSetting[] = "localTypes/hexOffsets";
constexpr char DependenciesSetting[] = "localTypes/dependencies";
constexpr char AssertsSetting[] = "localTypes/asserts";
/// The narrowest each pane may get, as the extensions window keeps.
constexpr int MinimumPaneWidth = 48;

QString hex(qint64 value) {
  return QStringLiteral("0x%1")
      .arg(value, 0, 16)
      .toUpper()
      .replace(QLatin1String("0X"), QLatin1String("0x"));
}

/// Draws where a member lies in its type: a track as wide as the type, and
/// the member's bits on it.
class MemberBarDelegate final : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  void setTotalBits(qint64 bits) { total_ = bits; }

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyledItemDelegate::paint(painter, option, index);
    if (total_ <= 0 || !index.data(BarBitsRole).isValid())
      return;
    const auto &theme = Theme::instance();
    const QRect track = option.rect.adjusted(4, option.rect.height() / 3, -4,
                                             -option.rect.height() / 3);
    if (track.width() <= 0 || track.height() <= 0)
      return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    QColor background = theme.color(ColorRole::PrefixUnmapped);
    background.setAlpha(70);
    painter->setBrush(background);
    painter->drawRoundedRect(track, 2, 2);
    const qint64 start = index.data(BarStartRole).toLongLong();
    const qint64 bits = index.data(BarBitsRole).toLongLong();
    const double left = track.width() * double(start) / double(total_);
    const double width =
        std::max(2.0, track.width() * double(bits) / double(total_));
    QColor fill;
    switch (index.data(BarKindRole).toInt()) {
    case RecordBar:
      fill = theme.color(ColorRole::NavLibraryFunction);
      break;
    case PaddingBar:
      fill = theme.color(ColorRole::PrefixUnmapped);
      break;
    default:
      fill = theme.color(ColorRole::ArrowMarker);
      break;
    }
    painter->setBrush(fill);
    painter->drawRoundedRect(QRectF(track.left() + left, track.top(),
                                    std::min(width, track.width() - left),
                                    track.height()),
                             2, 2);
    painter->restore();
  }

private:
  qint64 total_ = 0;
};

} // namespace

LocalTypesView::LocalTypesView(Session &session, QWidget *parent)
    : QWidget(parent), session_(session) {
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  auto *splitter = new QSplitter(this);
  layout->addWidget(splitter);

  // The list.
  auto *left = new QWidget(splitter);
  left->setMinimumWidth(MinimumPaneWidth);
  auto *leftLayout = new QVBoxLayout(left);
  leftLayout->setContentsMargins(4, 4, 4, 4);
  filter_ = new QLineEdit(left);
  filter_->setObjectName(QStringLiteral("localTypesFilter"));
  filter_->setPlaceholderText(tr("Filter types by name"));
  filter_->setClearButtonEnabled(true);
  filter_->installEventFilter(this);
  leftLayout->addWidget(filter_);
  model_ = new QStandardItemModel(0, ListColumns, this);
  model_->setHorizontalHeaderLabels(
      {tr("#"), tr("Name"), tr("Kind"), tr("Size"), tr("Members")});
  proxy_ = new QSortFilterProxyModel(this);
  proxy_->setSourceModel(model_);
  proxy_->setFilterKeyColumn(NameColumn);
  proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
  proxy_->setSortRole(SortRole);
  list_ = new QTreeView(left);
  list_->setObjectName(QStringLiteral("localTypesList"));
  list_->setModel(proxy_);
  list_->setRootIsDecorated(false);
  list_->setUniformRowHeights(true);
  list_->setSortingEnabled(true);
  list_->sortByColumn(OrdinalColumn, Qt::AscendingOrder);
  list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  list_->setAllColumnsShowFocus(true);
  list_->header()->setStretchLastSection(false);
  list_->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
  list_->installEventFilter(this);
  activateOnReturn(*list_);
  leftLayout->addWidget(list_, 1);
  summary_ = new QLabel(left);
  summary_->setObjectName(QStringLiteral("localTypesSummary"));
  leftLayout->addWidget(summary_);

  // The type.
  auto *right = new QWidget(splitter);
  right->setMinimumWidth(MinimumPaneWidth);
  auto *rightLayout = new QVBoxLayout(right);
  rightLayout->setContentsMargins(4, 4, 4, 4);
  auto *headingRow = new QWidget(right);
  auto *headingLayout = new QHBoxLayout(headingRow);
  headingLayout->setContentsMargins(0, 0, 0, 0);
  back_ = new QToolButton(headingRow);
  back_->setIcon(icon(QStringLiteral("back")));
  back_->setToolTip(tr("Back to the type shown before (Esc)"));
  back_->setAutoRaise(true);
  heading_ = new QLabel(headingRow);
  heading_->setObjectName(QStringLiteral("localTypesHeading"));
  heading_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  headingLayout->addWidget(back_);
  headingLayout->addWidget(heading_, 1);
  makeRowShrinkable(*headingRow);
  rightLayout->addWidget(headingRow);

  auto *tabRow = new QWidget(right);
  auto *tabLayout = new QHBoxLayout(tabRow);
  tabLayout->setContentsMargins(0, 0, 0, 0);
  tabs_ = new QTabBar(tabRow);
  tabs_->setObjectName(QStringLiteral("localTypesTabs"));
  tabs_->setDrawBase(false);
  tabs_->setExpanding(false);
  tabs_->addTab(tr("Layout"));
  for (const auto &language : Languages)
    tabs_->addTab(QString::fromLatin1(language.title));
  copy_ = new QToolButton(tabRow);
  copy_->setIcon(icon(QStringLiteral("copy")));
  copy_->setToolTip(tr("Copy the layout or the source"));
  copy_->setAutoRaise(true);
  export_ = new QToolButton(tabRow);
  export_->setIcon(icon(QStringLiteral("file_produce")));
  export_->setToolTip(tr("Export types as source"));
  export_->setAutoRaise(true);
  export_->setPopupMode(QToolButton::InstantPopup);
  auto *exportMenu = new QMenu(export_);
  auto *exportOne = exportMenu->addAction(tr("Export this type…"));
  auto *exportSelected = exportMenu->addAction(tr("Export selected types…"));
  auto *exportAll = exportMenu->addAction(tr("Export all types…"));
  export_->setMenu(exportMenu);
  tabLayout->addWidget(tabs_);
  tabLayout->addStretch(1);
  tabLayout->addWidget(copy_);
  tabLayout->addWidget(export_);
  makeRowShrinkable(*tabRow);
  rightLayout->addWidget(tabRow);

  auto *optionRow = new QWidget(right);
  auto *optionLayout = new QHBoxLayout(optionRow);
  optionLayout->setContentsMargins(0, 0, 0, 0);
  spelling_ = new QComboBox(optionRow);
  spelling_->setObjectName(QStringLiteral("localTypesSpelling"));
  spelling_->setToolTip(tr("How C and C++ name builtin types"));
  for (const auto &entry : Spellings)
    spelling_->addItem(tr(entry.title), QString::fromLatin1(entry.id));
  hex_ = new QCheckBox(tr("Hex offsets"), optionRow);
  dependencies_ = new QCheckBox(tr("With dependencies"), optionRow);
  dependencies_->setToolTip(
      tr("Also declare every type this one needs, so the source compiles"));
  asserts_ = new QCheckBox(tr("Layout checks"), optionRow);
  asserts_->setToolTip(tr("Follow each type with checks of its size and "
                          "offsets that the compiler verifies"));
  optionLayout->addWidget(spelling_);
  optionLayout->addWidget(hex_);
  optionLayout->addWidget(dependencies_);
  optionLayout->addWidget(asserts_);
  optionLayout->addStretch(1);
  makeRowShrinkable(*optionRow);
  rightLayout->addWidget(optionRow);

  pages_ = new QStackedWidget(right);
  layout_ = new QTreeWidget(pages_);
  layout_->setObjectName(QStringLiteral("localTypesLayout"));
  layout_->setColumnCount(LayoutColumns);
  layout_->setHeaderLabels(
      {tr("Member"), tr("Offset"), tr("Size"), tr("Bytes"), tr("Type")});
  layout_->setUniformRowHeights(true);
  layout_->setAllColumnsShowFocus(true);
  layout_->setFont(Theme::instance().codeFont());
  layout_->header()->setStretchLastSection(true);
  layout_->header()->setSectionResizeMode(BarColumn, QHeaderView::Fixed);
  layout_->header()->resizeSection(BarColumn, BarWidth);
  layout_->setItemDelegateForColumn(BarColumn, new MemberBarDelegate(layout_));
  layout_->installEventFilter(this);
  source_ = new QPlainTextEdit(pages_);
  source_->setObjectName(QStringLiteral("localTypesSource"));
  source_->setReadOnly(true);
  source_->setLineWrapMode(QPlainTextEdit::NoWrap);
  source_->setFont(Theme::instance().codeFont());
  source_->viewport()->installEventFilter(this);
  source_->installEventFilter(this);
  highlighter_ = new TypeSourceHighlighter(source_->document());
  pages_->addWidget(layout_);
  pages_->addWidget(source_);
  rightLayout->addWidget(pages_, 1);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({260, 640});

  const QSettings settings;
  tabs_->setCurrentIndex(std::clamp(settings.value(LanguageSetting, 0).toInt(),
                                    0, tabs_->count() - 1));
  pages_->setCurrentIndex(tabs_->currentIndex() ? 1 : 0);
  const int spellingIndex = spelling_->findData(
      settings.value(SpellingSetting, QStringLiteral("windows")).toString());
  spelling_->setCurrentIndex(std::max(spellingIndex, 0));
  hex_->setChecked(settings.value(HexSetting, true).toBool());
  dependencies_->setChecked(
      settings.value(DependenciesSetting, false).toBool());
  asserts_->setChecked(settings.value(AssertsSetting, false).toBool());
  highlighter_->setLanguage(language());

  connect(filter_, &QLineEdit::textChanged, this, [this](const QString &text) {
    proxy_->setFilterFixedString(text);
    updateActions();
  });
  connect(list_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          &LocalTypesView::selectionChanged);
  connect(list_->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, &LocalTypesView::selectionChanged);
  connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
    QSettings().setValue(LanguageSetting, index);
    pages_->setCurrentIndex(index ? 1 : 0);
    highlighter_->setLanguage(language());
    if (index)
      requestSource();
    updateActions();
  });
  connect(spelling_, &QComboBox::currentIndexChanged, this, [this] {
    QSettings().setValue(SpellingSetting, spelling());
    requestLayout();
    requestSource();
  });
  connect(hex_, &QCheckBox::toggled, this, [this](bool on) {
    QSettings().setValue(HexSetting, on);
    laidOut(type_);
    requestSource();
  });
  connect(dependencies_, &QCheckBox::toggled, this, [this](bool on) {
    QSettings().setValue(DependenciesSetting, on);
    requestSource();
  });
  connect(asserts_, &QCheckBox::toggled, this, [this](bool on) {
    QSettings().setValue(AssertsSetting, on);
    requestSource();
  });
  connect(back_, &QToolButton::clicked, this, &LocalTypesView::back);
  connect(copy_, &QToolButton::clicked, this, &LocalTypesView::copy);
  connect(exportOne, &QAction::triggered, this, [this] {
    if (!current_.isEmpty())
      exportTypes({current_});
  });
  connect(exportSelected, &QAction::triggered, this,
          [this] { exportTypes(selectedNames()); });
  connect(exportAll, &QAction::triggered, this, [this] { exportTypes({}); });
  connect(exportMenu, &QMenu::aboutToShow, this,
          [this, exportOne, exportSelected, exportAll] {
            exportOne->setEnabled(!current_.isEmpty());
            exportSelected->setEnabled(selectedNames().size() > 1);
            exportAll->setEnabled(model_->rowCount() > 0);
          });
  connect(layout_, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem *item) {
            const QString refers = item->data(0, RefersRole).toString();
            if (!refers.isEmpty())
              open(refers, true);
          });
  connect(&session_, &Session::opened, this, &LocalTypesView::reload);
  connect(&session_, &Session::revisionChanged, this, &LocalTypesView::reload);
  connect(&session_, &Session::unloaded, this, &LocalTypesView::reload);
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    layout_->setFont(Theme::instance().codeFont());
    source_->setFont(Theme::instance().codeFont());
    highlighter_->rehighlight();
  });
  reload();
}

void LocalTypesView::focusFilter() {
  filter_->setFocus();
  filter_->selectAll();
}

int LocalTypesView::page() const { return tabs_->currentIndex(); }

void LocalTypesView::setPage(int page) {
  tabs_->setCurrentIndex(std::clamp(page, 0, tabs_->count() - 1));
}

QString LocalTypesView::language() const {
  const int index = tabs_->currentIndex() - 1;
  return index >= 0 && index < int(std::size(Languages))
             ? QString::fromLatin1(Languages[index].id)
             : QStringLiteral("c");
}

QString LocalTypesView::spelling() const {
  return spelling_->currentData().toString();
}

QString LocalTypesView::offsetText(qint64 offset, qint64 bit) const {
  QString text = hex_->isChecked() ? hex(offset) : QString::number(offset);
  if (bit >= 0)
    text += QLatin1Char('.') + QString::number(bit);
  return text;
}

QStringList LocalTypesView::selectedNames() const {
  QStringList names;
  for (const QModelIndex &index :
       list_->selectionModel()->selectedRows(NameColumn))
    names.append(index.data(NameRole).toString());
  return names;
}

void LocalTypesView::reload() {
  const quint64 serial = ++listSerial_;
  if (!session_.loaded() || !session_.listsLocalTypes()) {
    model_->removeRows(0, model_->rowCount());
    names_.clear();
    highlighter_->setTypeNames(names_);
    current_.clear();
    type_ = {};
    history_.clear();
    layout_->clear();
    source_->clear();
    heading_->clear();
    summary_->setText(session_.loaded() ? tr("This engine has no local types.")
                                        : QString());
    updateActions();
    return;
  }
  summary_->setText(tr("Reading the program's types…"));
  session_.read(
      QStringLiteral("local_types"), {}, this,
      [this, serial](const QJsonObject &payload) {
        if (serial == listSerial_)
          listed(payload);
      },
      [this, serial](const QString &, const QString &message) {
        if (serial == listSerial_)
          summary_->setText(tr("Cannot read the types: %1").arg(message));
      });
}

void LocalTypesView::listed(const QJsonObject &payload) {
  const QString keep = current_;
  model_->removeRows(0, model_->rowCount());
  names_.clear();
  const QJsonArray types = payload.value("types").toArray();
  for (const QJsonValue &value : types) {
    const QJsonObject type = value.toObject();
    const QString name = type.value("name").toString();
    names_.insert(name);
    QList<QStandardItem *> row;
    const auto cell = [&](const QString &text, const QVariant &sort) {
      auto *item = new QStandardItem(text);
      item->setEditable(false);
      item->setData(sort, SortRole);
      item->setData(name, NameRole);
      row.append(item);
    };
    const qint64 ordinal = type.value("ordinal").toInteger();
    cell(QString::number(ordinal), ordinal);
    cell(name, name.toLower());
    cell(type.value("kind").toString(), type.value("kind").toString());
    if (type.contains("size")) {
      const qint64 size = type.value("size").toInteger();
      cell(hex(size), size);
    } else {
      cell(QStringLiteral("-"), qint64(-1));
    }
    if (type.contains("members")) {
      const qint64 members = type.value("members").toInteger();
      cell(QString::number(members), members);
    } else {
      cell(QString(), qint64(-1));
    }
    row[SizeColumn]->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row[MembersColumn]->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row[OrdinalColumn]->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    model_->appendRow(row);
  }
  highlighter_->setTypeNames(names_);
  for (int column : {OrdinalColumn, KindColumn, SizeColumn, MembersColumn})
    list_->resizeColumnToContents(column);
  const QString source = payload.value("source").toString();
  if (types.isEmpty())
    summary_->setText(source == QLatin1String("pdb")
                          ? tr("The PDB defines no types.")
                          : tr("No local types: this program has no PDB "
                               "with types."));
  else
    summary_->setText(tr("Types: %1 · PDB · %2")
                          .arg(QLocale().toString(qlonglong(types.size())),
                               payload.value("dataModel").toString()));
  const QJsonArray mismatches =
      payload.value("stats").toObject().value("nestingMismatches").toArray();
  if (!mismatches.isEmpty())
    summary_->setToolTip(
        tr("Types that do not lay out as the PDB lists them: %1")
            .arg(mismatches.toVariantList().size()));
  // The type shown before stays, laid out anew; else the first.
  current_.clear();
  if (!keep.isEmpty() && names_.contains(keep))
    open(keep, false);
  else if (proxy_->rowCount() > 0)
    list_->setCurrentIndex(proxy_->index(0, NameColumn));
  updateActions();
}

void LocalTypesView::selectionChanged() {
  const QModelIndex index = list_->currentIndex();
  if (!index.isValid())
    return;
  const QString name = index.data(NameRole).toString();
  const bool remember = rememberNext_;
  rememberNext_ = true;
  if (name == current_)
    return;
  if (remember && !current_.isEmpty())
    history_.append(current_);
  current_ = name;
  requestLayout();
  requestSource();
  updateActions();
}

void LocalTypesView::showType(const QString &name) { open(name, true); }

void LocalTypesView::open(const QString &name, bool remember) {
  const auto rows = model_->match(model_->index(0, NameColumn), NameRole, name,
                                  1, Qt::MatchExactly);
  if (rows.isEmpty())
    return;
  // Show the type even where the filter hides it.
  if (!proxy_->mapFromSource(rows.first()).isValid())
    filter_->clear();
  const QModelIndex index = proxy_->mapFromSource(rows.first());
  rememberNext_ = remember;
  list_->selectionModel()->setCurrentIndex(
      index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  rememberNext_ = true;
  list_->scrollTo(index);
}

void LocalTypesView::back() {
  while (!history_.isEmpty()) {
    const QString previous = history_.takeLast();
    if (names_.contains(previous)) {
      open(previous, false);
      break;
    }
  }
  updateActions();
}

void LocalTypesView::requestLayout() {
  if (current_.isEmpty())
    return;
  const quint64 serial = ++layoutSerial_;
  session_.read(
      QStringLiteral("local_type"),
      {{"name", current_}, {"spelling", spelling()}}, this,
      [this, serial](const QJsonObject &payload) {
        if (serial == layoutSerial_)
          laidOut(payload);
      },
      [this, serial](const QString &, const QString &message) {
        if (serial == layoutSerial_)
          heading_->setText(tr("Cannot lay the type out: %1").arg(message));
      });
}

void LocalTypesView::requestSource() {
  if (current_.isEmpty() || tabs_->currentIndex() == 0)
    return;
  const quint64 serial = ++sourceSerial_;
  const QJsonObject options{{"language", language()},
                            {"spelling", spelling()},
                            {"hexOffsets", hex_->isChecked()},
                            {"asserts", asserts_->isChecked()},
                            {"dependencies", dependencies_->isChecked()},
                            {"types", QJsonArray{current_}}};
  session_.read(
      QStringLiteral("local_types_source"), options, this,
      [this, serial](const QJsonObject &payload) {
        if (serial != sourceSerial_)
          return;
        const int scroll = source_->verticalScrollBar()->value();
        source_->setPlainText(payload.value("source").toString());
        source_->verticalScrollBar()->setValue(scroll);
      },
      [this, serial](const QString &, const QString &message) {
        if (serial == sourceSerial_)
          source_->setPlainText(
              tr("// Cannot render the type: %1").arg(message));
      });
}

void LocalTypesView::laidOut(const QJsonObject &type) {
  type_ = type;
  layout_->clear();
  if (type.isEmpty())
    return;
  const QString kind = type.value("kind").toString();
  static const QHash<QString, const char *> kinds{
      {"struct", QT_TR_NOOP("structure")},
      {"union", QT_TR_NOOP("union")},
      {"enum", QT_TR_NOOP("enumeration")},
      {"typedef", QT_TR_NOOP("typedef")},
      {"function", QT_TR_NOOP("function type")}};
  QString heading = QStringLiteral("<b>%1</b> %2")
                        .arg(type.value("name").toString().toHtmlEscaped(),
                             tr(kinds.value(kind, "")));
  if (type.contains("size")) {
    const qint64 size = type.value("size").toInteger();
    heading +=
        QStringLiteral(" · ") + tr("%1 (%2) bytes").arg(hex(size)).arg(size);
  }
  if (const qint64 align = type.value("alignment").toInteger())
    heading += QStringLiteral(" · ") + tr("aligned to %1").arg(align);
  if (const qint64 pack = type.value("pack").toInteger())
    heading += QStringLiteral(" · ") + tr("packed to %1").arg(pack);
  if (type.value("defined").isBool() && !type.value("defined").toBool())
    heading += QStringLiteral(" · ") + tr("declared only");
  heading_->setText(heading);

  // An enumeration lists values where a record lists offsets.
  if (type.contains("values"))
    layout_->setHeaderLabels(
        {tr("Enumerator"), tr("Value"), QString(), QString(), tr("Type")});
  else
    layout_->setHeaderLabels(
        {tr("Member"), tr("Offset"), tr("Size"), tr("Bytes"), tr("Type")});
  auto *bars = static_cast<MemberBarDelegate *>(
      layout_->itemDelegateForColumn(BarColumn));
  bars->setTotalBits(type.value("size").toInteger() * 8);
  if (type.contains("members")) {
    addMembers(nullptr, type.value("members").toArray());
  } else if (type.contains("values")) {
    for (const QJsonValue &value : type.value("values").toArray()) {
      const QJsonObject entry = value.toObject();
      auto *item = new QTreeWidgetItem(layout_);
      const QJsonValue number = entry.value("value");
      const qint64 raw = number.toInteger();
      item->setText(OffsetColumn, hex_->isChecked() && raw >= 0
                                      ? hex(raw)
                                      : QString::number(raw));
      item->setText(MemberColumn, entry.value("name").toString());
      item->setText(TypeColumn, type.value("underlying").toString());
    }
  } else {
    auto *item = new QTreeWidgetItem(layout_);
    item->setText(MemberColumn, type.value("name").toString());
    item->setText(TypeColumn, type.contains("target")
                                  ? type.value("target").toString()
                                  : type.value("declaration").toString());
    if (const QString refers = type.value("refers").toString();
        !refers.isEmpty())
      item->setData(0, RefersRole, refers);
  }
  layout_->expandAll();
  for (int column : {OffsetColumn, SizeOfColumn, MemberColumn})
    layout_->resizeColumnToContents(column);
  // Long nested names may not push the types out of sight.
  const int widest = layout_->viewport()->width() * 2 / 5;
  if (layout_->columnWidth(MemberColumn) > widest && widest > 0)
    layout_->setColumnWidth(MemberColumn, widest);
}

void LocalTypesView::addMembers(QTreeWidgetItem *parent,
                                const QJsonArray &members) {
  const auto &theme = Theme::instance();
  for (const QJsonValue &value : members) {
    const QJsonObject member = value.toObject();
    const QString kind = member.value("kind").toString();
    auto *item =
        parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(layout_);
    const qint64 offset = member.value("offset").toInteger();
    const qint64 size = member.value("size").toInteger();
    if (kind == QLatin1String("bitfield")) {
      const qint64 storage = member.value("storageOffset").toInteger();
      const qint64 bit = member.value("bitOffset").toInteger() - storage * 8;
      const qint64 width = member.value("bitWidth").toInteger();
      item->setText(OffsetColumn, offsetText(storage, bit));
      // A bitfield's width as C declares it.
      item->setText(SizeOfColumn, QStringLiteral(":%1").arg(width));
      item->setData(BarColumn, BarStartRole,
                    member.value("bitOffset").toInteger());
      item->setData(BarColumn, BarBitsRole, width);
    } else {
      item->setText(OffsetColumn, offsetText(offset));
      item->setText(SizeOfColumn, offsetText(size));
      item->setData(BarColumn, BarStartRole, offset * 8);
      item->setData(BarColumn, BarBitsRole, size * 8);
    }
    item->setTextAlignment(OffsetColumn, Qt::AlignRight | Qt::AlignVCenter);
    item->setTextAlignment(SizeOfColumn, Qt::AlignRight | Qt::AlignVCenter);
    const QString name = member.value("name").toString();
    if (kind == QLatin1String("padding")) {
      item->setText(MemberColumn, tr("(padding)"));
      item->setData(BarColumn, BarKindRole, PaddingBar);
      for (int column = 0; column < LayoutColumns; ++column)
        item->setForeground(column, theme.color(ColorRole::PrefixUnmapped));
      continue;
    }
    if (kind == QLatin1String("struct") || kind == QLatin1String("union")) {
      // An anonymous record reads as the keyword that opens it in source.
      if (name.isEmpty()) {
        item->setText(MemberColumn, kind);
        item->setForeground(MemberColumn,
                            theme.color(ColorRole::ListingKeyword));
      } else {
        item->setText(MemberColumn, name);
        item->setText(TypeColumn, member.value("type").toString());
        item->setForeground(TypeColumn, theme.color(ColorRole::ListingKeyword));
      }
      item->setData(BarColumn, BarKindRole, RecordBar);
      addMembers(item, member.value("members").toArray());
      continue;
    }
    item->setText(MemberColumn, name);
    item->setText(TypeColumn, member.value("type").toString());
    item->setData(BarColumn, BarKindRole, FieldBar);
    if (const QString refers = member.value("refers").toString();
        !refers.isEmpty() && names_.contains(refers)) {
      item->setData(0, RefersRole, refers);
      item->setForeground(TypeColumn,
                          theme.color(ColorRole::ListingLibraryName));
      item->setToolTip(TypeColumn, tr("Double-click to open %1").arg(refers));
    }
  }
}

void LocalTypesView::copy() {
  QString text;
  if (tabs_->currentIndex() == 0) {
    // The layout as a table, one member a line.
    std::function<void(QTreeWidgetItem *, int)> add = [&](QTreeWidgetItem *item,
                                                          int depth) {
      text += item->text(OffsetColumn) + QLatin1Char('\t') +
              item->text(SizeOfColumn) + QLatin1Char('\t') +
              QString(depth * 2, QLatin1Char(' ')) + item->text(MemberColumn) +
              QLatin1Char('\t') + item->text(TypeColumn) + QLatin1Char('\n');
      for (int child = 0; child < item->childCount(); ++child)
        add(item->child(child), depth + 1);
    };
    for (int row = 0; row < layout_->topLevelItemCount(); ++row)
      add(layout_->topLevelItem(row), 0);
  } else {
    text = source_->textCursor().hasSelection()
               ? source_->textCursor().selectedText().replace(
                     QChar::ParagraphSeparator, QLatin1Char('\n'))
               : source_->toPlainText();
  }
  QApplication::clipboard()->setText(text);
}

void LocalTypesView::exportTypes(const QStringList &names) {
  const int index = std::max(tabs_->currentIndex() - 1, 0);
  const SourceLanguage &target = Languages[index];
  const QString base = names.size() == 1
                           ? names.first()
                           : QFileInfo(session_.fileName()).completeBaseName() +
                                 QStringLiteral("_types");
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Export local types"),
      base + QLatin1Char('.') + QString::fromLatin1(target.suffix),
      tr(target.filter));
  if (path.isEmpty())
    return;
  QJsonObject options{{"language", QString::fromLatin1(target.id)},
                      {"spelling", spelling()},
                      {"hexOffsets", hex_->isChecked()},
                      {"asserts", asserts_->isChecked()}};
  if (!names.isEmpty()) {
    options.insert("types", QJsonArray::fromStringList(names));
    options.insert("dependencies", true);
  }
  session_.read(
      QStringLiteral("local_types_source"), options, this,
      [this, path](const QJsonObject &payload) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(payload.value("source").toString().toUtf8()) < 0 ||
            !file.commit())
          summary_->setText(tr("Cannot write %1").arg(path));
        else
          summary_->setText(tr("Exported to %1").arg(path));
      },
      [this](const QString &, const QString &message) {
        summary_->setText(tr("Cannot export the types: %1").arg(message));
      });
}

void LocalTypesView::updateActions() {
  back_->setEnabled(!history_.isEmpty());
  copy_->setEnabled(!current_.isEmpty());
  export_->setEnabled(model_->rowCount() > 0);
  const bool source = tabs_->currentIndex() > 0;
  const bool cLike =
      language() == QLatin1String("c") || language() == QLatin1String("cpp");
  spelling_->setEnabled(!source || cLike);
  dependencies_->setEnabled(source);
  asserts_->setEnabled(source);
}

bool LocalTypesView::eventFilter(QObject *object, QEvent *event) {
  if (!source_)
    return QWidget::eventFilter(object, event);
  if (event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->matches(QKeySequence::Find)) {
      focusFilter();
      return true;
    }
    if (key->key() == Qt::Key_Escape && object != filter_ &&
        !history_.isEmpty()) {
      back();
      return true;
    }
    if (object == filter_ && key->key() == Qt::Key_Down) {
      list_->setFocus();
      return true;
    }
  }
  if (object == source_->viewport() &&
      event->type() == QEvent::MouseButtonDblClick) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    QTextCursor cursor = source_->cursorForPosition(mouse->pos());
    cursor.select(QTextCursor::WordUnderCursor);
    const QString type = highlighter_->typeOf(cursor.selectedText());
    if (!type.isEmpty() && type != current_) {
      open(type, true);
      return true;
    }
  }
  return QWidget::eventFilter(object, event);
}

} // namespace neverd::gui
