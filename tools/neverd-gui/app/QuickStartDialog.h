#pragma once

#include <QDialog>
#include <QString>

class QListWidget;
class QPushButton;

namespace neverd::gui {

/// The dialog a workbench started without a file greets with, after IDA's
/// Quick start: disassemble a new file (New), work on your own (Go), or load
/// a recent one (Previous).  Recent files show their format, folder and when
/// they were last opened; a file dropped on the dialog opens.
class QuickStartDialog final : public QDialog {
  Q_OBJECT
public:
  enum class Start { None, New, Previous };

  explicit QuickStartDialog(QWidget *parent = nullptr);

  /// What the user chose, once the dialog was accepted.
  Start start() const { return start_; }
  /// The recent file Previous loads.
  QString file() const { return file_; }

private:
  void choose(Start start);
  void loadSelected();
  void removeSelected();
  void showRecentMenu(const QPoint &position);
  void fillRecent();

  QListWidget *recent_ = nullptr;
  QPushButton *previous_ = nullptr;
  Start start_ = Start::None;
  QString file_;
};

} // namespace neverd::gui
