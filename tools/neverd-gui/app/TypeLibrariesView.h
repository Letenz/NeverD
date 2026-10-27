//===- TypeLibrariesView.h - IDA's type libraries window --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IDA's Type libraries (Shift+F11): the libraries of a system's headers
/// NeverD ships whose types and prototypes join the program's, each with what
/// it joined and the names it left to a library before it.  Ins adds another
/// library whose records lay out as the program's, Del leaves the selected
/// one out; the project keeps the choice (libraries.json), and the program's
/// types are read anew.
//===----------------------------------------------------------------------===//

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class QLabel;
class QToolButton;
class QTreeWidget;

namespace neverd::gui {

class Session;

class TypeLibrariesView final : public QWidget {
  Q_OBJECT
public:
  explicit TypeLibrariesView(Session &session, QWidget *parent = nullptr);

  QTreeWidget *table() const { return table_; }
  /// Lets the user choose another library whose records lay out as the
  /// program's, which then joins its types (Ins).
  void addLibrary();
  /// Leaves the selected library out of the program's types (Del).
  void removeLibrary();

private:
  void reload();
  void listed(const QJsonObject &payload);
  void failed(const QString &message);
  /// Sets the whole choice: the libraries joined beyond the image's own and
  /// the image's left out.
  void choose(const QStringList &added, const QStringList &removed);
  void updateButtons();

  Session &session_;
  QTreeWidget *table_;
  QLabel *status_;
  QToolButton *add_, *remove_;
  /// Every library type_libraries lists, and the choice it holds.
  QJsonArray libraries_;
  QStringList added_, removed_;
};

} // namespace neverd::gui
