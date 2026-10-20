//===- SetTypeDialog.h - IDA's Y, checked as it is typed -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IDA's Y for a function, or for one of its parameters as Hex-Rays offers
/// it: a C declaration, or the parameter's type, read against the program's
/// local types at every keystroke.  The dialog shows where the text stops
/// reading and why, the parameters it declares with the register or stack
/// slot each arrives in, and completes the local types' names as an
/// editor's quick open matches them.  The function's declaration can also
/// go back to what analysis and the debug information say.
//===----------------------------------------------------------------------===//

#pragma once

#include "Address.h"

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <optional>

class QCompleter;
class QLabel;
class QLineEdit;
class QPushButton;
class QStandardItemModel;
class QTreeWidget;

namespace neverd::gui {

class Session;

class SetTypeDialog final : public QDialog {
  Q_OBJECT
public:
  /// The declaration of \p function, or with \p parameter (from 0, as the
  /// code prints it) that parameter's type; \p name is how the code names
  /// what the dialog types.
  SetTypeDialog(Session &session, Address function,
                std::optional<int> parameter, const QString &name,
                QWidget *parent = nullptr, const QJsonObject &variable = {});

  /// The declaration to state once the dialog is accepted, unless the user
  /// handed the function's type back to analysis (resets).
  QString declaration() const { return declaration_; }
  bool resets() const { return reset_; }

protected:
  bool eventFilter(QObject *object, QEvent *event) override;
  void accept() override;
  void done(int result) override;

private:
  void loaded(const QJsonObject &reply);
  void check();
  void checked(quint64 serial, const QString &text, const QJsonObject &reply);
  void refused(quint64 serial, const QString &text, const QString &message);
  void showParameters(const QJsonArray &parameters);
  void showProblem(const QString &message, int column);
  void complete();
  void completed(quint64 serial, const QJsonObject &reply);
  /// Where the identifier the cursor ends starts, and the identifier.
  std::pair<int, QString> wordAtCursor() const;
  void insertCompletion(const QString &name);

  Session &session_;
  Address function_;
  std::optional<int> parameter_;
  QString name_;
  QJsonObject variable_;
  QLabel *subject_ = nullptr;
  QLineEdit *field_ = nullptr;
  QLabel *status_ = nullptr;
  QTreeWidget *parameters_ = nullptr;
  QPushButton *apply_ = nullptr;
  QPushButton *reset_button_ = nullptr;
  QCompleter *completer_ = nullptr;
  QStandardItemModel *completions_ = nullptr;
  QTimer checkTimer_;
  QTimer completeTimer_;
  quint64 checkSerial_ = 0;
  quint64 completeSerial_ = 0;
  /// The text the last answer judged, whether it reads, and for a
  /// parameter the declaration it makes.
  QString checkedText_;
  bool reads_ = false;
  QString proposal_;
  bool acceptOnAnswer_ = false;
  QString declaration_;
  bool reset_ = false;
};

} // namespace neverd::gui
