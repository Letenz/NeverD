#pragma once

#include "ActionRegistry.h"
#include "Address.h"
#include "AddressSpace.h"
#include "ChooserView.h"

#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/MainWindow.h>
#include <optional>

class QLabel;
class McpConnectionManager;
class GuiSessionBroker;

namespace neverd::gui {

class CodeView;
class DisassemblyView;
class ExtensionsView;
class GraphOverview;
class HexView;
class NavigationBand;
class OutputWindow;
class Session;

/// The workbench window: the classic disassembler layout (functions on the
/// left, disassembly with hex, imports and exports in the center, output at
/// the bottom, navigation band above), its menus, toolbars and status bar.
class MainWindow final : public KDDockWidgets::QtWidgets::MainWindow {
  Q_OBJECT
public:
  MainWindow(Session &session, McpConnectionManager &mcp,
             GuiSessionBroker &broker);
  ~MainWindow() override;

  void openFile(const QString &path);
  /// Use a disposable layout file (tests and benchmarks).
  void setLayoutPath(const QString &path) { layoutPath_ = path; }
  /// Restore the saved desktop, or lay out the default one.  Call once the
  /// window has its size so preferred dock sizes apply.
  void initializeLayout();
  void showQuickStart();
  /// Command lines to run in the output window once the next file opens.
  void runAfterOpen(const QStringList &commands) {
    pendingCommands_ = commands;
  }
  DisassemblyView *disassembly() const { return disassembly_; }
  ActionRegistry &actions() { return actions_; }

  /// The window that creates on-demand docks while a layout is restored.
  static MainWindow *instance() { return instance_; }

signals:
  /// The disassembly painted its first content after a file opened.
  void firstContentPainted();

protected:
  void closeEvent(QCloseEvent *event) override;
  void changeEvent(QEvent *event) override;
  void showEvent(QShowEvent *event) override;
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  using Dock = KDDockWidgets::QtWidgets::DockWidget;
  static KDDockWidgets::Core::DockWidget *createDock(const QString &name);
  Dock *dockNamed(const QString &name);
  void retranslateUi();
  void openDock(Dock *dock, bool tabbed);
  void showConnections();

  void buildMenusAndToolbars();
  void buildStatusBar();
  void buildDocks();
  void applyDefaultLayout();
  /// Give the default side and bottom windows their classic proportions.
  void applyDefaultSizes();
  void showOverview();
  void connectSession();
  void connectActions();
  /// \p title is an untranslated MainWindow string, or null when the
  /// caller titles the dock itself.
  Dock *makeDock(const QString &id, const char *title, const QString &icon,
                 QWidget *content);
  Dock *chooserDock(ChooserKind kind);
  CodeView *codeView(const QString &representation);

  // Location and synchronization.
  std::optional<Address> currentAddress() const;
  std::optional<Address> currentFunction() const;
  void navigate(Address address);
  /// Jump in the active address view: the hex view when it was the last
  /// analysis view used, otherwise the disassembly.
  void jump(Address address);
  void navigateExpression(const QString &text);
  void synchronize(Address address, QObject *source);
  void updateActions();
  void updateStatusBar();
  void updateTitle();

  // Commands.
  void openDialog();
  void rename();
  void comment();
  void jumpAnywhere();
  void showCrossReferences(std::optional<Address> address, bool to);
  void showPseudocode(const QString &representation);
  void toggleGraph();
  void searchBinary(const QString &kind, bool again);
  void searchHighlight(bool forward);
  void stepFunction(bool forward);
  void showCalculator();
  void showAbout();
  void showOptions();
  void chooseFont();
  void chooseTheme();
  void showShortcuts();
  void showCommandPalette();
  void exportCurrent(bool pseudocode);
  void runCommand(const QString &command, const QString &argument);
  void saveDesktop();
  void restoreDesktop();
  void resetDesktop();
  void cycleWindows(bool forward);
  void fillPlaceholder(QMenu *menu, const QString &name);
  void contextMenu(const QPoint &globalPosition);
  QString layoutPath() const;
  // Project state packed into the database.
  QHash<QString, QByteArray> projectState() const;
  void restoreProjectState();
  QJsonArray bookmarks() const;
  void setBookmarks(const QJsonArray &rows);
  QString bookmarksKey() const;

  Session &session_;
  McpConnectionManager &mcp_;
  GuiSessionBroker &broker_;
  ActionRegistry actions_;
  AddressSpace space_;
  NavigationBand *navigationBand_ = nullptr;
  DisassemblyView *disassembly_ = nullptr;
  HexView *hex_ = nullptr;
  OutputWindow *output_ = nullptr;
  GraphOverview *overview_ = nullptr;
  ExtensionsView *extensions_ = nullptr;
  ChooserView *functions_ = nullptr;
  QHash<QString, Dock *> docks_;
  QHash<int, ChooserView *> choosers_;
  QPointer<CodeView> pseudocode_;
  QLabel *analysisLabel_ = nullptr, *directionLabel_ = nullptr,
         *diskLabel_ = nullptr, *fileLabel_ = nullptr;
  QMenu *recentMenu_ = nullptr;
  QMenu *windowsMenu_ = nullptr;
  QString layoutPath_;
  QString lastTextSearch_, lastByteSearch_;
  QString lastPaletteCommand_;
  bool searchDown_ = true;
  bool synchronizing_ = false;
  bool hexActive_ = false;
  bool quitting_ = false;
  bool defaultSizesPending_ = false;
  QTimer statusTimer_;
  QStringList pendingCommands_;
  QHash<QString, const char *> dockTitles_;
  static inline MainWindow *instance_ = nullptr;
};

} // namespace neverd::gui
