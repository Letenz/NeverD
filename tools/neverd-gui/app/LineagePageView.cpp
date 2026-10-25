//===- LineagePageView.cpp - Revision-bound table navigation --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One retained page with full-value copy, keyboard row activation and explicit
/// retry. Labels are presentation only; engine coordinates remain unchanged.
//===----------------------------------------------------------------------===//

#include "LineagePageView.h"

#include "../ResourceLimits.h"
#include "LineageSession.h"
#include "ShrinkableRow.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace {
QString cell(const QJsonValue &Value) {
  if (Value.isString())
    return Value.toString();
  if (Value.isBool())
    return Value.toBool() ? LineagePageView::tr("Yes")
                          : LineagePageView::tr("No");
  if (Value.isDouble())
    return Value.toDouble() == double(Value.toInteger())
               ? QLocale().toString(Value.toInteger())
               : QLocale().toString(Value.toDouble());
  if (Value.isArray()) {
    QStringList Items;
    for (const auto &V : Value.toArray())
      Items.append(cell(V));
    return Items.join(", ");
  }
  return QString::fromUtf8("—");
}
QString display(const QJsonObject &Row, const QString &Key) {
  const auto Value = cell(Row[Key]);
  if (Key == "name" && Value.isEmpty())
    return Row["address"].toString();
  if ((Key == "id" || Key == "report") && Value.size() == 64)
    return Value.left(::neverd::limits::gui::kLineageDigestDisplayCharacters);
  if (Key == "state" || Key == "kind" || Key == "assessment") {
    if (Value == "multiple_functions")
      return LineagePageView::tr("Conflicting counterparts");
    if (Value == "rejected_pair")
      return LineagePageView::tr("Rejected pair connected");
    if (Value == "complete")
      return LineagePageView::tr("Complete");
    if (Value == "cancelled")
      return LineagePageView::tr("Cancelled");
    if (Value == "timed_out")
      return LineagePageView::tr("Deadline reached");
    if (Value == "resource_limited")
      return LineagePageView::tr("Resource limit reached");
    if (Value == "match")
      return LineagePageView::tr("Match");
    if (Value == "unmatch")
      return LineagePageView::tr("Unmatch");
    if (Value == "confirm")
      return LineagePageView::tr("Confirm");
    if (Value == "investigating")
      return LineagePageView::tr("Investigating");
    if (Value == "relevant_change")
      return LineagePageView::tr("Relevant change");
    if (Value == "unrelated_change")
      return LineagePageView::tr("Unrelated change");
    if (Value == "candidate_fix")
      return LineagePageView::tr("Candidate fix");
    if (Value == "patch_present")
      return LineagePageView::tr("Patch present");
    if (Value == "patch_absent")
      return LineagePageView::tr("Patch absent");
    if (Value == "partial_patch")
      return LineagePageView::tr("Partial patch");
    if (Value == "reverted_patch")
      return LineagePageView::tr("Reverted patch");
    if (Value == "inconclusive")
      return LineagePageView::tr("Inconclusive");
  }
  return Value;
}
QString identity(const QJsonObject &Row) {
  for (const auto *Key : {"id", "node", "edge", "conflict", "index"})
    if (Row.contains(Key))
      return QString::fromLatin1(Key) + ':' + cell(Row[Key]);
  return {};
}
} // namespace

LineagePageView::LineagePageView(LineageSession &S, const QStringList &Headers,
                                 const QStringList &Fields, QWidget *Parent)
    : QWidget(Parent), Session(S), Keys(Fields) {
  auto *Root = new QVBoxLayout(this);
  Root->setContentsMargins(0, 0, 0, 0);
  Model = new QStandardItemModel(this);
  Model->setHorizontalHeaderLabels(Headers);
  Table = new QTableView(this);
  Table->setModel(Model);
  Table->setSelectionBehavior(QAbstractItemView::SelectRows);
  Table->setSelectionMode(QAbstractItemView::SingleSelection);
  Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  Table->setAlternatingRowColors(true);
  Table->setWordWrap(false);
  Table->verticalHeader()->hide();
  Table->horizontalHeader()->setStretchLastSection(true);
  Table->setContextMenuPolicy(Qt::CustomContextMenu);
  auto *Copy = new QAction(tr("Copy row evidence"), Table);
  Copy->setShortcut(QKeySequence::Copy);
  Copy->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  Table->addAction(Copy);
  connect(Copy, &QAction::triggered, this, [this] {
    if (!selected().isEmpty())
      QApplication::clipboard()->setText(QString::fromUtf8(
          QJsonDocument(selected()).toJson(QJsonDocument::Indented)));
  });
  Root->addWidget(Table, 1);
  auto *Navigation = new QHBoxLayout;
  Status = new QLabel(tr("Choose a collection to begin."), this);
  Status->setTextFormat(Qt::PlainText);
  makeStatusLine(*Status);
  Previous = new QPushButton(tr("Previous"), this);
  Next = new QPushButton(tr("Next"), this);
  Retry = new QPushButton(tr("Refresh"), this);
  for (auto *Button : {Previous, Next, Retry})
    Button->setAutoDefault(false);
  Navigation->addWidget(Status, 1);
  Navigation->addWidget(Previous);
  Navigation->addWidget(Next);
  Navigation->addWidget(Retry);
  Root->addLayout(Navigation);
  connect(Previous, &QPushButton::clicked, this, [this] {
    page(std::max<qint64>(0,
                          Offset - ::neverd::limits::gui::kComparisonPageSize));
  });
  connect(Next, &QPushButton::clicked, this, [this] { page(NextOffset); });
  connect(Retry, &QPushButton::clicked, this, &LineagePageView::refresh);
  connect(&Session, &LineageSession::changed, this, &LineagePageView::buttons);
  connect(Table->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, [this] { emit selectionChanged(selected()); });
  connect(Table, &QTableView::activated, this,
          [this] { emit activated(selected()); });
  connect(Table, &QWidget::customContextMenuRequested, this,
          [this](const QPoint &Point) {
            const auto Index = Table->indexAt(Point);
            if (!Index.isValid())
              return;
            Table->setCurrentIndex(Index);
            QMenu Menu(this);
            auto *Value = Menu.addAction(tr("Copy full value"));
            auto *Evidence = Menu.addAction(tr("Copy row evidence"));
            const auto Choice =
                Menu.exec(Table->viewport()->mapToGlobal(Point));
            if (Choice == Value)
              QApplication::clipboard()->setText(
                  cell(selected()[Keys[Index.column()]]));
            if (Choice == Evidence)
              QApplication::clipboard()->setText(QString::fromUtf8(
                  QJsonDocument(selected()).toJson(QJsonDocument::Indented)));
          });
  buttons();
}
QJsonObject LineagePageView::selected() const {
  const auto Row = Table->currentIndex().row();
  return Row >= 0 && Row < Rows.size() ? Rows[Row].toObject() : QJsonObject{};
}
void LineagePageView::setQuery(QJsonObject NextQuery) {
  const bool Same = Query == NextQuery;
  Query = std::move(NextQuery);
  page(Same ? Offset : 0);
}
void LineagePageView::clear() {
  ++Generation;
  Pending = false;
  Query = {};
  Rows = {};
  Offset = 0;
  NextOffset = -1;
  Model->removeRows(0, Model->rowCount());
  Status->setText(tr("Select evidence to view."));
  buttons();
}
void LineagePageView::refresh() { page(Offset); }
void LineagePageView::buttons() {
  const bool Available = Session.ready() && !Session.cancelRequested() &&
                         !Query.isEmpty() && !Pending;
  Previous->setEnabled(Available && Offset > 0);
  Next->setEnabled(Available && NextOffset >= 0);
  Retry->setEnabled(Available);
}
void LineagePageView::page(qint64 First) {
  if (First < 0 || Query.isEmpty() || Session.revision().isEmpty())
    return;
  const auto Current = ++Generation;
  const auto Revision = Session.revision();
  const auto Selection = identity(selected());
  Pending = true;
  buttons();
  Status->setText(tr("Loading evidence…"));
  auto Request = Query;
  Request["revision"] = Revision;
  Request["offset"] = First;
  Request["limit"] = ::neverd::limits::gui::kComparisonPageSize;
  Session.request(
      "lineage_query", {{"query", Request}}, this,
      [this, Current, Revision, Selection, First](const QJsonObject &Reply) {
        if (Current != Generation)
          return;
        Pending = false;
        if (Revision != Session.revision()) {
          buttons();
          return;
        }
        if (Reply["status"] != QJsonValue("ok")) {
          const auto Error = Reply["error"].toObject()["message"].toString();
          Status->setText(Rows.isEmpty()
                              ? Error
                              : tr("%1 — previous page retained").arg(Error));
          Status->setToolTip(Status->text());
          buttons();
          return;
        }
        const auto Page = Reply["payload"].toObject();
        if (Page["revision"].toString() != Revision) {
          Status->setText(tr("The graph changed. Refresh this page."));
          buttons();
          return;
        }
        Rows = Page["items"].toArray();
        Offset = First;
        NextOffset = Page["next_offset"].isDouble()
                         ? Page["next_offset"].toInteger()
                         : -1;
        Model->removeRows(0, Model->rowCount());
        int Selected = 0;
        for (int R = 0; R < Rows.size(); ++R) {
          if (!Rows[R].isObject())
            Rows[R] = QJsonObject{{"warning", Rows[R]}};
          const auto Row = Rows[R].toObject();
          if (!Selection.isEmpty() && identity(Row) == Selection)
            Selected = R;
          QList<QStandardItem *> Items;
          for (const auto &Key : Keys) {
            const auto Full = cell(Row[Key]);
            auto *Item = new QStandardItem(display(Row, Key));
            Item->setToolTip(Full);
            Items.append(Item);
          }
          Model->appendRow(Items);
        }
        if (!Rows.isEmpty())
          Table->selectRow(Selected);
        Status->setText(tr("%1–%2 of %3")
                            .arg(Rows.isEmpty() ? 0 : Offset + 1)
                            .arg(Offset + Rows.size())
                            .arg(Page["total"].toInteger()));
        Status->setToolTip(Status->text());
        buttons();
        emit pageChanged(Rows);
      });
}
} // namespace neverd::gui
