#pragma once

#include <QDialog>
#include <QString>
#include <functional>

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
  /// How the user chose to start.  Escape works on one's own as Go does;
  /// closing the window chooses nothing (None).
  enum class Start { None, Go, New, Previous };
  /// Asks for a file to open in a dialog over \p parent; empty when the user
  /// cancels.
  using FilePicker = std::function<QString(QWidget *parent)>;

  /// New asks \p pickFile for the file while the quick start stays open, so
  /// cancelling returns to it.
  explicit QuickStartDialog(FilePicker pickFile, QWidget *parent = nullptr);

  /// What the user chose, once the dialog closed.
  Start start() const { return start_; }
  /// The file New or Previous loads.
  QString file() const { return file_; }
  /// Shows or hides what a file dropped now would do.  The workbench, which
  /// takes drops for the whole session, says when a file is over the dialog.
  void showDropTarget(bool shown);
  /// Starts as New does with \p path, a file dropped on the dialog.
  void startNew(const QString &path);

protected:
  void keyPressEvent(QKeyEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void showEvent(QShowEvent *event) override;

private:
  void choose(Start start);
  void chooseNew();
  void loadSelected();
  void removeSelected();
  void showRecentMenu(const QPoint &position);
  void fillRecent();

  FilePicker pickFile_;
  QWidget *side_ = nullptr;
  QListWidget *recent_ = nullptr;
  QPushButton *previous_ = nullptr;
  QWidget *dropTarget_ = nullptr;
  Start start_ = Start::None;
  QString file_;
};

} // namespace neverd::gui
