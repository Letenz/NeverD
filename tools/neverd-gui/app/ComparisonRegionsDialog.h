//===- ComparisonRegionsDialog.h - Overlapping region review ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Virtualized transformation hypotheses, shared spans and original paired
/// instructions. This view does not edit the accepted function mapping.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONREGIONSDIALOG_H
#define NEVERD_GUI_COMPARISONREGIONSDIALOG_H

#include <QDialog>

class QComboBox;
class QLabel;
class QTableView;
namespace neverd::gui {
class ComparisonModel;
class ComparisonSession;
class ComparisonRegionsDialog final : public QDialog {
  Q_OBJECT
public:
  ComparisonRegionsDialog(ComparisonSession &Session, QWidget *Parent,
                          bool SSA = false);
signals:
  void inspectFunction(const QString &Side, const QString &Address);

private:
  ComparisonSession &Session;
  bool Flow;
  ComparisonModel *Hypotheses, *Anchors, *Instructions;
  QTableView *HypothesisTable, *AnchorTable, *InstructionTable;
  QComboBox *Kind;
  QComboBox *Coordinates = nullptr;
  QLabel *FragmentScope = nullptr;
  QLabel *Summary, *Evidence;
  QString Revision, RegionId;
  void refresh();
  void filter();
  void selectAnchor();
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONREGIONSDIALOG_H
