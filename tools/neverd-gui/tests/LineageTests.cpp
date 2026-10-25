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
#include "LineagePageView.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace neverd::gui;
namespace {
constexpr int Timeout = 30000;
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
    const auto ID = Versions->selected()["id"];
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
    Trace->table()->setFocus();
    Trace->table()->selectRow(0);
    QTest::keyClick(Trace->table(), Qt::Key_C, Qt::ControlModifier);
    const auto Copied =
        QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8())
            .object();
    QCOMPARE(Copied, Trace->selected());
    QVERIFY(Copied.contains("witness_edge") && Copied.contains("version"));
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
      const auto OriginalFont = View.font();
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        const auto Name = Mode == Theme::Mode::Light ? "light" : "dark";
        View.resize(1180, 780);
        for (int Tab : {0, 1, 2}) {
          Tabs->setCurrentIndex(Tab);
          QTest::qWait(60);
          QVERIFY(View.grab().save(
              Captures + QString("/%1-tab%2.png").arg(Name).arg(Tab)));
        }
        View.resize(760, 560);
        Tabs->setCurrentIndex(0);
        QTest::qWait(60);
        QVERIFY(
            View.grab().save(Captures + QString("/%1-narrow.png").arg(Name)));
        auto Large = OriginalFont;
        Large.setPointSizeF(OriginalFont.pointSizeF() * 1.5);
        View.setFont(Large);
        View.resize(1180, 780);
        QTest::qWait(60);
        QVERIFY(View.grab().save(Captures +
                                 QString("/%1-large-font.png").arg(Name)));
        View.setFont(OriginalFont);
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
QTEST_MAIN(LineageTests)
#include "LineageTests.moc"
