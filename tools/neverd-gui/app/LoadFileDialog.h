#pragma once

#include "Session.h"

#include <QDialog>
#include <QJsonArray>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTreeWidget;

namespace neverd::gui {

/// "Load a new file": how to load a binary NeverD keeps no project for yet,
/// laid out as IDA's dialog.  The rows come from the worker's identify
/// operation; a row NeverD cannot load is listed, greyed, with its reason.
class LoadFileDialog final : public QDialog {
  Q_OBJECT
public:
  LoadFileDialog(const QString &path, const QJsonArray &rows,
                 QWidget *parent = nullptr);
  /// The chosen row's index in the rows; -1 when no row can be loaded.
  int row() const;
  LoadOptions options() const;
  /// Whether the status line shows the analysis indicator.
  bool indicator() const;
  void setIndicator(bool shown);

private:
  void select(int row);
  QJsonArray rows_;
  QListWidget *loaders_;
  QTreeWidget *processors_;
  QLabel *note_;
  QCheckBox *analysis_, *indicator_, *debugInfo_;
  QPushButton *ok_;
};

} // namespace neverd::gui
