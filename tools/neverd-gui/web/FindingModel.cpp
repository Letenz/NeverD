//===- FindingModel.cpp - Separate evidence and analyst columns ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Metadata-only finding rows with plain text and keyboard-accessible anchors.
///
//===----------------------------------------------------------------------===//

#include "FindingModel.h"

#include <QCoreApplication>
#include <QJsonObject>

namespace neverd::gui::web {
namespace {
constexpr const char *Fields[] = {
    "kind",        "evidence_status",    "binding",   "confidence",
    "disposition", "disposition_source", "finding_id"};
constexpr const char *Headers[] = {
    QT_TRANSLATE_NOOP("neverd::gui::web::FindingModel", "Kind"),
    QT_TRANSLATE_NOOP("neverd::gui::web::FindingModel", "Evidence"),
    QT_TRANSLATE_NOOP("neverd::gui::web::FindingModel", "Binding"),
    QT_TRANSLATE_NOOP("neverd::gui::web::FindingModel", "Confidence"),
    QT_TRANSLATE_NOOP("neverd::gui::web::FindingModel", "Analyst disposition"),
    QT_TRANSLATE_NOOP("neverd::gui::web::FindingModel", "Disposition source"),
    QT_TRANSLATE_NOOP("neverd::gui::web::FindingModel", "Finding ID")};
} // namespace

int FindingModel::rowCount(const QModelIndex &Parent) const {
  return Parent.isValid() ? 0 : int(Rows.size());
}
int FindingModel::columnCount(const QModelIndex &Parent) const {
  return Parent.isValid() ? 0 : int(std::size(Fields));
}
QVariant FindingModel::data(const QModelIndex &Index, int Role) const {
  if (!Index.isValid() || Index.row() < 0 || Index.row() >= Rows.size() ||
      Index.column() < 0 || Index.column() >= int(std::size(Fields)) ||
      (Role != Qt::DisplayRole && Role != Qt::ToolTipRole))
    return {};
  return finding(Index.row())[QLatin1StringView(Fields[Index.column()])]
      .toString();
}
QVariant FindingModel::headerData(int Section, Qt::Orientation Orientation,
                                  int Role) const {
  if (Orientation != Qt::Horizontal || Role != Qt::DisplayRole || Section < 0 ||
      Section >= int(std::size(Headers)))
    return {};
  return QCoreApplication::translate("neverd::gui::web::FindingModel",
                                     Headers[Section]);
}
QJsonObject FindingModel::finding(int Row) const {
  return Row >= 0 && Row < Rows.size() ? Rows[Row].toObject() : QJsonObject{};
}
void FindingModel::setRows(QJsonArray NewRows) {
  beginResetModel();
  Rows = std::move(NewRows);
  endResetModel();
}
} // namespace neverd::gui::web
