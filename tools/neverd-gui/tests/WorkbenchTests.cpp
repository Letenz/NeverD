// Workbench controller tests against the fixture worker: the session, the
// production main window and its views, edits and session transitions.
#include "ChooserView.h"
#include "CodeView.h"
#include "DisassemblyView.h"
#include "GraphView.h"
#include "HexView.h"
#include "MainWindow.h"
#include "ProjectDatabase.h"
#include "Session.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
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
} // namespace

class WorkbenchTests : public QObject {
  Q_OBJECT
  QTemporaryDir settingsDirectory_;
private slots:
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
    QTemporaryDir moved;
    const auto copy = moved.filePath(QStringLiteral("moved.nddb"));
    QVERIFY(QFile::copy(database, copy));
    Workbench bench;
    bench.window->openFile(copy);
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
