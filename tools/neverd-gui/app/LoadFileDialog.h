#pragma once

#include "Session.h"

#include <QDialog>
#include <QJsonArray>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeWidget;

namespace neverd::gui {

/// "Load a new file": how to load a binary NeverD keeps no project for yet,
/// laid out as IDA's dialog.  The rows come from the worker's identify
/// operation; a row NeverD cannot load is listed, greyed, with its reason.
/// The binary file row reads the file as the processor and at the address
/// the user names.
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
  /// The processor a binary file is read as until the user picks another,
  /// such as the one picked last time; none at first.
  void setBinaryProcessor(const QString &processor);

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  void select(int row);
  /// Whether the chosen row reads the file as a binary file.
  bool binary() const;
  /// The binary file's processor, as the list's chosen item names it.
  QString chosenProcessor() const;
  /// What a hexadecimal field holds; none when it holds no number, and
  /// \p empty when it is empty.
  static std::optional<quint64> number(const QLineEdit *field,
                                       std::optional<quint64> empty = {});
  /// What the chosen binary file row's bytes show of the processor they
  /// hold code for, as the note puts it; empty for a row that reads a header.
  QString identification() const;
  /// Enable OK and explain what keeps it disabled.
  void update();
  /// Fit the file's path into the heading, keeping its name.
  void elidePath();
  QString path_;
  QLabel *heading_;
  QJsonArray rows_;
  QListWidget *loaders_;
  QLabel *processorHeading_;
  QTreeWidget *processors_;
  QLineEdit *base_, *offset_, *size_, *entry_;
  QComboBox *platform_;
  QLabel *note_;
  QCheckBox *analysis_, *indicator_, *debugInfo_;
  QPushButton *ok_;
  QString binaryProcessor_;
  /// The processor the engine reads the bytes as unasked, which the user
  /// keeps by not choosing another.
  QString detectedProcessor_;
};

} // namespace neverd::gui
