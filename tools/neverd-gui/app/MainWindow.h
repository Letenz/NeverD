#pragma once

#include "ActionRegistry.h"
#include "Address.h"
#include "AddressSpace.h"
#include "ChooserView.h"
#include "Session.h"

#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/MainWindow.h>
#include <optional>
#include <utility>

class QDialog;
class QLabel;
class QMenuBar;
class McpConnectionManager;
class GuiSessionBroker;

namespace neverd::gui {

namespace web {
class PackageReviewDialog;
}

class CodeView;
class DisassemblyView;
class ExtensionsView;
class GraphOverview;
class HexView;
class NavigationBand;
class MicrosoftSymbols;
class OutputWindow;
class Session;
class TitleBar;
class WindowFrame;

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
  /// Whether opening a PE asks to load the PDB it names, before the load or
  /// once it lacks one; automated runs, which nobody answers, load the PDB
  /// discovery finds.
  void setAsksForPdb(bool asks);
  /// Use a disposable layout file (tests and benchmarks).
  void setLayoutPath(const QString &path) { layoutPath_ = path; }
  /// Restore the saved desktop, or lay out the default one; later calls do
  /// nothing.  Call once the window has its size so preferred dock sizes
  /// apply.
  void initializeLayout();
  /// Show the workbench, laying out its desktop first if nothing did yet.
  void showWorkbench();
  /// Before the workbench shows, the quick start greets alone and the
  /// workbench follows the user's choice; closing it quits.
  void showQuickStart();
  /// Greet an empty startup on the next event-loop turn, unless opening a
  /// file takes precedence. No window-exposure or engine-ready event is needed.
  void scheduleQuickStart();
  /// Command lines to run in the output window once the next file opens.
  void runAfterOpen(const QStringList &commands) {
    pendingCommands_ = commands;
  }
  DisassemblyView *disassembly() const { return disassembly_; }
  ActionRegistry &actions() { return actions_; }
  /// The window's menus, which sit in its title bar when it draws its own
  /// frame.  This hides QMainWindow::menuBar(), which would replace that
  /// title bar with a new menu bar.
  QMenuBar *menuBar() const { return menuBar_; }

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
  /// Title the pseudocode and IR windows by what they show, lettered apart.
  void retitleCodeDocks();

  // Location and synchronization.
  std::optional<Address> currentAddress() const;
  /// The list window holding the keyboard focus, if one does.
  ChooserView *focusedChooser() const;
  /// The pseudocode or IR window holding the keyboard focus, if one does.
  CodeView *focusedCodeView() const;
  std::optional<Address> currentFunction() const;
  /// Open a function in the code window used before the chooser took focus.
  void activateFunction(Address address);
  void navigate(Address address);
  /// Jump in the active address view: the hex view when it was the last
  /// analysis view used, otherwise the disassembly.
  void jump(Address address);
  void navigateExpression(const QString &text);
  /// Follow a code name in \p view, optionally selecting its instruction.
  void activateCodeName(CodeView *view, const QString &name,
                        bool selectSourceAddress = false);
  void navigateCodeFunction(CodeView *view, Address address, Address function,
                            std::optional<Address> from);
  void navigateHistory(bool forward);
  void synchronize(Address address, QObject *source);
  void updateActions();
  void updateStatusBar();
  void updateTitle();

  // Commands.
  void openDialog();
  /// The binary or database to open, asked in a dialog over \p parent;
  /// empty when the user cancels.
  QString askForFile(QWidget *parent) const;
  /// Open \p path, first asking how to load it when NeverD keeps no project
  /// for it, as IDA's "Load a new file" dialog does.
  /// Open \p path once the user answered whether to load the PDB it names,
  /// as IDA asks when it loads a PE that names one.
  void openAskingForPdb(const QString &path, LoadOptions options);
  /// Show the workbench and start loading \p path.
  void openNow(const QString &path, const LoadOptions &options);
  void chooseLoader(const QString &path);
  /// File, Load file, Reload the input file: read the file again, asking
  /// again how to read a binary file.
  void reloadInput();
  /// Show or hide the status line's analysis indicator, as the load dialog
  /// last chose.
  void applyIndicator();
  void rename();
  void comment();
  void jumpAnywhere();
  void showCrossReferences(std::optional<Address> address, bool to);
  void showLocalReferences(CodeView *origin, const QJsonObject &target);
  void showReferencesDialog(std::optional<Address> address, bool to,
                            CodeView *origin);
  void showPseudocode(const QString &representation,
                      bool selectAssemblyAddress = false);
  void toggleGraph();
  void searchBinary(const QString &kind, bool again);
  void searchHighlight(bool forward);
  void stepFunction(bool forward);
  void showCalculator();
  void showAbout();
  void showOptions();
  /// Options -> String literals: the engine's encodings and the minimum
  /// length, applied to the listing and the Strings window.
  void showStringOptions();
  void stringOptionsDialog(const QJsonArray &encodings,
                           const QJsonObject &current);
  void chooseFont();
  void chooseTheme();
  void showShortcuts();
  /// Edit > Copy: the selection of the view holding the focus.
  void copySelection();
  void showCommandPalette();
  void exportCurrent(bool pseudocode);
  void loadPdb();
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
  void navigateInitialAddress();
  QJsonArray bookmarks() const;
  void setBookmarks(const QJsonArray &rows);
  QString bookmarksKey() const;

  Session &session_;
  McpConnectionManager &mcp_;
  GuiSessionBroker &broker_;
  ActionRegistry actions_;
  AddressSpace space_;
  NavigationBand *navigationBand_ = nullptr;
  /// Null where the window keeps the window manager's frame.
  WindowFrame *frame_ = nullptr;
  TitleBar *titleBar_ = nullptr;
  QMenuBar *menuBar_ = nullptr;
  DisassemblyView *disassembly_ = nullptr;
  HexView *hex_ = nullptr;
  OutputWindow *output_ = nullptr;
  MicrosoftSymbols *microsoftSymbols_ = nullptr;
  /// A file to open once the worker has started, asking for its PDB then.
  std::optional<std::pair<QString, LoadOptions>> pdbPromptPending_;
  bool asksForPdb_ = true;
  GraphOverview *overview_ = nullptr;
  ExtensionsView *extensions_ = nullptr;
  ChooserView *functions_ = nullptr;
  QHash<QString, Dock *> docks_;
  QHash<int, ChooserView *> choosers_;
  QPointer<CodeView> pseudocode_;
  QPointer<CodeView> lastCodeView_;
  /// Tab returns to the last code window, even after assembly takes focus.
  QPointer<CodeView> tabCodeView_;
  std::optional<Address> initialAddress_;
  bool restoreGraph_ = false;
  QPointer<QDialog> quickStart_;
  QPointer<web::PackageReviewDialog> packageReview_;
  bool startupQuickStartPending_ = false;
  QLabel *analysisLabel_ = nullptr, *directionLabel_ = nullptr,
         *diskLabel_ = nullptr, *fileLabel_ = nullptr;
  QMenu *recentMenu_ = nullptr;
  QMenu *windowsMenu_ = nullptr;
  QString layoutPath_;
  QString lastTextSearch_, lastByteSearch_;
  QString lastPaletteCommand_;
  bool searchDown_ = true;
  quint64 codeNavigationSerial_ = 0;
  bool codeHistoryNavigation_ = false;
  bool hexActive_ = false;
  bool quitting_ = false;
  bool layoutInitialized_ = false;
  bool defaultSizesPending_ = false;
  QTimer statusTimer_;
  QStringList pendingCommands_;
  /// The code view asked for while a jump was still loading.
  std::optional<std::pair<QString, bool>> pseudocodeAfterJump_;
  QHash<QString, const char *> dockTitles_;
  static inline MainWindow *instance_ = nullptr;
};

} // namespace neverd::gui
