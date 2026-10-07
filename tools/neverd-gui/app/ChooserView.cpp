#include "ChooserView.h"

#include "AddressSpace.h"
#include "Icons.h"
#include "Session.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace {
constexpr int PageSize = 256;
constexpr int MaxCachedPages = 64;
constexpr int FilterDebounceMs = 150;

struct ChooserSpec {
  const char *operation;
  const char *title;
  const char *icon;
  bool filterable;
};
constexpr ChooserSpec Specs[] = {
#define NEVERD_CHOOSER(Id, Operation, Title, Icon, Filterable)                 \
  {Operation, Title, Icon, Filterable},
#include "Choosers.def"
};

struct ColumnSpec {
  ChooserKind chooser;
  const char *field;
  const char *title;
  ChooserModel::Format format;
  const char *sortField;
};
constexpr ColumnSpec ColumnSpecs[] = {
#define NEVERD_CHOOSER_COLUMN(Chooser, Field, Title, Kind, SortField)          \
  {ChooserKind::Chooser, Field, Title, ChooserModel::Format::Kind, SortField},
#include "Choosers.def"
};

QVector<ChooserModel::Column> columnsOf(ChooserKind kind) {
  QVector<ChooserModel::Column> columns;
  for (const auto &spec : ColumnSpecs)
    if (spec.chooser == kind)
      columns.append({QString::fromLatin1(spec.field), spec.title, spec.format,
                      QString::fromLatin1(spec.sortField)});
  return columns;
}

bool isLibrary(const QJsonObject &function) {
  return function.value("library").toBool() ||
         function.value("display_origin").toString() ==
             QLatin1String("recognition");
}

QString flagLetter(const QString &flags, int index, QChar letter) {
  return index < flags.size() && flags.at(index) == letter
             ? QString(letter)
             : QStringLiteral(".");
}
} // namespace

ChooserModel::ChooserModel(Session &session, const AddressSpace &space,
                           ChooserKind kind, QObject *parent)
    : QAbstractTableModel(parent), session_(session), space_(space),
      kind_(kind),
      operation_(QString::fromLatin1(Specs[static_cast<int>(kind)].operation)),
      columns_(columnsOf(kind)) {
  connect(&session_, &Session::revisionChanged, this, &ChooserModel::reload);
  connect(&session_, &Session::unloaded, this, &ChooserModel::reload);
  if (kind_ == ChooserKind::Functions || kind_ == ChooserKind::Names)
    connect(&session_, &Session::functionsChanged, this, &ChooserModel::reload);
}

QString ChooserModel::title() const {
  return QCoreApplication::translate("Choosers",
                                     Specs[static_cast<int>(kind_)].title);
}
QString ChooserModel::iconName() const {
  return QString::fromLatin1(Specs[static_cast<int>(kind_)].icon);
}
bool ChooserModel::filterable() const {
  return Specs[static_cast<int>(kind_)].filterable;
}

void ChooserModel::setFilter(const QString &filter) {
  if (filter == filter_)
    return;
  filter_ = filter;
  reload();
}

void ChooserModel::setRequest(const QJsonObject &payload) {
  request_ = payload;
  reload();
}

void ChooserModel::setLocalRows(const QJsonArray &rows) {
  beginResetModel();
  localRows_ = true;
  local_ = rows;
  total_ = int(rows.size());
  endResetModel();
  emit totalChanged(total_);
}

void ChooserModel::reload() {
  if (localRows_)
    return;
  beginResetModel();
  ++serial_;
  pages_.clear();
  pageOrder_.clear();
  inFlight_.clear();
  total_ = 0;
  endResetModel();
  emit totalChanged(total_);
  if (session_.loaded() && !operation_.isEmpty())
    requestPage(0);
}

void ChooserModel::requestPage(int page) const {
  if (inFlight_.contains(page) || !session_.loaded() || operation_.isEmpty())
    return;
  inFlight_.insert(page);
  QJsonObject payload = request_;
  payload["offset"] = page * PageSize;
  payload["limit"] = PageSize;
  if (!filter_.isEmpty())
    payload["filter"] = filter_;
  if (!sortField_.isEmpty()) {
    payload["sort"] = sortField_;
    payload["descending"] = descending_;
  }
  const quint64 serial = serial_;
  auto *self = const_cast<ChooserModel *>(this);
  session_.read(
      operation_, payload, self,
      [self, page, serial](const QJsonObject &result) {
        self->accept(page, serial, result);
      },
      [self, page, serial](const QString &code, const QString &message) {
        if (serial != self->serial_)
          return;
        self->inFlight_.remove(page);
        emit self->failed(code == QLatin1String("analysis_pending")
                              ? tr("References are still being indexed…")
                              : message);
      });
}

void ChooserModel::accept(int page, quint64 serial,
                          const QJsonObject &payload) {
  if (serial != serial_)
    return;
  inFlight_.remove(page);
  const auto items = payload.value("items").toArray();
  const int total = payload.contains("total")
                        ? payload.value("total").toInt()
                        : int(items.size()) + page * PageSize;
  pages_.insert(page, items);
  pageOrder_.remove(page);
  pageOrder_.push_front(page);
  while (int(pageOrder_.size()) > MaxCachedPages) {
    pages_.remove(pageOrder_.back());
    pageOrder_.pop_back();
  }
  if (total != total_) {
    if (total > total_) {
      beginInsertRows({}, total_, total - 1);
      total_ = total;
      endInsertRows();
    } else {
      beginResetModel();
      total_ = total;
      endResetModel();
    }
    emit totalChanged(total_);
  }
  const int first = page * PageSize;
  const int last = std::min(total_, first + PageSize) - 1;
  if (last >= first)
    emit dataChanged(index(first, 0), index(last, columnCount() - 1));
}

QJsonObject ChooserModel::rowObject(int row) const {
  if (localRows_)
    return row >= 0 && row < local_.size() ? local_.at(row).toObject()
                                           : QJsonObject();
  const int page = row / PageSize;
  auto it = pages_.constFind(page);
  if (it == pages_.cend())
    return {};
  const int offset = row % PageSize;
  return offset < it->size() ? it->at(offset).toObject() : QJsonObject();
}

std::optional<Address> ChooserModel::addressAt(int row) const {
  const auto object = rowObject(row);
  if (kind_ == ChooserKind::Segments)
    return addressValue(object.value("start"));
  return addressValue(object.value("address"));
}

std::optional<Address> ChooserModel::referenceAddressAt(int row) const {
  if (kind_ == ChooserKind::StringReferences)
    return addressValue(rowObject(row).value("string_address"));
  return addressAt(row);
}

int ChooserModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : total_;
}

int ChooserModel::columnCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : int(columns_.size());
}

QString ChooserModel::display(const QJsonObject &row,
                              const Column &column) const {
  const QJsonValue value = row.value(column.field);
  const int digits = session_.bitness() == 64 ? 16 : 8;
  switch (column.format) {
  case Format::Text:
  case Format::Name:
    if (value.isDouble())
      return QString::number(value.toDouble(), 'f', 0);
    return value.toString();
  case Format::Address: {
    const auto address = addressValue(value);
    return address ? displayAddress(*address, digits) : QString();
  }
  case Format::SegmentAddress: {
    const auto address = addressValue(value);
    if (!address)
      return {};
    const auto *region = space_.regionOf(*address);
    return (region ? region->name : QStringLiteral("?")) + QLatin1Char(':') +
           displayAddress(*address, digits);
  }
  case Format::SegmentOf: {
    const auto address = addressValue(value);
    const auto *region = address ? space_.regionOf(*address) : nullptr;
    return region ? region->name : QString();
  }
  case Format::Hex: {
    if (value.isString())
      if (const auto parsed = addressValue(value))
        return displayAddress(*parsed, 8);
    return value.isDouble()
               ? displayAddress(static_cast<Address>(value.toDouble()), 8)
               : QString();
  }
  case Format::Decimal:
    return value.isDouble() ? QString::number(qint64(value.toDouble()))
                            : value.toString();
  case Format::FlagRead:
    return flagLetter(value.toString(), 0, QLatin1Char('R'));
  case Format::FlagWrite:
    return flagLetter(value.toString(), 1, QLatin1Char('W'));
  case Format::FlagExecute:
    return flagLetter(value.toString(), 2, QLatin1Char('X'));
  case Format::Direction: {
    const auto from = addressValue(row.value("from"));
    const auto to = addressValue(row.value("to"));
    if (!from || !to)
      return {};
    return *from < *to ? tr("Up") : *from > *to ? tr("Down") : QString();
  }
  }
  return {};
}

QVariant ChooserModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.column() >= columns_.size())
    return {};
  const int row = index.row();
  if (!localRows_ && !pages_.contains(row / PageSize)) {
    if (role == Qt::DisplayRole) {
      // Fetch on demand; the visible rows decide which pages load.
      requestPage(row / PageSize);
      return index.column() == 0 ? QStringLiteral("…") : QString();
    }
    return {};
  }
  const auto object = rowObject(row);
  const auto &column = columns_[index.column()];
  switch (role) {
  case Qt::DisplayRole:
    return display(object, column);
  case Qt::ToolTipRole:
    if (kind_ == ChooserKind::Functions) {
      const auto linkage = object.value("linkage_name").toString();
      if (!linkage.isEmpty() &&
          linkage != object.value("display_name").toString())
        return linkage;
    }
    return {};
  case Qt::DecorationRole:
    if (index.column() == 0 && kind_ == ChooserKind::Functions)
      return icon(isLibrary(object) ? QStringLiteral("function_library")
                  : object.value("thunk").toBool()
                      ? QStringLiteral("imports")
                      : QStringLiteral("functions"));
    return {};
  case Qt::ForegroundRole:
    if (kind_ == ChooserKind::Functions && isLibrary(object))
      return Theme::instance().color(ColorRole::ListingLibraryName);
    return {};
  case Qt::BackgroundRole:
    // Import thunks and library code stand out, as in classic function lists.
    if (kind_ == ChooserKind::Functions) {
      if (object.value("thunk").toBool())
        return Theme::instance().color(ColorRole::ChooserThunkRow);
      if (isLibrary(object))
        return Theme::instance().color(ColorRole::ChooserLibraryRow);
    }
    return {};
  case Qt::FontRole:
    if (column.format == Format::Address || column.format == Format::Hex ||
        column.format == Format::SegmentAddress) {
      QFont font = Theme::instance().codeFont();
      font.setPointSize(QApplication::font().pointSize());
      return font;
    }
    return {};
  case Qt::TextAlignmentRole:
    if (column.format == Format::Hex || column.format == Format::Decimal)
      return int(Qt::AlignRight | Qt::AlignVCenter);
    return {};
  default:
    return {};
  }
}

QVariant ChooserModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole ||
      section >= columns_.size())
    return {};
  return QCoreApplication::translate("Choosers", columns_[section].title);
}

void ChooserModel::sort(int column, Qt::SortOrder order) {
  if (column < 0 || column >= columns_.size() ||
      columns_[column].sortField.isEmpty() || localRows_)
    return;
  sortField_ = columns_[column].sortField;
  descending_ = order == Qt::DescendingOrder;
  reload();
}

//===----------------------------------------------------------------------===//
// ChooserView
//===----------------------------------------------------------------------===//

ChooserView::ChooserView(Session &session, const AddressSpace &space,
                         ChooserKind kind, QWidget *parent)
    : QWidget(parent), model_(new ChooserModel(session, space, kind, this)),
      table_(new QTreeView(this)), filter_(new QLineEdit(this)),
      status_(new QLabel(this)) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  table_->setModel(model_);
  table_->setRootIsDecorated(false);
  table_->setUniformRowHeights(true);
  table_->setAllColumnsShowFocus(true);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSortingEnabled(false);
  table_->header()->setSectionsClickable(true);
  // Rows start in address order with no sort column, as the worker sends them.
  table_->header()->setSortIndicatorShown(true);
  table_->header()->setSortIndicator(-1, Qt::AscendingOrder);
  table_->header()->setStretchLastSection(true);
  table_->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  table_->setIconSize(QSize(16, 16));
  table_->installEventFilter(this);
  layout->addWidget(table_, 1);
  filter_->setPlaceholderText(tr("Quick filter"));
  filter_->setClearButtonEnabled(true);
  filter_->setVisible(false);
  filter_->installEventFilter(this);
  layout->addWidget(filter_);
  status_->setContentsMargins(6, 2, 6, 2);
  layout->addWidget(status_);
  filterTimer_.setSingleShot(true);
  filterTimer_.setInterval(FilterDebounceMs);
  connect(&filterTimer_, &QTimer::timeout, this,
          [this] { model_->setFilter(filter_->text().trimmed()); });
  connect(filter_, &QLineEdit::textChanged, this,
          [this] { filterTimer_.start(); });
  connect(table_->header(), &QHeaderView::sectionClicked, this,
          [this](int section) {
            const auto order = table_->header()->sortIndicatorOrder();
            table_->header()->setSortIndicator(section, order);
            model_->sort(section, order);
          });
  connect(table_, &QTreeView::activated, this,
          [this](const QModelIndex &index) {
            if (const auto address = model_->addressAt(index.row()))
              emit activated(*address);
          });
  connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, [this](const QModelIndex &current) {
            updateStatus();
            if (const auto address = model_->addressAt(current.row()))
              emit currentChanged(*address);
          });
  connect(model_, &ChooserModel::totalChanged, this,
          [this] { updateStatus(); });
  connect(model_, &ChooserModel::failed, this,
          [this](const QString &message) { status_->setText(message); });
  if (kind == ChooserKind::Functions) {
    table_->header()->resizeSection(0, 240);
  } else {
    for (int i = 0; i + 1 < model_->columnCount(); ++i)
      table_->header()->resizeSection(i, 140);
  }
  updateStatus();
}

std::optional<Address> ChooserView::currentAddress() const {
  const auto index = table_->currentIndex();
  return index.isValid() ? model_->addressAt(index.row()) : std::nullopt;
}

std::optional<Address> ChooserView::referenceTarget() const {
  const auto index = table_->currentIndex();
  return index.isValid() ? model_->referenceAddressAt(index.row())
                         : std::nullopt;
}

void ChooserView::focusFilter() {
  filter_->setVisible(true);
  filter_->setFocus(Qt::ShortcutFocusReason);
  filter_->selectAll();
}

void ChooserView::setFilterText(const QString &text) {
  filter_->setVisible(true);
  filter_->setText(text);
}

void ChooserView::revealAddress(Address address) {
  for (int row = 0; row < model_->total(); ++row) {
    const auto object = model_->rowObject(row);
    if (object.isEmpty())
      continue;
    if (model_->addressAt(row) == address) {
      const auto index = model_->index(row, 0);
      table_->setCurrentIndex(index);
      table_->scrollTo(index, QAbstractItemView::PositionAtCenter);
      return;
    }
  }
}

void ChooserView::updateStatus() {
  const int total = model_->total();
  const auto current = table_->currentIndex();
  if (current.isValid())
    status_->setText(tr("Line %1 of %2").arg(current.row() + 1).arg(total));
  else
    status_->setText(total == 1 ? tr("1 item") : tr("%n items", "", total));
}

bool ChooserView::eventFilter(QObject *object, QEvent *event) {
  if (event->type() != QEvent::KeyPress)
    return QWidget::eventFilter(object, event);
  auto *key = static_cast<QKeyEvent *>(event);
  if (object == filter_ && key->key() == Qt::Key_Escape) {
    filter_->clear();
    filter_->setVisible(false);
    table_->setFocus();
    return true;
  }
  if (object == filter_ &&
      (key->key() == Qt::Key_Down || key->key() == Qt::Key_Return)) {
    table_->setFocus();
    if (!table_->currentIndex().isValid() && model_->total())
      table_->setCurrentIndex(model_->index(0, 0));
    return key->key() == Qt::Key_Down;
  }
  if (object == table_) {
    if (key->matches(QKeySequence::Find)) {
      focusFilter();
      return true;
    }
    if (key->matches(QKeySequence::Copy)) {
      QStringList rows;
      for (const auto &index : table_->selectionModel()->selectedRows()) {
        QStringList cells;
        for (int column = 0; column < model_->columnCount(); ++column)
          cells.append(model_->index(index.row(), column).data().toString());
        rows.append(cells.join(QLatin1Char('\t')));
      }
      QApplication::clipboard()->setText(rows.join(QLatin1Char('\n')));
      return true;
    }
    // Typing starts a quick filter, as in the classic list windows.
    const QString text = key->text();
    if (model_->filterable() && !text.isEmpty() && text.at(0).isPrint() &&
        !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
      focusFilter();
      filter_->setText(filter_->text() + text);
      return true;
    }
  }
  return QWidget::eventFilter(object, event);
}

} // namespace neverd::gui
