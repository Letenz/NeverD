// Workbench controller tests against the fixture worker: the session, the
// production main window and its views, edits and session transitions.
#include "ChooserView.h"
#include "CodeView.h"
#include "DisassemblyView.h"
#include "GraphView.h"
#include "HexView.h"
#include "ListingView.h"
#include "MainWindow.h"
#include "OutputWindow.h"
#include "ProjectDatabase.h"
#include "Session.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <memory>

using namespace neverd::gui;

namespace {
constexpr Address Base = 0xffff800012340000ULL;
constexpr int OpenTimeoutMs = 7000;

QString writeFixture(const QTemporaryDir &directory, const QString &name) {
  const auto path = directory.filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write("fixture") != 7)
    return {};
  return path;
}

/// The production window over the fixture worker.
struct Workbench {
  Session session{QString::fromLocal8Bit(TEST_WORKER)};
  McpConnectionManager mcp;
  GuiSessionBroker broker;
  QTemporaryDir layout;
  std::unique_ptr<MainWindow> window;

  Workbench() {
    window = std::make_unique<MainWindow>(session, mcp, broker);
    window->setLayoutPath(layout.filePath(QStringLiteral("layout.json")));
    window->resize(1400, 900);
    window->initializeLayout();
    window->show();
  }
  ChooserView *functions() const {
    auto *table =
        window->findChild<QTreeView *>(QStringLiteral("functionsList"));
    return table ? qobject_cast<ChooserView *>(table->parentWidget()) : nullptr;
  }
  QAction *action(ActionId id) const { return window->actions().action(id); }
  QString dockTitle(const QString &uniqueName) const {
    for (auto *dock :
         window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == uniqueName)
        return dock->title();
    return {};
  }
  CodeView *codeView(const QString &representation) const {
    for (auto *view : window->findChildren<CodeView *>())
      if (view->representation() == representation)
        return view;
    return nullptr;
  }
};

QByteArray readAll(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool dropFile(QWidget *target, const QString &path) {
  QMimeData mime;
  mime.setUrls({QUrl::fromLocalFile(path)});
  QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction | Qt::MoveAction, &mime,
                        Qt::LeftButton, Qt::ShiftModifier);
  QApplication::sendEvent(target, &enter);
  if (!enter.isAccepted() || enter.dropAction() != Qt::CopyAction)
    return false;
  QDragMoveEvent move(QPoint(10, 10), Qt::CopyAction | Qt::MoveAction, &mime,
                      Qt::LeftButton, Qt::ShiftModifier);
  QApplication::sendEvent(target, &move);
  if (!move.isAccepted() || move.dropAction() != Qt::CopyAction)
    return false;
  QDropEvent drop(QPointF(10, 10), Qt::CopyAction | Qt::MoveAction, &mime,
                  Qt::LeftButton, Qt::ShiftModifier);
  QApplication::sendEvent(target, &drop);
  return drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
}
} // namespace

class WorkbenchTests : public QObject {
  Q_OBJECT
  QTemporaryDir settingsDirectory_;
private slots:
  void unknownEntryBrowsesMappedCodeAndShowsDiagnostics() {
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("unknown-entry.bin"));
    Workbench bench;
    QSignalSpy messages(&bench.session, &Session::message);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(bench.session.entryAddress(), Address(0));
    QTRY_VERIFY_WITH_TIMEOUT(
        bench.window->disassembly()->currentAddress().has_value(),
        OpenTimeoutMs);
    QVERIFY(*bench.window->disassembly()->currentAddress() >= Base);
    QVERIFY(*bench.window->disassembly()->currentAddress() < Base + 0x3000);
    bool warned = false;
    for (const auto &message : messages)
      warned |= message.first().toString() ==
                QLatin1String("Fixture PE entry is unknown.");
    QVERIFY(warned);
    QCOMPARE(bench.session.entryAddress(), Address(0));
  }

  void fileDropOpensFromWorkbenchViews_data() {
    QTest::addColumn<QString>("targetName");
    for (const auto *name : {"window", "listing", "functions", "hex", "code",
                             "output", "command", "floating"})
      QTest::newRow(name) << QString::fromLatin1(name);
  }

  void fileDropOpensFromWorkbenchViews() {
    QFETCH(QString, targetName);
    QTemporaryDir directory;
    const auto path = writeFixture(
        directory, QString::fromUtf8("\u4e2d\u6587 space #% fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    QWidget *target = bench.window.get();
    if (targetName == QLatin1String("listing"))
      target = bench.window->disassembly()->listing()->viewport();
    else if (targetName == QLatin1String("functions"))
      target = bench.functions()->table()->viewport();
    else if (targetName == QLatin1String("hex"))
      target = bench.window->findChild<HexView *>()->viewport();
    else if (targetName == QLatin1String("code")) {
      // Code docks created after startup must accept file drops too.
      bench.window->openFile(writeFixture(directory, "first.bin"));
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->disassembly()->navigate(Base + 0x140);
      QTRY_VERIFY_WITH_TIMEOUT(
          bench.window->disassembly()->currentFunction().has_value(),
          OpenTimeoutMs);
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *view = bench.codeView(QStringLiteral("c"));
      QVERIFY(view);
      target = view->text()->viewport();
    } else if (targetName == QLatin1String("output"))
      target = bench.window->findChild<OutputWindow *>()
                   ->findChild<QPlainTextEdit *>()
                   ->viewport();
    else if (targetName == QLatin1String("command"))
      target =
          bench.window->findChild<OutputWindow *>()->findChild<QLineEdit *>();
    else if (targetName == QLatin1String("floating")) {
      auto *dock = qobject_cast<KDDockWidgets::QtWidgets::DockWidget *>(
          bench.functions()->parentWidget());
      QVERIFY(dock);
      dock->setFloating(true);
      target = bench.functions()->table()->viewport();
      QVERIFY(target->window() != bench.window.get());
    }
    QVERIFY(target);
    QVERIFY(dropFile(target, path));
    QTRY_COMPARE_WITH_TIMEOUT(bench.session.filePath(), path, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(QFileInfo::exists(path));
    if (targetName == QLatin1String("command"))
      QVERIFY(static_cast<QLineEdit *>(target)->text().isEmpty());
  }

  void fileDropRejectsInvalidInputs() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    const QList<QList<QUrl>> rejected{
        {},
        {QUrl(QStringLiteral("https://example.com/fixture.bin"))},
        {QUrl::fromLocalFile(directory.path())},
        {QUrl::fromLocalFile(directory.filePath("missing.bin"))},
        {QUrl::fromLocalFile(path), QUrl::fromLocalFile(path)},
        {QUrl(QStringLiteral("file:relative.bin"))}};
    for (const auto &urls : rejected) {
      QMimeData mime;
      mime.setUrls(urls);
      QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime,
                            Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(bench.window.get(), &enter);
      QVERIFY(!enter.isAccepted());
    }
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    QDragEnterEvent moveOnly(QPoint(10, 10), Qt::MoveAction, &mime,
                             Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(bench.window.get(), &moveOnly);
    QVERIFY(!moveOnly.isAccepted());
    QMimeData text;
    text.setText(path);
    QDragEnterEvent plainText(QPoint(10, 10), Qt::CopyAction, &text,
                              Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(bench.window.get(), &plainText);
    QVERIFY(!plainText.isAccepted());
    QVERIFY(bench.session.filePath().isEmpty());
  }

  void fileDropOpensFromQuickStart() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    QTimer drag;
    bool accepted = false;
    connect(&drag, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog || dialog->objectName() != QLatin1String("quickStartDialog"))
        return;
      drag.stop();
      accepted = dropFile(dialog->findChild<QListWidget *>(), path);
      if (!accepted)
        dialog->reject();
    });
    drag.start(10);
    bench.window->showQuickStart();
    QVERIFY(accepted);
    QTRY_COMPARE_WITH_TIMEOUT(bench.session.filePath(), path, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!QApplication::activeModalWidget());
  }

  void fileDropKeepsUnsavedChangesWhenCancelled() {
    QTemporaryDir directory;
    const auto first = writeFixture(directory, QStringLiteral("first.bin"));
    const auto second = writeFixture(directory, QStringLiteral("second.bin"));
    Workbench bench;
    bench.window->openFile(first);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.session.setComment(Base, QStringLiteral("unsaved comment"));
    QTRY_VERIFY(bench.session.dirty());
    QTimer cancel;
    bool prompted = false;
    connect(&cancel, &QTimer::timeout, this, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        prompted = true;
        box->reject();
      }
    });
    cancel.start(10);
    QVERIFY(dropFile(bench.window.get(), second));
    QTRY_VERIFY(prompted);
    QCOMPARE(bench.session.filePath(), first);
    QVERIFY(bench.session.loaded());
    QVERIFY(bench.session.dirty());
  }

  void initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("WorkbenchTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory_.path());
  }

  void hexViewShownBeforeOpeningLoadsItsBytes() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    // A restored desktop can show the hex view before the regions arrive.
    for (auto *dock :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == QLatin1String("hex-1"))
        dock->raise();
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_VERIFY(hex->isVisible());
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(hex->byteAt(Base).has_value(), OpenTimeoutMs);
    QVERIFY(hex->isVisible());

    // Shift and the arrows select bytes, which copy as hex text.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    hex->setCurrent(Base);
    hex->setFocus();
    QTRY_VERIFY(hex->hasFocus());
    for (int i = 0; i < 3; ++i)
      QTest::keyClick(hex, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(hex->selection(),
             (std::optional<std::pair<Address, Address>>({Base, Base + 3})));
    QStringList bytes;
    for (Address at = Base; at <= Base + 3; ++at) {
      QTRY_VERIFY(hex->byteAt(at).has_value());
      bytes.append(QStringLiteral("%1")
                       .arg(*hex->byteAt(at), 2, 16, QLatin1Char('0'))
                       .toUpper());
    }
    QApplication::clipboard()->clear();
    QTest::keyClick(hex, Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE(QApplication::clipboard()->text(), bytes.join(' '));
    QApplication::clipboard()->clear();
    bench.action(ActionId::EditCopy)->trigger();
    QTRY_COMPARE(QApplication::clipboard()->text(), bytes.join(' '));
    // A move without Shift ends the selection.
    QTest::keyClick(hex, Qt::Key_Left);
    QVERIFY(!hex->selection());
  }

  void stringReferencesAndHexTextEncodings() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);

    // The reference search lists instructions with the strings they use and
    // opens ready for a query.
    bench.action(ActionId::SearchStringReferences)->trigger();
    ChooserView *references = nullptr;
    for (auto *view : bench.window->findChildren<ChooserView *>())
      if (view->model().kind() == ChooserKind::StringReferences)
        references = view;
    QVERIFY(references);
    QTRY_VERIFY(references->findChild<QLineEdit *>()->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(references->model().total(), 2, OpenTimeoutMs);
    QTRY_COMPARE(references->model().rowObject(1).value("text").toString(),
                 QStringLiteral("Wide"));
    QCOMPARE(references->model().addressAt(1),
             std::optional<Address>(Base + 0x73));
    // Its cross references are those of the string, not the instruction.
    QCOMPARE(references->model().referenceAddressAt(1),
             std::optional<Address>(Base + 0x3108));
    references->setFilterText(QString::fromUtf8("\u6587"));
    QTRY_COMPARE(references->model().total(), 1);

    // In the Strings list, Ctrl+X lists the selected string's references.
    bench.action(ActionId::ViewStrings)->trigger();
    ChooserView *strings = nullptr;
    for (auto *view : bench.window->findChildren<ChooserView *>())
      if (view->model().kind() == ChooserKind::Strings)
        strings = view;
    QVERIFY(strings);
    QTRY_COMPARE_WITH_TIMEOUT(strings->model().total(), 2, OpenTimeoutMs);
    QTRY_VERIFY(strings->model().addressAt(1).has_value());
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    strings->table()->setFocus();
    strings->table()->setCurrentIndex(strings->model().index(1, 0));
    QTRY_VERIFY(strings->table()->hasFocus());
    QString title;
    QTimer::singleShot(0, [&title] {
      if (auto *dialog = QApplication::activeModalWidget()) {
        title = dialog->windowTitle();
        dialog->close();
      }
    });
    QTest::keyClick(strings->table(), Qt::Key_X, Qt::ControlModifier);
    QTRY_VERIFY(!title.isEmpty());
    QVERIFY2(
        title.contains(QStringLiteral("FFFF800012343108"), Qt::CaseInsensitive),
        qPrintable(title));

    // The hex view's text column reads the bytes in a chosen encoding.
    for (auto *dock :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == QLatin1String("hex-1"))
        dock->raise();
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_VERIFY(hex->isVisible());
    hex->setCurrent(Base + 0x3108);
    QTRY_COMPARE_WITH_TIMEOUT(hex->textAt(Base + 0x3109),
                              std::optional<QString>(QStringLiteral(".")),
                              OpenTimeoutMs);
    hex->setTextEncoding(QStringLiteral("utf-16le"));
    QTRY_COMPARE_WITH_TIMEOUT(hex->textAt(Base + 0x3108),
                              std::optional<QString>(QStringLiteral("W")),
                              OpenTimeoutMs);
    QCOMPARE(hex->textAt(Base + 0x3109), std::optional<QString>(QString()));
    QCOMPARE(QSettings().value(QStringLiteral("hex/textEncoding")).toString(),
             QStringLiteral("utf-16le"));
    // An encoding the engine does not know falls back to ASCII.
    hex->setTextEncoding(QStringLiteral("klingon"));
    QTRY_COMPARE_WITH_TIMEOUT(hex->textEncoding(), QString(), OpenTimeoutMs);
    QVERIFY(!QSettings().contains(QStringLiteral("hex/textEncoding")));
  }

  void viewsFollowTheSessionAndNavigation() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentItem(),
                              std::optional<Address>(Base), OpenTimeoutMs);

    // The function window pages rows from the worker.
    auto *functions = bench.functions();
    QVERIFY(functions);
    QTRY_COMPARE_WITH_TIMEOUT(functions->model().total(), 600, OpenTimeoutMs);
    QTRY_COMPARE(functions->model().rowObject(0).value("name").toString(),
                 QStringLiteral("function_0"));
    QCOMPARE(functions->model().addressAt(0), std::optional<Address>(Base));
    functions->setFilterText(QStringLiteral("function_599"));
    QTRY_COMPARE(functions->model().total(), 1);
    QTRY_COMPARE(functions->model().rowObject(0).value("name").toString(),
                 QStringLiteral("function_599"));
    QCOMPARE(functions->model().addressAt(0),
             std::optional<Address>(Base + 0x2570));
    functions->setFilterText({});
    QTRY_COMPARE(functions->model().total(), 600);
    // Rows that arrive before the regions name their segments once the
    // regions do: only the Segment column repaints.
    QTRY_VERIFY(functions->model().rowCount() > 0);
    QSignalSpy repaint(&functions->model(), &QAbstractItemModel::dataChanged);
    functions->model().addressSpaceChanged();
    QCOMPARE(repaint.size(), 1);
    QCOMPARE(repaint.first().at(0).toModelIndex().column(), 1);
    QCOMPARE(repaint.first().at(1).toModelIndex().column(), 1);

    // Navigation records history and synchronizes the hex view.
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_COMPARE(hex->currentAddress(), std::optional<Address>(Base + 0x140));
    QVERIFY(disassembly->canGoBack());
    disassembly->goBack();
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(Base));
    QVERIFY(disassembly->canGoForward());
    disassembly->goForward();
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));

    // Pseudocode pages every line of the function.
    bench.action(ActionId::ViewPseudocode)->trigger();
    CodeView *pseudocode = nullptr;
    QTRY_VERIFY((pseudocode = bench.codeView(QStringLiteral("c"))) != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(
        pseudocode->text()->allText().contains(QStringLiteral("code line 699")),
        OpenTimeoutMs);
    QCOMPARE(pseudocode->text()->function(),
             std::optional<Address>(Base + 0x140));

    // IR rows mapped to instructions move the disassembly cursor.
    bench.action(ActionId::ViewLowIR)->trigger();
    CodeView *ir = nullptr;
    QTRY_VERIFY((ir = bench.codeView(QStringLiteral("low"))) != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(ir->text()->lineCount() > 3, OpenTimeoutMs);
    ir->text()->setCursorLine(3);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x143));

    // Copy takes the lines selected in the window holding the focus, also in
    // an IR window switched to pseudocode, which is not the pseudocode
    // window; it used to copy the disassembly.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    ir->setRepresentation(QStringLiteral("c"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !ir->text()->loading() && ir->text()->lineCount() > 3, OpenTimeoutMs);
    // Two windows showing pseudocode are lettered apart.
    QCOMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
             QStringLiteral("Pseudocode-A"));
    QCOMPARE(bench.dockTitle(QStringLiteral("ir-a")),
             QStringLiteral("Pseudocode-B"));
    ir->text()->setFocus();
    QTRY_VERIFY(ir->text()->hasFocus());
    ir->text()->setCursorLine(1);
    QTest::keyClick(ir->text(), Qt::Key_Down, Qt::ShiftModifier);
    const QString lines = QStringLiteral("// code line 1\n// code line 2");
    QApplication::clipboard()->clear();
    QTest::keyClick(ir->text(), Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(), lines);
    QApplication::clipboard()->clear();
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(QApplication::clipboard()->text(), lines);

    // C opens at the definition: the includes and declarations before it fold
    // into one line, which Keypad + expands and Keypad - folds again, and
    // which copies as the lines it stands for.
    pseudocode->setRepresentation(QStringLiteral("llvmc"));
    QCOMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
             QStringLiteral("LLVM C-A"));
    QCOMPARE(bench.dockTitle(QStringLiteral("ir-a")),
             QStringLiteral("Pseudocode-A"));
    auto *code = pseudocode->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() == 702,
                             OpenTimeoutMs);
    QVERIFY(code->allText().startsWith(QStringLiteral("#include <stdint.h>")));
    QCOMPARE(code->foldableCount(), 0);
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    code->setCursorLine(0);
    QApplication::clipboard()->clear();
    QTest::keyClick(code, Qt::Key_C, Qt::ControlModifier);
    const QString declaration =
        QStringLiteral("typedef struct QDomNode QDomNode;");
    const QString linked = QStringLiteral(
        "extern int Bar_ctor() __asm__(\"_ZN3BarC1Ev\"); /* Bar::Bar() */");
    const QString globals =
        QStringLiteral("/* neverd.image: 0x20 */\nint64_t dso_handle = 0x20;\n"
                       "extern uint64_t qword_10; /* 0x10 */\n");
    QCOMPARE(QApplication::clipboard()->text(),
             QStringLiteral("#include <stdint.h>\n") + declaration +
                 QLatin1Char('\n') + linked + QLatin1Char('\n') + globals);
    QTest::keyClick(code, Qt::Key_Plus, Qt::KeypadModifier);
    QCOMPARE(code->lineCount(), 708);
    QTest::keyClick(code, Qt::Key_Minus, Qt::KeypadModifier);
    QCOMPARE(code->lineCount(), 702);
    QVERIFY(code->preludeFolded());

    // A type the prelude declares opens at its declaration, expanding the
    // prelude; C's own types are not declarations.
    QCOMPARE(code->declarationLine(QStringLiteral("QDomNode")),
             std::optional<int>(1));
    QVERIFY(!code->declarationLine(QStringLiteral("uint64_t")));
    QVERIFY(code->goToDeclaration(QStringLiteral("QDomNode")));
    QVERIFY(!code->preludeFolded());
    QCOMPARE(code->lineCount(), 708);
    // A global goes to its address, under whatever name C gives it there.
    QCOMPARE(code->objectAddress(QStringLiteral("dso_handle")),
             std::optional<Address>(0x20));
    QCOMPARE(code->objectAddress(QStringLiteral("qword_10")),
             std::optional<Address>(0x10));
    QVERIFY(!code->objectAddress(QStringLiteral("QDomNode")));
    QVERIFY(!code->objectAddress(QStringLiteral("Bar_ctor")));
    // A C++ function links by the mangled symbol its label names, which is
    // what navigation looks up.
    QCOMPARE(code->linkedSymbol(QStringLiteral("Bar_ctor")),
             std::optional<QString>(QStringLiteral("_ZN3BarC1Ev")));
    QVERIFY(!code->linkedSymbol(QStringLiteral("QDomNode")));
    QCOMPARE(code->currentToken(), QStringLiteral("QDomNode"));
    code->setPreludeFolded(true);
    QCOMPARE(code->lineCount(), 702);

    // A list copies its selected rows, by key or from the Edit menu.
    auto &model = functions->model();
    functions->table()->setFocus();
    QTRY_VERIFY(functions->table()->hasFocus());
    functions->table()->selectionModel()->select(
        QItemSelection(model.index(0, 0),
                       model.index(1, model.columnCount() - 1)),
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QApplication::clipboard()->clear();
    QTest::keyClick(functions->table(), Qt::Key_C, Qt::ControlModifier);
    const QString rows = QApplication::clipboard()->text();
    QCOMPARE(rows.count(QLatin1Char('\n')), 1);
    QVERIFY2(rows.startsWith(QStringLiteral("function_0\t")), qPrintable(rows));
    QVERIFY2(rows.contains(QStringLiteral("\nfunction_1\t")), qPrintable(rows));
    QApplication::clipboard()->clear();
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(QApplication::clipboard()->text(), rows);

    // The graph of the current function.
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewToggleGraph)->trigger();
    QVERIFY(disassembly->graphMode());
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->graph()->nodes().size(),
                              qsizetype(2), OpenTimeoutMs);
    bench.action(ActionId::ViewToggleGraph)->trigger();
    QVERIFY(!disassembly->graphMode());

    // Unicode comments save atomically beside the binary.
    bench.session.setComment(Base + 0x140, QString::fromUtf8("中文 تعليق"));
    QTRY_VERIFY(bench.session.dirty());
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd-annotations.json"),
                             OpenTimeoutMs);
    QTRY_VERIFY(!bench.session.dirty());
    QVERIFY(QString::fromUtf8(readAll(path + ".neverd-annotations.json"))
                .contains(QString::fromUtf8("中文 تعليق")));

    // Renames reach the function window.
    bench.session.rename(Base + 0x140, QStringLiteral("renamed"));
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(20).value("name").toString(),
        QStringLiteral("renamed"), OpenTimeoutMs);
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(20).value("name").toString(),
        QStringLiteral("function_20"), OpenTimeoutMs);

    // P starts a function inside another; the cursor stays on its
    // instruction under the new function's header.
    const Address loose = Base + 0x148;
    disassembly->navigate(loose);
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(loose));
    QTRY_VERIFY(bench.action(ActionId::EditCreateFunction)->isEnabled());
    bench.action(ActionId::EditCreateFunction)->trigger();
    const QString looseName = QStringLiteral("sub_FFFF800012340148");
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(21).value("name").toString(), looseName,
        OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentFunction(),
                              std::optional<Address>(loose), OpenTimeoutMs);
    QCOMPARE(disassembly->currentItem(), std::optional<Address>(loose));
    QTRY_VERIFY(bench.action(ActionId::EditDeleteFunction)->isEnabled());
    QVERIFY(!bench.action(ActionId::EditCreateFunction)->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd-functions.json"),
                             OpenTimeoutMs);
    // Deleting it gives the instruction back to the function around it, and
    // undo brings the new function back.
    bench.action(ActionId::EditDeleteFunction)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(21).value("name").toString(),
        QStringLiteral("function_21"), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentFunction(),
                              std::optional<Address>(Base + 0x140),
                              OpenTimeoutMs);
    bench.session.undo();
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(21).value("name").toString(), looseName,
        OpenTimeoutMs);

    // D makes the data under the cursor a value, the next size each time
    // (stderr's qword gives way to a byte, then a word); U shows its bytes as
    // bytes, and undo takes U back.  Each commits at once.
    const Address object = Base + 0x3200;
    const auto items = [&] {
      return QString::fromUtf8(readAll(path + ".neverd-items.json"));
    };
    disassembly->navigate(object);
    disassembly->focusContent();
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(object));
    QTRY_VERIFY(bench.action(ActionId::EditDefineData)->isEnabled());
    bench.action(ActionId::EditDefineData)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"byte\"")),
                             OpenTimeoutMs);
    bench.action(ActionId::EditDefineData)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"word\"")),
                             OpenTimeoutMs);
    bench.action(ActionId::EditUndefine)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"undefined\"")),
                             OpenTimeoutMs);
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"word\"")),
                             OpenTimeoutMs);

    // A restarted worker reopens the file where its database left it.
    bench.session.restart();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentItem(),
                              std::optional<Address>(Base + 0x140),
                              OpenTimeoutMs);
    QVERIFY2(bench.session.lastError().isEmpty(),
             qPrintable(bench.session.lastError()));
  }

  void sessionChangesAreExplicit() {
    QTemporaryDir directory;
    const auto first = writeFixture(directory, QStringLiteral("first.bin"));
    const auto second = writeFixture(directory, QStringLiteral("second.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(first);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    session.setComment(Base, QStringLiteral("first revision"));
    session.setComment(Base, QStringLiteral("second revision"));
    session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(first + ".neverd-annotations.json"),
                             OpenTimeoutMs);
    QTRY_VERIFY(!session.dirty());
    QVERIFY(readAll(first + ".neverd-annotations.json")
                .contains("second revision"));

    // Opening another file with unsaved edits asks first.
    session.setComment(Base + 0x140, QStringLiteral("comment on A"));
    QTRY_VERIFY(session.dirty());
    QSignalSpy confirmation(&session, &Session::transitionRequested);
    session.open(second);
    QCOMPARE(confirmation.size(), 1);
    QCOMPARE(session.filePath(), first);
    session.resolveTransition(QStringLiteral("cancel"));
    QCOMPARE(session.filePath(), first);
    session.open(second);
    QCOMPARE(confirmation.size(), 2);
    session.resolveTransition(QStringLiteral("save"));
    QTRY_COMPARE_WITH_TIMEOUT(session.filePath(), second, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QVERIFY(!session.dirty());
    QVERIFY(
        readAll(first + ".neverd-annotations.json").contains("comment on A"));

    // An interrupted external read gets a terminal reply.
    QSignalSpy replies(&session, &Session::externalResponse);
    session.externalQuery(QStringLiteral("external-restart"),
                          QStringLiteral("metadata"), {}, {});
    session.restart();
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies.first().at(0).toString(),
             QStringLiteral("external-restart"));
    const auto status = replies.first().at(1).toJsonObject();
    QVERIFY2(status.value("status").toString() == QLatin1String("ok") ||
                 status.value("error").toObject().value("code").toString() ==
                     QLatin1String("worker_stopped"),
             QJsonDocument(status).toJson().constData());
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);

    // Quitting with unsaved edits saves first when asked to.
    session.setComment(Base, QStringLiteral("keep before quit"));
    QTRY_VERIFY(session.dirty());
    QVERIFY(!session.requestQuit());
    QSignalSpy approved(&session, &Session::quitApproved);
    session.resolveTransition(QStringLiteral("save"));
    QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, OpenTimeoutMs);
    QVERIFY(!session.dirty());
    QVERIFY(readAll(second + ".neverd-annotations.json")
                .contains("keep before quit"));
  }

  void editsForAnOlderSessionAreRefused() {
    QTemporaryDir directory;
    const auto first = writeFixture(directory, QStringLiteral("first.bin"));
    const auto second = writeFixture(directory, QStringLiteral("second.bin"));
    const auto third = writeFixture(directory, QStringLiteral("third.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(first);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    const quint64 oldEpoch = session.epoch();
    QSignalSpy confirmation(&session, &Session::transitionRequested);
    session.open(second);
    session.open(third);
    QTRY_COMPARE_WITH_TIMEOUT(session.filePath(), third, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QCOMPARE(confirmation.size(), 0);
    QVERIFY(session.epoch() != oldEpoch);
    QSignalSpy messages(&session, &Session::message);
    session.setComment(Base, QStringLiteral("must not enter new project"),
                       oldEpoch);
    QVERIFY(!session.dirty());
    QVERIFY(!messages.isEmpty());
    QCOMPARE(messages.last().at(1).toInt(), 1);
  }

  void cancellingViewReadsStillCompletesExternalQueries() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("external.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QObject view;
    bool viewReplied = false;
    session.read(QStringLiteral("decompile"),
                 {{"address", hexAddress(Base)}, {"representation", "low"}},
                 &view, [&](const QJsonObject &) { viewReplied = true; });
    QSignalSpy replies(&session, &Session::externalResponse);
    session.externalQuery(QStringLiteral("external-survives-cancel"),
                          QStringLiteral("metadata"), {}, {});
    session.cancelReads();
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, OpenTimeoutMs);
    QCOMPARE(replies.first().first().toString(),
             QStringLiteral("external-survives-cancel"));
    QCOMPARE(replies.first().at(1).toJsonObject().value("status").toString(),
             QStringLiteral("ok"));
    QTest::qWait(200);
    QVERIFY(!viewReplied);
    replies.clear();
    session.externalQuery(QStringLiteral("exact-stale"),
                          QStringLiteral("metadata"), {},
                          QStringLiteral("invalid-revision"));
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies.first()
                 .at(1)
                 .toJsonObject()
                 .value("error")
                 .toObject()
                 .value("code")
                 .toString(),
             QStringLiteral("stale_revision"));
  }

  void saveBeforeQuitRejectsALateEdit() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("transition.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    session.setComment(Base, QStringLiteral("accepted before quit"));
    QTRY_VERIFY(session.dirty());
    QVERIFY(!session.requestQuit());
    QSignalSpy approved(&session, &Session::quitApproved);
    session.resolveTransition(QStringLiteral("save"));
    session.setComment(Base + 0x10,
                       QStringLiteral("late edit after save intent"));
    QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, OpenTimeoutMs);
    QVERIFY(!session.dirty());
    const auto bytes = readAll(path + ".neverd-annotations.json");
    QVERIFY(bytes.contains("accepted before quit"));
    QVERIFY(!bytes.contains("late edit after save intent"));
  }

  void databaseCarriesTheProject() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("project.bin"));
    const auto database = ProjectDatabase::pathFor(path);
    {
      Workbench bench;
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->disassembly()->navigate(Base + 0x140);
      QTRY_COMPARE(bench.window->disassembly()->currentItem(),
                   std::optional<Address>(Base + 0x140));
      bench.session.setComment(Base + 0x140, QStringLiteral("packed comment"));
      QTRY_VERIFY(bench.session.dirty());
      QSignalSpy saved(&bench.session, &Session::databaseSaved);
      bench.session.save();
      QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
      QVERIFY(saved.first().first().toBool());
      QVERIFY(QFileInfo::exists(database));
      QTRY_VERIFY(!bench.session.dirty());
    }
    // The database alone reopens the project: input, comment and location.
    // Its header identifies it, so a copy without the suffix opens too.
    for (const auto &name :
         {QStringLiteral("moved.nddb"), QStringLiteral("moved.db")}) {
      QTemporaryDir moved;
      const auto copy = moved.filePath(name);
      QVERIFY(QFile::copy(database, copy));
      Workbench bench;
      QVERIFY(dropFile(bench.window.get(), copy));
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QCOMPARE(bench.session.projectPath(), copy);
      QCOMPARE(bench.session.databasePath(), copy);
      QVERIFY(bench.session.filePath() != path);
      QTRY_COMPARE_WITH_TIMEOUT(bench.window->disassembly()->currentItem(),
                                std::optional<Address>(Base + 0x140),
                                OpenTimeoutMs);
      QString comment;
      bench.session.read(QStringLiteral("resolve"),
                         {{"query", hexAddress(Base + 0x140)}}, &bench.session,
                         [&](const QJsonObject &payload) {
                           comment = payload.value("comment").toString();
                         });
      QTRY_COMPARE(comment, QStringLiteral("packed comment"));
    }
  }

  void contributionsRegisterRunAndUnload() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("contrib.bin"));
    const auto manifest = directory.filePath(QStringLiteral("manifest.json"));
    {
      QFile file(manifest);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(
          R"({"schema_version": 1, "namespace": "sample", "version": "1.0",
        "contributions": [{"id": "sample:selected-code", "title": "Selected instructions",
        "kind": "panel", "query": {"operation": "disasm",
        "payload": {"address": "${address}", "limit": 3}}}]})");
    }
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!session.contributions().isEmpty(), OpenTimeoutMs);
    const auto builtIn = session.contributions().size();
    session.registerContributions(manifest);
    QTRY_COMPARE(session.contributions().size(), builtIn + 1);
    QSignalSpy results(&session, &Session::contributionResult);
    session.executeContribution(QStringLiteral("sample:selected-code"), Base);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, OpenTimeoutMs);
    const auto result = results.first().first().toJsonObject();
    QCOMPARE(result.value("contribution_id").toString(),
             QStringLiteral("sample:selected-code"));
    QCOMPARE(result.value("result").toObject().value("items").toArray().size(),
             3);
    session.unregisterContributions(QStringLiteral("sample"));
    QTRY_COMPARE(session.contributions().size(), builtIn);
  }
};

QTEST_MAIN(WorkbenchTests)
#include "WorkbenchTests.moc"
