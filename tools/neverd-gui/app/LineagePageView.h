//===- LineagePageView.h - Bounded offline evidence table -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Accessible table navigation over engine-owned pages. Failed queries retain
/// the visible page and stale replies never replace a newer selection.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_LINEAGEPAGEVIEW_H
#define NEVERD_GUI_LINEAGEPAGEVIEW_H

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class QLabel;
class QPushButton;
class QStandardItemModel;
class QTableView;

namespace neverd::gui {
class LineageSession;
class LineagePageView final : public QWidget {
  Q_OBJECT
public:
  LineagePageView(LineageSession &Session, const QStringList &Headers,
                  const QStringList &Keys, QWidget *Parent = nullptr);
  void setQuery(QJsonObject Query);
  void clear();
  void refresh();
  QJsonObject selected() const;
  QTableView *table() const { return Table; }
  const QJsonArray &rows() const { return Rows; }

signals:
  void selectionChanged(const QJsonObject &Row);
  void activated(const QJsonObject &Row);
  void pageChanged(const QJsonArray &Rows);

private:
  LineageSession &Session;
  QTableView *Table;
  QStandardItemModel *Model;
  QLabel *Status;
  QPushButton *Previous, *Next, *Retry;
  QStringList Keys;
  QJsonObject Query;
  QJsonArray Rows;
  qint64 Offset = 0, NextOffset = -1;
  quint64 Generation = 0;
  bool Pending = false;
  void page(qint64 First);
  void buttons();
};
} // namespace neverd::gui

#endif // NEVERD_GUI_LINEAGEPAGEVIEW_H
