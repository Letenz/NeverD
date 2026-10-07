#pragma once

#include <QDialog>
#include <QTimer>

class QLineEdit;
class QListWidget;
class QLabel;

namespace neverd::gui {

class Session;

/// "Jump anywhere": an address, expression or name with live name matches
/// and the recent jump history.
class JumpDialog final : public QDialog {
  Q_OBJECT
public:
  JumpDialog(Session &session, QWidget *parent = nullptr);
  /// The expression to evaluate, or a chosen match's address.
  QString result() const { return result_; }
  static void remember(const QString &text);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void search();
  void accept() override;
  Session &session_;
  QLineEdit *input_;
  QListWidget *matches_;
  QLabel *hint_;
  QTimer debounce_;
  QString result_;
  quint64 serial_ = 0;
};

} // namespace neverd::gui
