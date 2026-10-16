//===- TypePickerDialog.h - The local type a pointer points at -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IDA's Structure offset (T), ranked by the code: the program's structures
/// and unions listed by how the accesses the function makes through the
/// operand's pointer land on their members, best first; searched as an
/// editor's quick open searches (a name, a typedef's, Type.member or
/// .member); and the type under the cursor previewed access by access, with
/// what does not fit marked.  The type goes to the operand, and to every
/// other access through the same pointer at once when the user keeps that.
//===----------------------------------------------------------------------===//

#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <optional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeWidget;

namespace neverd::gui {

class Session;

class TypePickerDialog final : public QDialog {
  Q_OBJECT
public:
  /// \p query names the operand as local_types_fit reads it:
  /// {"function","address","operand"?}.
  TypePickerDialog(Session &session, QJsonObject query,
                   QWidget *parent = nullptr);

  /// The record chosen, and how many bytes into it the register points.
  QString typeName() const { return type_; }
  qint64 delta() const { return delta_; }
  /// The operand the type goes to, as the worker chose it.
  std::optional<int> operand() const { return operand_; }
  /// The other instructions that go through the same pointer, when the
  /// user keeps every access; each shows the same structure offset.
  QStringList others() const;

protected:
  bool eventFilter(QObject *object, QEvent *event) override;
  void accept() override;
  void done(int result) override;

private:
  void search();
  void listed(const QJsonObject &payload, bool first);
  void failed(const QString &message, bool first);
  void preview();
  /// Where the operand's own access is among the accesses judged.
  int ownAccess() const;

  Session &session_;
  QJsonObject query_;
  QLabel *subject_ = nullptr;
  QLineEdit *search_ = nullptr;
  QListWidget *list_ = nullptr;
  QLabel *member_ = nullptr;
  QLabel *memberType_ = nullptr;
  QLabel *expression_ = nullptr;
  QLabel *note_ = nullptr;
  QLabel *evidenceTitle_ = nullptr;
  QTreeWidget *evidence_ = nullptr;
  QCheckBox *every_ = nullptr;
  QLabel *summary_ = nullptr;
  QPushButton *apply_ = nullptr;
  QTimer debounce_;
  quint64 serial_ = 0;
  /// What every search judges once the first answer came: the accesses,
  /// and the pointer's own with the instructions they are at.
  QJsonArray accesses_;
  QJsonObject target_;
  bool answered_ = false;
  QString type_;
  qint64 delta_ = 0;
  std::optional<int> operand_;
};

} // namespace neverd::gui
