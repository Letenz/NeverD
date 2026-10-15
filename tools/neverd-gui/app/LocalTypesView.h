#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QStackedWidget;
class QStandardItemModel;
class QTabBar;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

namespace neverd::gui {

class Session;
class TypeSourceHighlighter;

/// A command of IDA's Local Types window, by IDA's key (IdaActions.def).
enum class TypesCommand {
#define NEVERD_IDA_TYPES_COMMAND(IdaAction, Shortcut, QtShortcut, Command)     \
  Command,
#include "IdaActions.def"
};

/// IDA's Local Types window: every named type of the program, filtered by
/// name, and the selected one laid out member by member -- offset, size,
/// bit range and the padding between, each with a bar of where it lies in
/// the type -- or as the C, C++, Rust or Go that declares it, to copy or
/// export.  A member's type, or a type named in the source, opens on a
/// double click; Back returns to the type before.  Types are added and
/// edited as C, C++, Rust or Go declarations, and deleted, by IDA's keys.
class LocalTypesView final : public QWidget {
  Q_OBJECT
public:
  explicit LocalTypesView(Session &session, QWidget *parent = nullptr);
  void focusFilter();
  /// Shows the type named \p name, as following a member's type does.
  void showType(const QString &name);
  /// The type shown, empty for none.
  QString currentType() const { return current_; }
  /// The tab shown: 0 the layout, then C, C++, Rust and Go.
  int page() const;
  void setPage(int page);
  /// Add opens the editor empty, Edit with the type shown declared in the
  /// language shown (C from the layout), and Delete asks before removing
  /// the selected types.
  void run(TypesCommand command);
  static QKeySequence commandKey(TypesCommand command);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void reload();
  void listed(const QJsonObject &payload);
  void selectionChanged();
  void requestLayout();
  void requestSource();
  void laidOut(const QJsonObject &type);
  void addMembers(QTreeWidgetItem *parent, const QJsonArray &members);
  void exportTypes(const QStringList &names);
  void copy();
  void back();
  void open(const QString &name, bool remember);
  QString language() const;
  QString spelling() const;
  QString offsetText(qint64 offset, qint64 bit = -1) const;
  QStringList selectedNames() const;
  void updateActions();
  std::optional<TypesCommand> commandFor(const QKeySequence &key) const;
  void openEditor(const QString &language, const QString &source,
                  const QString &title);
  void deleteTypes(const QStringList &names);

  Session &session_;
  // Null until the constructor makes them: the event filter sees events of
  // the first ones before the last exist.
  QLineEdit *filter_ = nullptr;
  QLabel *summary_ = nullptr;
  QStandardItemModel *model_ = nullptr;
  QSortFilterProxyModel *proxy_ = nullptr;
  QTreeView *list_ = nullptr;
  QToolButton *back_ = nullptr;
  QLabel *heading_ = nullptr;
  QTabBar *tabs_ = nullptr;
  QStackedWidget *pages_ = nullptr;
  QTreeWidget *layout_ = nullptr;
  QPlainTextEdit *source_ = nullptr;
  TypeSourceHighlighter *highlighter_ = nullptr;
  QComboBox *spelling_ = nullptr;
  QCheckBox *hex_ = nullptr, *dependencies_ = nullptr, *asserts_ = nullptr;
  QToolButton *copy_ = nullptr, *export_ = nullptr;
  QToolButton *add_ = nullptr, *edit_ = nullptr, *delete_ = nullptr;
  QString current_;
  QJsonObject type_;
  QStringList history_;
  QSet<QString> names_;
  /// Serials of the last list, layout and source requests; older replies
  /// are dropped.
  quint64 listSerial_ = 0, layoutSerial_ = 0, sourceSerial_ = 0;
  /// Whether the next type shown puts the one before on the history.
  bool rememberNext_ = true;
  /// The type the next list shows, one just declared; and what the summary
  /// says of the last change.
  QString showNext_, note_;
};

} // namespace neverd::gui
