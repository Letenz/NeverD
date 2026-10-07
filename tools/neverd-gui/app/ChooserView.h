#pragma once

#include "Address.h"
#include "ChooserKind.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <list>
#include <optional>

class QLabel;
class QLineEdit;
class QTreeView;

namespace neverd::gui {

class AddressSpace;
class Session;

/// Rows of a worker table operation, fetched in pages on demand so a list of
/// a million functions costs only its visible pages.  Filtering and sorting
/// run in the worker.
class ChooserModel final : public QAbstractTableModel {
  Q_OBJECT
public:
  enum class Format {
    Text,
    Name,
    Address,
    SegmentAddress,
    SegmentOf,
    Hex,
    Decimal,
    FlagRead,
    FlagWrite,
    FlagExecute,
    Direction
  };
  struct Column {
    QString field;
    const char *title;
    Format format;
    QString sortField;
  };

  ChooserModel(Session &session, const AddressSpace &space, ChooserKind kind,
               QObject *parent = nullptr);

  ChooserKind kind() const { return kind_; }
  QString title() const;
  QString iconName() const;
  bool filterable() const;
  void setFilter(const QString &filter);
  QString filter() const { return filter_; }
  /// Extra request fields, such as the address and direction of references.
  void setRequest(const QJsonObject &payload);
  /// Rows supplied locally instead of by the worker (bookmarks).
  void setLocalRows(const QJsonArray &rows);
  void reload();
  int total() const { return total_; }
  bool loading() const { return !inFlight_.isEmpty(); }
  QJsonObject rowObject(int row) const;
  std::optional<Address> addressAt(int row) const;
  /// The item a row stands for when its cross references are listed: a
  /// string reference's string, otherwise the row's address.
  std::optional<Address> referenceAddressAt(int row) const;

  int rowCount(const QModelIndex &parent = {}) const override;
  int columnCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override;
  void sort(int column, Qt::SortOrder order) override;

signals:
  void totalChanged(int total);
  void failed(const QString &message);

private:
  void requestPage(int page) const;
  void accept(int page, quint64 serial, const QJsonObject &payload);
  QString display(const QJsonObject &row, const Column &column) const;

  Session &session_;
  const AddressSpace &space_;
  ChooserKind kind_;
  QString operation_, filter_, sortField_;
  bool descending_ = false;
  QJsonObject request_;
  QVector<Column> columns_;
  mutable QHash<int, QJsonArray> pages_;
  mutable std::list<int> pageOrder_;
  mutable QSet<int> inFlight_;
  QJsonArray local_;
  bool localRows_ = false;
  int total_ = 0;
  quint64 serial_ = 0;
};

/// A list window: table, IDA-style "Line N of M" status and a quick filter.
class ChooserView final : public QWidget {
  Q_OBJECT
public:
  ChooserView(Session &session, const AddressSpace &space, ChooserKind kind,
              QWidget *parent = nullptr);
  ChooserModel &model() { return *model_; }
  QTreeView *table() const { return table_; }
  std::optional<Address> currentAddress() const;
  /// The current row's item for cross references (referenceAddressAt).
  std::optional<Address> referenceTarget() const;
  void focusFilter();
  /// Show the quick filter with \p text applied after the usual debounce.
  void setFilterText(const QString &text);
  /// Select the row whose address equals \p address among loaded rows.
  void revealAddress(Address address);

signals:
  void activated(neverd::gui::Address address);
  void currentChanged(neverd::gui::Address address);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void updateStatus();
  ChooserModel *model_;
  QTreeView *table_;
  QLineEdit *filter_;
  QLabel *status_;
  QTimer filterTimer_;
};

} // namespace neverd::gui
