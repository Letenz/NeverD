//===- FindingModel.h - Package evidence table ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read-only presentation of backend findings. Review assertions never replace
/// evidence confidence or become a safety verdict.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <QAbstractTableModel>
#include <QJsonArray>

namespace neverd::gui::web {
class FindingModel final : public QAbstractTableModel {
  Q_OBJECT
public:
  explicit FindingModel(QObject *Parent = nullptr)
      : QAbstractTableModel(Parent) {}
  int rowCount(const QModelIndex &Parent = {}) const override;
  int columnCount(const QModelIndex &Parent = {}) const override;
  QVariant data(const QModelIndex &Index, int Role) const override;
  QVariant headerData(int Section, Qt::Orientation Orientation,
                      int Role) const override;
  QJsonObject finding(int Row) const;
  void setRows(QJsonArray Rows);

private:
  QJsonArray Rows;
};
} // namespace neverd::gui::web
