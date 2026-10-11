//===- PackageReviewDialog.h - Offline package review UI --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A bounded package evidence workbench over one NeverD worker owner.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "FindingModel.h"
#include "PackageSession.h"

#include <QDialog>

class QComboBox;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;

namespace neverd::gui::web {
class PackageReviewDialog final : public QDialog {
  Q_OBJECT
public:
  explicit PackageReviewDialog(const QString &WorkerPath,
                               QWidget *Parent = nullptr);

public slots:
  void done(int Result) override;

private:
  void updateState();
  void showAssessment();
  void showFinding();
  void fillArtifacts();
  void fillMembers();
  void saveJournal();
  void loadJournal();
  bool discardUnsaved();
  QJsonObject selectedFinding() const;
  QString manifestID() const;

  PackageSession Session;
  FindingModel Model;
  QSortFilterProxyModel *Filter;
  QTableView *Table;
  QPlainTextEdit *Details;
  QLabel *Status, *Coverage, *JournalStatus;
  QProgressBar *Progress;
  QComboBox *Artifact, *Container, *Member, *Kind, *Disposition;
  QComboBox *RegistryOriginal, *RegistryMetadata, *RegistryDescriptor;
  QGroupBox *Registry;
  QPushButton *CaptureFile, *CaptureDirectory, *Commit, *Extract, *Analyze;
  QPushButton *Apply, *Save, *Load, *Restore, *Cancel;
  QJsonObject LoadedJournal;
  QString SavedHead;
};
} // namespace neverd::gui::web
