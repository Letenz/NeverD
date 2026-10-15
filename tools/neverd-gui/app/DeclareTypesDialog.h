//===- DeclareTypesDialog.h - Declare or edit local types ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The Local Types window's editor, as IDA's "Add type" and "Edit type":
/// declarations in C, C++, Rust or Go that the engine reads into the
/// program's local types, a type of a name the program has replacing it.
/// A declaration that does not read stays in the editor with its problems,
/// each at its line and column.
//===----------------------------------------------------------------------===//

#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <functional>

class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace neverd::gui {

class TypeSourceHighlighter;

class DeclareTypesDialog final : public QDialog {
  Q_OBJECT
public:
  /// Sends \p source in \p language and calls \p answered with the reply:
  /// the types declared, or the problems.
  using Submit = std::function<void(
      const QString &language, const QString &source,
      std::function<void(const QJsonObject &reply)> answered)>;

  /// \p language is c, cpp, rust or go; \p source what the editor starts
  /// with, a type's declaration to edit it.
  DeclareTypesDialog(const QString &language, const QString &source,
                     Submit submit, QWidget *parent = nullptr);

  QString language() const;
  QString source() const;
  /// The types the accepted declaration defined.
  const QStringList &declared() const { return declared_; }
  void setTypeNames(const QSet<QString> &names);
  /// A declaration on its way stays: the dialog closes on its answer.
  void reject() override;

private:
  void declare();
  void showProblems(const QJsonArray &problems);

  QComboBox *language_;
  QPlainTextEdit *editor_;
  QListWidget *problems_;
  QPushButton *declare_;
  TypeSourceHighlighter *highlighter_;
  Submit submit_;
  QStringList declared_;
  bool waiting_ = false;
};

} // namespace neverd::gui
