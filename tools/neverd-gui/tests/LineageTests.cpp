//===- LineageTests.cpp - Offline version graph interaction ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real worker evidence, keyboard navigation, failed replacement and captures.
/// Authored machine code is analyzed, never executed.
//===----------------------------------------------------------------------===//

#include "ComparisonSession.h"
#include "LineageDialog.h"
#include "LineageGraph.h"
#include "LineagePageView.h"
#include "Theme.h"
#include "WindowFrame.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QGraphicsItem>
#include <QGraphicsTextItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

using namespace neverd::gui;
namespace {
constexpr int Timeout = 30000;
bool privateShell(const QString &Script) {
  QProcess P;
  P.start("gdbus",
          {"call", "--session", "--dest", "org.gnome.Shell", "--object-path",
           "/org/gnome/Shell", "--method", "org.gnome.Shell.Eval", Script});
  return P.waitForFinished() && P.exitCode() == 0 &&
         P.readAllStandardOutput().startsWith("(true,");
}
bool write(const QString &Path, const QByteArray &Bytes) {
  QFile F(Path);
  return F.open(QIODevice::WriteOnly) && F.write(Bytes) == Bytes.size();
}
QByteArray read(const QString &Path) {
  QFile F(Path);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray{};
}
} // namespace

class LineageTests final : public QObject {
  Q_OBJECT
  QTemporaryDir Directory;
  QJsonArray Reports;
private slots:
  void initTestCase() {
    if (QGuiApplication::platformName().startsWith("wayland") &&
        qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER")) {
      // The socket precedes desktop readiness. Park the trusted pointer away
      // from (0, 0), whose hot corner otherwise reopens the startup overview.
      QTRY_VERIFY_WITH_TIMEOUT(
          privateShell("if (Main.layoutManager._startingUp) throw "
                       "Error('desktop starting');"),
          Timeout);
      QVERIFY(privateShell("global.neverdLineagePointer = "
                           "imports.gi.Clutter.get_default_backend()"
                           ".get_default_seat().create_virtual_device(imports."
                           "gi.Clutter.InputDeviceType.POINTER_DEVICE); "
                           "global.neverdLineagePointer.notify_absolute_motion("
                           "imports.gi.GLib.get_monotonic_time(), "
                           "global.stage.width / 2, global.stage.height / 2); "
                           "Main.overview.hide();"));
      QTest::qWait(750);
      QTRY_VERIFY_WITH_TIMEOUT(
          privateShell(
              "Main.overview.hide(); "
              "if (Main.overview.visible) throw Error('overview closing');"),
          Timeout);
    }
    QCoreApplication::setOrganizationName("NeverDLineageTests");
    QCoreApplication::setApplicationName("LineageTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Directory.path());
    Theme::instance().apply();
    ComparisonSession Source(TEST_LINEAGE_WORKER, this);
    QSignalSpy Errors(&Source, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Source.ready(), Timeout);
    QList<QJsonObject> Inputs;
    for (int I = 1; I <= 3; ++I) {
      const auto Path = Directory.filePath(QString("release-%1.raw").arg(I));
      auto Bytes = QByteArray::fromHex("b801000000c3");
      Bytes[1] = char(I);
      QVERIFY(write(Path, Bytes));
      Inputs.append({{"path", Path},
                     {"load_options", QJsonObject{{"loader", "binary"},
                                                  {"processor", "x86_64"},
                                                  {"platform", "sysv"},
                                                  {"base", "0x1000"}}}});
    }
    for (auto Pair : {qMakePair(0, 1), qMakePair(1, 2), qMakePair(0, 2)}) {
      Source.start(Inputs[Pair.first], Inputs[Pair.second]);
      QTRY_VERIFY_WITH_TIMEOUT(!Source.busy() && !Source.revision().isEmpty(),
                               Timeout);
      QCOMPARE(Errors.size(), 0);
      const auto Path = Directory.filePath(
          QString("%1-%2.nddiff").arg(Pair.first).arg(Pair.second));
      bool Saved = false;
      Source.request("comparison_save", {{"path", Path}}, this,
                     [&](const QJsonObject &R) {
                       Saved = R["status"] == QJsonValue("ok");
                     });
      QTRY_VERIFY_WITH_TIMEOUT(Saved, Timeout);
      Reports.append(Path);
    }
  }

  void navigationPersistenceAndRender() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    auto *Trace = View.findChild<LineagePageView *>("lineageTraceNodes");
    auto *Tabs = View.findChild<QTabWidget *>("lineageTabs");
    QVERIFY(Versions && Functions && Trace && Tabs);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 3 &&
                                 Functions->rows().size() == 1,
                             Timeout);
    QCOMPARE(Session.summary()["edges"].toInt(), 3);
    QCOMPARE(Errors.size(), 0);
    Versions->table()->selectRow(1);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    const auto ID = Versions->selected().value("id");
    Session.load(
        {{"append", true},
         {"version_updates",
          QJsonArray{QJsonObject{{"id", ID},
                                 {"label", "Release 2026.10 — stable 中文"},
                                 {"branch", "maintenance"}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    QCOMPARE(Versions->selected()["label"].toString(),
             QString::fromUtf8("Release 2026.10 — stable 中文"));
    auto *TraceButton = View.findChild<QPushButton *>("lineageTrace");
    QVERIFY(TraceButton && TraceButton->isEnabled());
    TraceButton->setFocus();
    QTest::keyClick(TraceButton, Qt::Key_Space);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Trace->rows().size() == 3,
                             Timeout);
    QCOMPARE(Tabs->currentIndex(), 1);
    View.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&View));
    Trace->table()->setFocus();
    Trace->table()->selectRow(0);
    QTest::keyClick(Trace->table(), Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(
        QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8())
            .object(),
        Trace->selected(), Timeout);
    const auto Copied =
        QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8())
            .object();
    QVERIFY(Copied.contains("witness_edge") && Copied.contains("version"));
    Tabs->setCurrentIndex(2);
    auto *Graph = View.findChild<LineageGraph *>("lineageGraph");
    const auto Target = Versions->rows()[0].toObject()["id"].toString();
    QTest::qWait(60);
    QGraphicsItem *Node = nullptr;
    for (auto *Item : Graph->scene()->items())
      if (Item->data(0) == "version" && Item->data(1).toString() == Target)
        Node = Item;
    QVERIFY(Node);
    QTest::mouseDClick(Graph->viewport(), Qt::LeftButton, Qt::NoModifier,
                       Graph->mapFromScene(Node->sceneBoundingRect().center()));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Tabs->currentIndex() == 0,
                             Timeout);
    QCOMPARE(Versions->selected()["id"].toString(), Target);
    QCOMPARE(Functions->selected()["version"].toString(), Target);
    const auto OriginalRevision = Session.revision();
    const auto Selection = Functions->selected();
    const auto Malformed = Directory.filePath("malformed.ndlineage");
    QVERIFY(write(Malformed, "{broken"));
    Session.load({{"path", Malformed}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Errors.size(), 1);
    QCOMPARE(Session.revision(), OriginalRevision);
    QCOMPARE(Functions->selected(), Selection);
    QVERIFY(Session.message().contains("previous graph"));
    auto *Search = View.findChild<QLineEdit *>("lineageSearch");
    Tabs->setCurrentIndex(0);
    Search->setText("no such function");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().isEmpty(),
                             Timeout);
    QVERIFY(!TraceButton->isEnabled());
    Search->clear();
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    const auto Captures = qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      const auto OriginalStyle = View.styleSheet();
      const auto OriginalPixels =
          QFontInfo(Functions->table()->font()).pixelSize();
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        const auto Name = Mode == Theme::Mode::Light ? "light" : "dark";
        View.resize(1180, 780);
        for (int Tab : {0, 1, 2}) {
          Tabs->setCurrentIndex(Tab);
          QTest::qWait(60);
          if (Tab == 2) {
            auto *Graph = View.findChild<LineageGraph *>("lineageGraph");
            QVERIFY(Graph && Graph->height() >= 180 &&
                    Graph->transform().m11() > 0.4);
          }
          QVERIFY(View.grab().save(
              Captures + QString("/%1-tab%2.png").arg(Name).arg(Tab)));
        }
        View.resize(760, 560);
        Tabs->setCurrentIndex(0);
        QTest::qWait(60);
        QVERIFY(
            View.grab().save(Captures + QString("/%1-narrow.png").arg(Name)));
        View.setStyleSheet(OriginalStyle +
                           QString("\nQWidget { font-size: %1px; }")
                               .arg(qRound(OriginalPixels * 1.5)));
        View.resize(1180, 780);
        QTest::qWait(60);
        QVERIFY(View.grab().save(Captures +
                                 QString("/%1-large-font.png").arg(Name)));
        QVERIFY(QFontInfo(Functions->table()->font()).pixelSize() >
                OriginalPixels);
        QVERIFY(Functions->table()->rowHeight(0) >=
                Functions->table()->fontMetrics().height());
        View.setStyleSheet(OriginalStyle);
      }
    }
    const auto Saved = Directory.filePath("workspace.ndlineage");
    QSignalSpy SavedSignal(&Session, &LineageSession::saved);
    Session.save(Saved);
    QTRY_VERIFY_WITH_TIMEOUT(SavedSignal.size() == 1 && !Session.busy(),
                             Timeout);
    const auto Bundle = QJsonDocument::fromJson(read(Saved)).object();
    for (const auto &R : Reports)
      QVERIFY(Bundle["reports"].toArray().contains(
          QString::fromUtf8(read(R.toString()))));
    Session.load({{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.revision(), OriginalRevision);
    QCOMPARE(Errors.size(), 1);
  }

  void cancellationRetainsCollectionAndCanRetry() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    const auto Revision = Session.revision();
    const auto Large = Directory.filePath("large-cancel.nddiff");
    QVERIFY(write(Large, read(Reports[0].toString()) +
                             QByteArray(64 * 1024 * 1024, ' ')));
    bool Cancelled = false;
    const auto Connection = connect(
        &Session, &LineageSession::progress, this, [&](const QJsonObject &P) {
          if (!Cancelled && P["phase"] == QJsonValue("lineage:read")) {
            Cancelled = true;
            Session.cancel();
          }
        });
    Session.load({{"reports", QJsonArray{Large}}});
    QTRY_VERIFY_WITH_TIMEOUT(Cancelled && !Session.busy(), Timeout);
    disconnect(Connection);
    QVERIFY(!Session.cancelRequested());
    QCOMPARE(Session.revision(), Revision);
    QVERIFY(Session.message().contains("stopped"));
    Session.load({{"reports", Reports}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.revision(), Revision);
    QCOMPARE(Session.summary()["reports"].toInt(), 3);
  }

  void nextPageAndMetadataKeepTheSelectedSnapshot() {
    // Authored report-only identities exercise paging; these copies do not
    // claim fresh native authentication of synthetic release hashes.
    const auto Original =
        QJsonDocument::fromJson(read(Reports[0].toString())).object();
    QJsonArray Many;
    for (int I = 0; I < 202; ++I) {
      auto Document = Original;
      auto Secondary = Document["secondary"].toObject();
      Secondary["sha256"] =
          QString::number(I + 1000, 16).rightJustified(64, '0');
      Secondary["path"] = QString("fixture-release-%1.raw").arg(I);
      Document["secondary"] = Secondary;
      const auto Path = Directory.filePath(QString("paging-%1.nddiff").arg(I));
      QVERIFY(
          write(Path, QJsonDocument(Document).toJson(QJsonDocument::Compact)));
      Many.append(Path);
    }
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Many}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 200,
                             Timeout);
    QToolButton *Next = nullptr;
    for (auto *B : Versions->findChildren<QToolButton *>())
      if (B->arrowType() == Qt::RightArrow)
        Next = B;
    QVERIFY(Next && Next->isEnabled());
    QTest::mouseClick(Next, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 3,
                             Timeout);
    QVERIFY(!Next->isEnabled());
    Versions->table()->selectRow(1);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    const auto ID = Versions->selected().value("id");
    Session.load({{"append", true},
                  {"version_updates",
                   QJsonArray{QJsonObject{{"id", ID},
                                          {"label", "Second-page release"},
                                          {"branch", "stable"}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Versions->offset(), 200);
    QCOMPARE(Versions->selected().value("id"), ID);
    QCOMPARE(Versions->selected().value("label").toString(),
             QString("Second-page release"));
  }

  void smallerPagesRecoverLargeMetadataWithoutLosingSnapshots() {
    // These report-only fixture identities isolate paging. The source reader
    // still validates every report; no fresh native authentication is claimed.
    const auto Original =
        QJsonDocument::fromJson(read(Reports[0].toString())).object();
    QJsonArray Many;
    for (int I = 0; I < 100; ++I) {
      auto Document = Original;
      for (const auto &[Side, Number] :
           {qMakePair("primary", I * 2), qMakePair("secondary", I * 2 + 1)}) {
        auto Identity = Document[Side].toObject();
        Identity["sha256"] =
            QString::number(Number + 2000, 16).rightJustified(64, '0');
        Document[Side] = Identity;
      }
      const auto Path =
          Directory.filePath(QString("large-page-%1.nddiff").arg(I));
      QVERIFY(
          write(Path, QJsonDocument(Document).toJson(QJsonDocument::Compact)));
      Many.append(Path);
    }
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    View.show();
    View.resize(760, 560);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Many}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Sources = View.findChild<LineagePageView *>("lineageSources");
    QVERIFY(Versions && Sources);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 200,
                             Timeout);
    const auto Initial = Versions->rows();
    const QString Padding(48 * 1024, 'x');
    QJsonArray Updates;
    for (int I = 0; I < Initial.size(); ++I)
      Updates.append(
          QJsonObject{{"id", Initial[I].toObject()["id"]},
                      {"label", QString("Snapshot %1: ").arg(I) + Padding},
                      {"branch", ""}});
    const auto Saved = Directory.filePath("large-page.ndlineage");
    QSignalSpy SavedEvents(&Session, &LineageSession::saved);
    Session.save(Saved);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && SavedEvents.size() == 1,
                             Timeout);
    auto Collection = QJsonDocument::fromJson(read(Saved)).object();
    Collection["versions"] = Updates;
    QVERIFY(
        write(Saved, QJsonDocument(Collection).toJson(QJsonDocument::Compact)));
    Session.load({{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Errors.size(), 0);
    QCOMPARE(Session.summary()["versions"].toInt(), 200);
    QVERIFY(Versions->rows().isEmpty());
    QVERIFY(Sources->rows().isEmpty());

    // Admission succeeded, but both default pages exceed the byte allowance.
    // The user must be able to recover without replacing or truncating data.
    auto *Rows = Versions->findChild<QSpinBox *>("lineagePageRows");
    QVERIFY(Rows && Rows->isEnabled());
    QCOMPARE(Rows->value(), 200);
    Rows->setValue(100);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 100,
                             Timeout);
    for (int I = 0; I < 100; ++I) {
      QCOMPARE(Versions->rows()[I].toObject()["id"],
               Initial[I].toObject()["id"]);
      QVERIFY(
          Versions->rows()[I].toObject()["label"].toString().endsWith(Padding));
    }
    QToolButton *Next = nullptr, *Previous = nullptr;
    for (auto *Button : Versions->findChildren<QToolButton *>()) {
      if (Button->arrowType() == Qt::RightArrow)
        Next = Button;
      if (Button->arrowType() == Qt::LeftArrow)
        Previous = Button;
    }
    QVERIFY(Next && Previous && Next->isEnabled());
    QTest::mouseClick(Next, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->offset() == 100,
                             Timeout);
    QCOMPARE(Versions->rows().size(), 100);
    for (int I = 0; I < 100; ++I)
      QCOMPARE(Versions->rows()[I].toObject()["id"],
               Initial[I + 100].toObject()["id"]);
    QVERIFY(!Next->isEnabled());
    Rows->setValue(50);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 50,
                             Timeout);
    QCOMPARE(Versions->offset(), 100);
    QVERIFY(Previous->isEnabled());
    QTest::mouseClick(Previous, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->offset() == 50,
                             Timeout);
    QCOMPARE(Versions->rows()[0].toObject()["id"],
             Initial[50].toObject()["id"]);

    auto *SourceRows = Sources->findChild<QSpinBox *>("lineagePageRows");
    QVERIFY(SourceRows && SourceRows->isEnabled());
    SourceRows->setValue(50);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Sources->rows().size() == 50,
                             Timeout);
    QCOMPARE(Session.summary()["versions"].toInt(), 200);
    QCOMPARE(Errors.size(), 0);
    const auto *PageStatus = Versions->findChild<QLabel *>("lineagePageStatus");
    QVERIFY(PageStatus && PageStatus->isVisible());
    QVERIFY(PageStatus->width() >=
            PageStatus->fontMetrics().horizontalAdvance(PageStatus->text()));
    if (const auto Output = qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
        !Output.isEmpty())
      QVERIFY(View.grab().save(Output + "/large-page-recovered.png"));
  }

  void largeReportDiagramsCanZoomAfterFitting() {
    LineageGraph Graph;
    Graph.resize(400, 240);
    Graph.show();
    QJsonArray Edges;
    for (int I = 0; I < 200; ++I)
      Edges.append(
          QJsonObject{{"id", QString("report-%1").arg(I)},
                      {"primary_version", "origin"},
                      {"secondary_version", QString("snapshot-%1").arg(I)}});
    Graph.setReports(Edges);
    QTRY_VERIFY_WITH_TIMEOUT(Graph.transform().m11() < 1, Timeout);
    const auto Fitted = Graph.transform();
    Graph.zoom(false);
    QCOMPARE(Graph.transform(), Fitted);
    Graph.zoom(true);
    const auto First = Graph.transform().m11();
    QVERIFY(First > Fitted.m11());
    Graph.zoom(true);
    const auto Second = Graph.transform().m11();
    QVERIFY(Second > First);
    Graph.zoom(false);
    QVERIFY(Graph.transform().m11() < Second);
  }

  void replacementResetsNavigationButAppendAndFailureKeepIt() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    auto *Trace = View.findChild<LineagePageView *>("lineageTraceNodes");
    auto *Search = View.findChild<QLineEdit *>("lineageSearch");
    auto *Tabs = View.findChild<QTabWidget *>("lineageTabs");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    const auto ID = Versions->selected().value("id");
    QTest::mouseClick(View.findChild<QPushButton *>("lineageTrace"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Trace->rows().size() == 3,
                             Timeout);
    Search->setText("an unmatched search from the old collection");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().isEmpty(),
                             Timeout);
    const auto Previous = Session.revision();
    Session.load({{"path", Directory.filePath("missing-lineage.ndlineage")}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Errors.size() == 1, Timeout);
    QCOMPARE(Session.revision(), Previous);
    QCOMPARE(Trace->rows().size(), 3);
    QVERIFY(!Search->text().isEmpty());
    QCOMPARE(Tabs->currentIndex(), 1);
    Session.load({{"append", true},
                  {"version_updates",
                   QJsonArray{QJsonObject{{"id", ID},
                                          {"label", "A retained snapshot"},
                                          {"branch", "stable"}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Versions->selected()["id"], ID);
    QCOMPARE(Trace->rows().size(), 3);
    QVERIFY(!Search->text().isEmpty());
    QCOMPARE(Tabs->currentIndex(), 1);

    // Replace with two disjoint authored snapshot identities. These report
    // copies exercise navigation, not native authentication or new analysis.
    auto Other = QJsonDocument::fromJson(read(Reports[0].toString())).object();
    for (const auto *Side : {"primary", "secondary"}) {
      auto Identity = Other[Side].toObject();
      Identity["sha256"] = QString(64, Side[0] == 'p' ? 'a' : 'b');
      Other[Side] = Identity;
    }
    const auto Path = Directory.filePath("replacement.nddiff");
    QVERIFY(write(Path, QJsonDocument(Other).toJson(QJsonDocument::Compact)));
    Session.load({{"reports", QJsonArray{Path}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 2,
                             Timeout);
    QCOMPARE(Errors.size(), 1);
    QVERIFY(Search->text().isEmpty());
    QCOMPARE(Tabs->currentIndex(), 0);
    QVERIFY(Trace->rows().isEmpty());
    QCOMPARE(Functions->rows().size(), 1);
    QVERIFY(Versions->selected()["id"] != ID);
    QTest::mouseClick(View.findChild<QPushButton *>("lineageTrace"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Trace->rows().size() == 2,
                             Timeout);
  }

  void longMetadataKeepsFunctionControlsReachable_data() {
    QTest::addColumn<QString>("Long");
    QTest::newRow("wide") << QString("Long release label ").repeated(1024);
    QTest::newRow("multiple-lines")
        << QString("Release") + QString(256, '\r') + QString(256, '\n') +
               QString(256, QChar::LineSeparator) +
               QString(256, QChar::ParagraphSeparator) + "candidate";
  }

  void longMetadataKeepsFunctionControlsReachable() {
    QFETCH(QString, Long);
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    View.show();
    View.resize(760, 560);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    const auto Before = View.size();
    const auto ID = Versions->selected().value("id");
    Session.load({{"append", true},
                  {"version_updates",
                   QJsonArray{QJsonObject{
                       {"id", ID}, {"label", Long}, {"branch", "stable"}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    QTest::qWait(60);
    QCOMPARE(Versions->selected().value("label").toString(), Long);
    QCOMPARE(View.size(), Before);
    QVERIFY(Functions->table()->viewport()->height() >=
            Functions->table()->fontMetrics().height() * 3);
    auto *Action = View.findChild<QPushButton *>("lineageTrace");
    QVERIFY(Action && Action->isEnabled());
    QVERIFY(
        View.rect().contains(Action->mapTo(&View, Action->rect().center())));
    QTest::mouseClick(Action, Qt::LeftButton);
    auto *TraceNodes = View.findChild<LineagePageView *>("lineageTraceNodes");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && TraceNodes->rows().size() == 3,
                             Timeout);
    QTest::qWait(60);
    QCOMPARE(View.size(), Before);
    QVERIFY(TraceNodes->table()->viewport()->height() >=
            TraceNodes->table()->fontMetrics().height() * 3);
    View.findChild<QTabWidget *>("lineageTabs")->setCurrentIndex(2);
    auto *Graph = View.findChild<LineageGraph *>("lineageGraph");
    QTest::qWait(60);
    bool Found = false;
    for (auto *Item : Graph->scene()->items())
      if (Item->data(0) == "version" && Item->data(1) == ID.toString()) {
        Found = true;
        for (auto *Child : Item->childItems())
          if (auto *Text = qgraphicsitem_cast<QGraphicsTextItem *>(Child)) {
            QVERIFY(Item->boundingRect().contains(
                Text->mapRectToParent(Text->boundingRect())));
            QCOMPARE(Text->toPlainText().count('\n'), 1);
          }
      }
    QVERIFY(Found);
  }

  void privateCompositorResizeAndReopen() {
    if (!QGuiApplication::platformName().startsWith("wayland") ||
        !qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER"))
      QSKIP("Requires the isolated GNOME compositor's trusted pointer.");
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.setWindowTitle("NeverD lineage resize test");
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(View.session().ready(), Timeout);
    QVERIFY(QTest::qWaitForWindowExposed(&View));
    QVERIFY(privateShell(
        QString("Main.overview.hide(); global.neverdLineageWindow = "
                "global.get_window_actors().map(a => a.meta_window).find(w => "
                "w.get_pid() === %1 && w.get_title() === 'NeverD lineage "
                "resize test'); "
                "global.neverdLineageWindow.move_frame(true, 200, 150); "
                "Main.activateWindow(global.neverdLineageWindow);")
            .arg(QCoreApplication::applicationPid())));
    QTest::qWait(750);
    // Finish any overview animation before sending real input.
    QTRY_VERIFY_WITH_TIMEOUT(
        privateShell(
            "Main.overview.hide(); "
            "if (Main.overview.visible) throw Error('overview closing');"),
        Timeout);
    QVERIFY(
        privateShell("global.neverdLineageWindow.move_frame(true, 200, 150); "
                     "Main.activateWindow(global.neverdLineageWindow);"));
    QTest::qWait(100);
    const QSize Before = View.size();
    // The compositor frame excludes the transparent shadow band. Its visible
    // corner receives input; the outer buffer corner deliberately does not.
    QVERIFY(privateShell(
        "(() => { const r = global.neverdLineageWindow.get_frame_rect(); "
        "global.neverdLineageCorner = [r.x + r.width - 3, r.y + r.height "
        "- 3]; })()"));
    const auto Motion = [&](QPoint Delta) {
      return privateShell(
          QString("global.neverdLineagePointer.notify_absolute_motion("
                  "imports.gi.GLib.get_monotonic_time(), "
                  "global.neverdLineageCorner[0] + %1, "
                  "global.neverdLineageCorner[1] + %2)")
              .arg(qRound(Delta.x() * View.devicePixelRatioF()))
              .arg(qRound(Delta.y() * View.devicePixelRatioF())));
    };
    const auto Button = [&](const QString &State) {
      return privateShell(QString("global.neverdLineagePointer.notify_button("
                                  "imports.gi.GLib.get_monotonic_time(), 1, "
                                  "imports.gi.Clutter.ButtonState.%1)")
                              .arg(State));
    };
    QVERIFY(Motion({}));
    QTest::qWait(50);
    QVERIFY(Button("PRESSED"));
    QTest::qWait(50);
    QVERIFY(Motion(QPoint(80, 60)));
    QTest::qWait(100);
    QVERIFY(Button("RELEASED"));
    QTRY_VERIFY_WITH_TIMEOUT(View.width() > Before.width() &&
                                 View.height() > Before.height(),
                             Timeout);
    const auto Chosen = View.size();
    QVERIFY(View.close());
    LineageDialog Reopened(TEST_LINEAGE_WORKER);
    Reopened.setAttribute(Qt::WA_DeleteOnClose, false);
    Reopened.show();
    QVERIFY(QTest::qWaitForWindowExposed(&Reopened));
    QTRY_COMPARE_WITH_TIMEOUT(Reopened.size(), Chosen, Timeout);
  }

  void invalidResourceSettingsKeepTheDialogAndInput() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(View.session().ready(), Timeout);
    bool Checked = false;
    QTimer::singleShot(0, this, [&] {
      auto *D = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!D)
        return;
      auto *Nodes = D->findChild<QLineEdit *>("lineageOption_max_nodes");
      auto *Buttons = D->findChild<QDialogButtonBox *>();
      if (!Nodes || !Buttons) {
        D->reject();
        return;
      }
      Nodes->setText("0");
      QTest::mouseClick(Buttons->button(QDialogButtonBox::Save),
                        Qt::LeftButton);
      Checked =
          D->isVisible() && Nodes->text() == "0" &&
          !D->findChild<QLabel *>("lineageOptionsError")->text().isEmpty();
      D->reject();
    });
    QTest::mouseClick(View.findChild<QPushButton *>("lineageOptions"),
                      Qt::LeftButton);
    QVERIFY(Checked);
  }
};
int main(int argc, char **argv) {
  WindowFrame::prepareNativeWindows();
  QApplication App(argc, argv);
  WindowFrame::frameDialogs(App);
  LineageTests Tests;
  return QTest::qExec(&Tests, argc, argv);
}
#include "LineageTests.moc"
