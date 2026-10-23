//===- ComparisonFlowDialog.cpp - Native dependency evidence review -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Paginated immutable function and node records share the comparison worker.
/// Slices preserve control/effect gaps and never certify unchanged behavior.
//===----------------------------------------------------------------------===//

#include "ComparisonFlowDialog.h"

#include "ComparisonModel.h"
#include "ComparisonSession.h"
#include "Icons.h"

#include "neverd/diff/Limits.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {
QLabel *label(QWidget *Parent, const QString &Text) {
  auto *Out = new QLabel(Text, Parent);
  Out->setWordWrap(true);
  Out->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return Out;
}
QTableView *table(QWidget *Parent, ComparisonModel *Model, const char *Name) {
  auto *View = new QTableView(Parent);
  View->setObjectName(Name);
  View->setModel(Model);
  View->ensurePolished();
  View->setSelectionBehavior(QAbstractItemView::SelectRows);
  View->setSelectionMode(QAbstractItemView::SingleSelection);
  View->setEditTriggers(QAbstractItemView::NoEditTriggers);
  View->setAlternatingRowColors(true);
  View->setWordWrap(false);
  View->verticalHeader()->hide();
  View->horizontalHeader()->setStretchLastSection(true);
  for (int I = 0; I < Model->columnCount(); ++I)
    View->setColumnWidth(
        I,
        View->fontMetrics().horizontalAdvance(
            Model->headerData(I, Qt::Horizontal, Qt::DisplayRole).toString()) +
            28);
  return View;
}
} // namespace

ComparisonFlowDialog::ComparisonFlowDialog(ComparisonSession &S,
                                           QWidget *Parent)
    : QDialog(Parent), Session(S) {
  setObjectName("comparisonFlowDialog");
  setWindowTitle(tr("Dependency evidence — NeverD"));
  setWindowIcon(icon("compare"));
  resize(1280, 800);
  setMinimumSize(920, 580);
  auto *Root = new QVBoxLayout(this);
  Summary =
      label(this, tr("Enable dependency evidence in comparison settings."));
  Summary->setObjectName("flowSummary");
  Root->addWidget(Summary);
  auto *Split = new QSplitter(Qt::Horizontal, this);
  Split->setChildrenCollapsible(false);
  Root->addWidget(Split, 1);
  auto *Left = new QWidget(Split);
  auto *LeftLayout = new QVBoxLayout(Left);
  LeftLayout->setContentsMargins(0, 0, 0, 0);
  Side = new QComboBox(Left);
  Side->setObjectName("flowSide");
  Side->addItem(tr("Primary input"), "primary");
  Side->addItem(tr("Secondary input"), "secondary");
  LeftLayout->addWidget(Side);
  Functions =
      new ComparisonModel(Session, ComparisonModel::Kind::FlowFunctions, this);
  FunctionTable = table(Left, Functions, "flowFunctions");
  for (int Column : {2, 3, 4, 5})
    FunctionTable->hideColumn(Column);
  FunctionTable->horizontalHeader()->setSectionResizeMode(1,
                                                          QHeaderView::Stretch);
  LeftLayout->addWidget(FunctionTable, 1);
  auto *Right = new QSplitter(Qt::Vertical, Split);
  Right->setChildrenCollapsible(false);
  auto *Top = new QWidget(Right);
  auto *TopLayout = new QVBoxLayout(Top);
  TopLayout->setContentsMargins(0, 0, 0, 0);
  FunctionStatus =
      label(Top, tr("Select a function to inspect its SSA operations."));
  FunctionStatus->setObjectName("flowFunctionStatus");
  TopLayout->addWidget(FunctionStatus);
  Nodes = new ComparisonModel(Session, ComparisonModel::Kind::FlowNodes, this);
  NodeTable = table(Top, Nodes, "flowNodes");
  NodeTable->setColumnWidth(2, 145);
  NodeTable->setColumnWidth(4, 190);
  TopLayout->addWidget(NodeTable, 1);
  auto *Actions = new QHBoxLayout;
  Backward = new QPushButton(tr("Trace inputs"), Top);
  Backward->setObjectName("flowBackward");
  Forward = new QPushButton(tr("Trace uses"), Top);
  Forward->setObjectName("flowForward");
  Control = new QCheckBox(tr("Include control dependencies"), Top);
  Control->setChecked(true);
  Cancel = new QPushButton(tr("Cancel"), Top);
  Cancel->setObjectName("flowCancel");
  for (auto *B : {Backward, Forward})
    Actions->addWidget(B);
  Actions->addWidget(Control);
  Actions->addStretch();
  Actions->addWidget(Cancel);
  TopLayout->addLayout(Actions);
  auto *Bottom = new QWidget(Right);
  auto *BottomLayout = new QVBoxLayout(Bottom);
  BottomLayout->setContentsMargins(0, 0, 0, 0);
  SliceStatus =
      label(Bottom, tr("Select an operation, then trace its inputs or uses."));
  SliceStatus->setObjectName("flowSliceStatus");
  BottomLayout->addWidget(SliceStatus);
  Slice = new ComparisonModel(Session, ComparisonModel::Kind::FlowSlice, this);
  SliceTable = table(Bottom, Slice, "flowSlice");
  SliceTable->setColumnWidth(2, 145);
  SliceTable->setColumnWidth(4, 190);
  BottomLayout->addWidget(SliceTable, 1);
  Inspect = new QPushButton(tr("Inspect original function"), Bottom);
  Inspect->setObjectName("flowInspect");
  BottomLayout->addWidget(Inspect, 0, Qt::AlignLeft);
  Split->setStretchFactor(0, 1);
  Split->setStretchFactor(1, 3);
  Split->setSizes({270, 960});
  Root->addWidget(
      label(this, tr("Observed SSA dependencies. Calls, aliases and unresolved "
                     "control flow remain explicit gaps. Semantic validation "
                     "has not been performed.")));
  connect(Side, &QComboBox::currentIndexChanged, this,
          &ComparisonFlowDialog::selectSide);
  connect(&Session, &ComparisonSession::changed, this,
          &ComparisonFlowDialog::refresh);
  for (auto *Model : {Functions, Nodes, Slice})
    connect(Model, &ComparisonModel::failed, this,
            [this](const QString &Error) { SliceStatus->setText(Error); });
  connect(Functions, &ComparisonModel::pageReady, this, [this] {
    if (Functions->rowCount() && !FunctionTable->currentIndex().isValid())
      FunctionTable->selectRow(0);
  });
  connect(FunctionTable->selectionModel(),
          &QItemSelectionModel::currentRowChanged, this,
          [this](const QModelIndex &Index) {
            const auto Row = Functions->row(Index.row());
            Address = Row["function"].toString();
            Slice->clear();
            SliceStatus->setText(
                tr("Select an operation, then trace its inputs or uses."));
            if (Address.isEmpty())
              Nodes->clear();
            else
              Nodes->setQuery({{"side", Side->currentData().toString()},
                               {"kind", "nodes"},
                               {"address", Address}});
            controls();
          });
  connect(
      Nodes, &ComparisonModel::pageReady, this,
      [this](const QJsonObject &Page) {
        const auto Function = Page["function_info"].toObject();
        if (Page["availability"] != QJsonValue("available"))
          FunctionStatus->setText(tr("%1 · %2\n%3")
                                      .arg(Address,
                                           Page["availability"].toString(),
                                           Page["reason"].toString()));
        else
          FunctionStatus->setText(
              tr("%1 · %2 · %3 SSA operations\nSelect an operation to follow "
                 "dependencies. Synthetic operations have block provenance.")
                  .arg(Function["name"].toString(), Address)
                  .arg(Page["total"].toInt()));
        if (Nodes->rowCount() && !NodeTable->currentIndex().isValid())
          NodeTable->selectRow(0);
        controls();
      });
  connect(NodeTable->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, [this] { controls(); });
  connect(Slice, &ComparisonModel::pageReady, this,
          [this](const QJsonObject &Page) {
            QStringList Gaps;
            for (const auto &V : Page["gaps"].toArray())
              Gaps << V.toString().replace('_', ' ');
            SliceStatus->setText(
                tr("%1 slice · %2 nodes · %3\nGaps: %4")
                    .arg(Page["direction"].toString())
                    .arg(Page["total"].toInt())
                    .arg(Page["state"].toString().replace('_', ' '),
                         Gaps.isEmpty() ? tr("none observed within this model")
                                        : Gaps.join(", ")));
          });
  connect(Backward, &QPushButton::clicked, this, [this] { slice("backward"); });
  connect(Forward, &QPushButton::clicked, this, [this] { slice("forward"); });
  connect(Cancel, &QPushButton::clicked, &Session, &ComparisonSession::cancel);
  connect(Inspect, &QPushButton::clicked, this, [this] {
    emit inspectFunction(Side->currentData().toString(), Address);
  });
  refresh();
}
void ComparisonFlowDialog::controls() {
  const bool Node = !Nodes->row(NodeTable->currentIndex().row()).isEmpty();
  const bool Idle =
      Session.ready() && !Session.busy() && !Session.cancellable();
  Backward->setEnabled(Idle && Node);
  Forward->setEnabled(Idle && Node);
  Inspect->setEnabled(Idle && !Address.isEmpty());
  Cancel->setEnabled(Session.cancellable() && !Session.cancelRequested());
}
void ComparisonFlowDialog::refresh() {
  const bool WasRequested = Requested;
  Requested = Session.summary()["settings"]
                  .toObject()["dataflow"]
                  .toObject()["enabled"]
                  .toBool();
  if (Revision != Session.revision() || Requested != WasRequested) {
    Revision = Session.revision();
    selectSide();
  }
  controls();
}
void ComparisonFlowDialog::selectSide() {
  Address.clear();
  Functions->clear();
  Nodes->clear();
  Slice->clear();
  FunctionStatus->setText(
      tr("Select a function to inspect its SSA operations."));
  SliceStatus->setText(
      tr("Select an operation, then trace its inputs or uses."));
  if (!Requested || Revision.isEmpty()) {
    Summary->setText(tr("Enable dependency evidence in comparison settings, "
                        "then compare the inputs."));
  } else {
    const auto SideName = Side->currentData().toString();
    const auto E =
        Session.summary()["dataflow"].toObject()[SideName].toObject();
    const auto Profile = Session.summary()["settings"]
                             .toObject()["dataflow"]
                             .toObject()["profile"]
                             .toString(::neverd::limits::diff::kMedSSAProfile);
    Summary->setText(tr("%1 · %2 available / %3 processed / %4 eligible "
                        "functions · %5 unprocessed\n%6 unsupported, %7 "
                        "resource limited, %8 failed. Collection state: %9.")
                         .arg(Side->currentText())
                         .arg(E["available"].toInt())
                         .arg(E["processed_functions"].toInt())
                         .arg(E["eligible_functions"].toInt())
                         .arg(E["unprocessed_functions"].toInt())
                         .arg(E["unsupported"].toInt())
                         .arg(E["resource_limited"].toInt())
                         .arg(E["failed"].toInt())
                         .arg(E["state"].toString().replace('_', ' ')));
    Summary->setText(Summary->text() + '\n' +
                     tr("SSA representation: %1")
                         .arg(Profile == ::neverd::limits::diff::kLLVMSSAProfile
                                  ? tr("Optimized SSA")
                                  : tr("Original SSA")));
    Functions->setQuery({{"side", SideName}, {"kind", "functions"}});
  }
  controls();
}
void ComparisonFlowDialog::slice(const QString &Direction) {
  const auto Node = Nodes->row(NodeTable->currentIndex().row());
  if (Node.isEmpty() || Address.isEmpty())
    return;
  SliceStatus->setText(tr("Tracing %1 dependencies…").arg(Direction));
  Slice->setQuery({{"side", Side->currentData().toString()},
                   {"address", Address},
                   {"seeds", QJsonArray{Node["id"]}},
                   {"direction", Direction},
                   {"control", Control->isChecked()},
                   {"refresh", true}});
}
} // namespace neverd::gui
