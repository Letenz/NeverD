//===- ComparisonTests.cpp - Real comparison GUI contracts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real worker inputs, bounded pages, mapping edits and responsive Qt widgets.
/// Authored machine code is inspected but never executed.
//===----------------------------------------------------------------------===//

#include "ComparisonChangesDialog.h"
#include "ComparisonCounterpartDialog.h"
#include "ComparisonDialog.h"
#include "ComparisonFlowDialog.h"
#include "ComparisonGraph.h"
#include "ComparisonImpactDialog.h"
#include "ComparisonModel.h"
#include "ComparisonRegionsDialog.h"
#include "ComparisonReviewDialog.h"
#include "ComparisonSourceView.h"
#include "ComparisonValidationDialog.h"
#include "ProjectDatabase.h"
#include "Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <algorithm>

using namespace neverd::gui;
namespace {
constexpr int Timeout = 15000;
bool write(const QString &Path, const QByteArray &Bytes) {
  QFile F(Path);
  return F.open(QIODevice::WriteOnly) && F.write(Bytes) == Bytes.size();
}
} // namespace

class ComparisonTests final : public QObject {
  Q_OBJECT
  QTemporaryDir Directory;
private slots:
  void initTestCase() {
    QCoreApplication::setOrganizationName("NeverDComparisonTests");
    QCoreApplication::setApplicationName("ComparisonTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Directory.path());
    Theme::instance().apply();
  }
  void savedProjectsCompareThroughTheSharedReader() {
    const auto A = Directory.filePath("saved-before.bin");
    const auto B = Directory.filePath("saved-after.bin");
    const auto DA = ProjectDatabase::pathFor(A);
    const auto DB = ProjectDatabase::pathFor(B);
    QVERIFY(write(A, QByteArray::fromHex("b811000000c3")));
    QVERIFY(write(B, QByteArray::fromHex("b809000000c3")));
    const auto Load = QJsonDocument(QJsonObject{{"loader", "binary"},
                                                {"processor", "x86_64"},
                                                {"platform", "sysv"},
                                                {"base", "0x1000"}});
    for (const auto &Path : {A, B}) {
      QCOMPARE(
          ProjectDatabase::writeProjectFile(Path, ".neverd-load.json", Load),
          QString());
      QCOMPARE(ProjectDatabase::writeProjectFile(
                   Path, ".neverd-renames.json",
                   QJsonDocument(QJsonArray{QJsonObject{
                       {"addr", "0x1000"}, {"renamed", "saved_check"}}})),
               QString());
      QCOMPARE(ProjectDatabase::save(ProjectDatabase::pathFor(Path), Path, {}),
               QString());
      QVERIFY(QFile::remove(Path));
      QVERIFY(QDir(ProjectDatabase::projectFolder(Path)).removeRecursively());
    }
    const auto Read = [](const QString &Path) {
      QFile F(Path);
      return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
    };
    const auto Before = Read(DA);
    const auto After = Read(DB);
    // A user's normal unpack/recovery copy must remain separate from DIFF.
    const auto Recovery = ProjectDatabase::workingDirectory(DA);
    QVERIFY(QDir().mkpath(Recovery));
    const auto Sentinel = QDir(Recovery).filePath("pending-edit.txt");
    QVERIFY(write(Sentinel, "local recovery remains intact"));
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(DA);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(DB);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    QCOMPARE(Failures.size(), 0);
    const auto Primary = Session.summary()["primary"].toObject();
    QCOMPARE(Primary["path"].toString(), DA);
    QCOMPARE(Primary["project"].toObject()["input_name"].toString(),
             QString("saved-before.bin"));
    auto *Inventory = View.findChild<QTableView *>("comparisonFunctions");
    auto *Model = qobject_cast<ComparisonModel *>(Inventory->model());
    QTRY_VERIFY_WITH_TIMEOUT(Model->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Model->row(0)["name"].toString() == "saved_check",
                             Timeout);
    Inventory->setCurrentIndex(Model->index(0, 0));
    auto *Instructions = View.findChild<QTableView *>("comparisonInstructions");
    QTRY_VERIFY_WITH_TIMEOUT(Instructions->model()->rowCount() > 0, Timeout);
    View.resize(1280, 800);
    QCOMPARE(Read(DA), Before);
    QCOMPARE(Read(DB), After);
    QCOMPARE(Read(Sentinel), QByteArray("local recovery remains intact"));
    QVERIFY(!QFileInfo::exists(A));
    QVERIFY(!QFileInfo::exists(B));
    auto *Source = View.findChild<ComparisonSourceView *>("comparisonSource");
    QVERIFY(Source);
    auto *Tabs = View.findChild<QTabWidget *>();
    QVERIFY(Tabs);
    Tabs->setCurrentWidget(Source);
    QCOMPARE(Tabs->currentWidget(), Source);
    QVERIFY(Source->isVisible());
    auto *PrimarySource =
        View.findChild<QPlainTextEdit *>("comparisonPrimarySource");
    auto *SecondarySource =
        View.findChild<QPlainTextEdit *>("comparisonSecondarySource");
    QTRY_VERIFY2_WITH_TIMEOUT(
        PrimarySource->toPlainText().contains("return"),
        qPrintable(
            View.findChild<QLabel *>("comparisonPrimarySourceStatus")->text()),
        Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(SecondarySource->toPlainText().contains("return"),
                             Timeout);
    QVERIFY(PrimarySource->toPlainText().contains("return"));
    QVERIFY(PrimarySource->toPlainText() != SecondarySource->toPlainText());
    auto *Alignment = View.findChild<QCheckBox *>("comparisonSourceAlignment");
    QVERIFY(Alignment && Alignment->isChecked());
    QCOMPARE(PrimarySource->blockCount(), SecondarySource->blockCount());
    QVERIFY(!PrimarySource->extraSelections().isEmpty());
    QVERIFY(!SecondarySource->extraSelections().isEmpty());
    QVERIFY(View.findChild<QLabel *>("comparisonPrimarySourceTitle")
                ->text()
                .contains("saved_check"));
    auto *Jump =
        View.findChild<QPushButton *>("comparisonPrimarySourceInstruction");
    for (auto Block = PrimarySource->document()->begin();
         Block.isValid() && !Jump->isEnabled(); Block = Block.next())
      PrimarySource->setTextCursor(QTextCursor(Block));
    QVERIFY(Jump->isEnabled());
    QTest::mouseClick(Jump, Qt::LeftButton);
    QCOMPARE(Tabs->currentWidget(), Instructions);
    QVERIFY(Instructions->currentIndex().isValid());
    QCOMPARE(Instructions->currentIndex().column(), 0);
    Tabs->setCurrentWidget(Source);
    QTest::mouseClick(Alignment, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(SecondarySource->toPlainText().contains("return"),
                             Timeout);
    QVERIFY(PrimarySource->extraSelections().isEmpty());
    const auto SecondaryPage = SecondarySource->toPlainText();
    auto *PrimaryLine =
        View.findChild<QSpinBox *>("comparisonPrimarySourceLine");
    QVERIFY(PrimaryLine->value() > 1);
    PrimaryLine->setFocus();
    PrimaryLine->setValue(1);
    bool UnexpectedDialog = false;
    QTimer::singleShot(100, &View, [&] {
      if (auto *Modal =
              qobject_cast<QDialog *>(QApplication::activeModalWidget());
          Modal && Modal != &View) {
        UnexpectedDialog = true;
        qWarning() << "Source line Enter opened"
                   << Modal->metaObject()->className() << Modal->windowTitle();
        Modal->reject();
      }
    });
    QTest::keyClick(PrimaryLine, Qt::Key_Return);
    QTest::qWait(120);
    QVERIFY(!UnexpectedDialog);
    QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->toPlainText().startsWith("#define"),
                             Timeout);
    QCOMPARE(SecondarySource->toPlainText(), SecondaryPage);
    auto *Representations =
        View.findChild<QComboBox *>("comparisonSourceRepresentation");
    Representations->setCurrentIndex(Representations->findData("llvmc"));
    QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(SecondarySource->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Representations->setCurrentIndex(Representations->findData("rust"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QVERIFY(PrimarySource->toPlainText().isEmpty());
    QVERIFY(SecondarySource->toPlainText().isEmpty());
    QVERIFY(View.findChild<QLabel *>("comparisonPrimarySourceStatus")
                ->text()
                .contains("Source unavailable"));
    QVERIFY(!Jump->isEnabled());
    Tabs->setCurrentWidget(Instructions);
    Tabs->setCurrentWidget(Source);
    QVERIFY(!Session.cancellable());
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonSourceReload"),
                      Qt::LeftButton);
    QVERIFY(Session.cancellable());
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    // Multiple queued view changes must publish only the last selection.
    Representations->setCurrentIndex(Representations->findData("c"));
    Representations->setCurrentIndex(Representations->findData("llvmc"));
    QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(SecondarySource->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QTest::mouseClick(Alignment, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!PrimarySource->extraSelections().isEmpty(),
                             Timeout);
    QCOMPARE(PrimarySource->blockCount(), SecondarySource->blockCount());
    const int PrimaryMinimum = PrimarySource->minimumHeight();
    const int SecondaryMinimum = SecondarySource->minimumHeight();
    PrimarySource->setMinimumHeight(0);
    SecondarySource->setMinimumHeight(0);
    PrimarySource->setMaximumHeight(PrimarySource->fontMetrics().height() * 2);
    SecondarySource->setMaximumHeight(SecondarySource->fontMetrics().height() *
                                      2);
    QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->verticalScrollBar()->maximum() > 0,
                             Timeout);
    PrimarySource->verticalScrollBar()->setValue(1);
    QCOMPARE(SecondarySource->verticalScrollBar()->value(), 1);
    PrimarySource->setMaximumHeight(QWIDGETSIZE_MAX);
    SecondarySource->setMaximumHeight(QWIDGETSIZE_MAX);
    PrimarySource->setMinimumHeight(PrimaryMinimum);
    SecondarySource->setMinimumHeight(SecondaryMinimum);
    PrimarySource->verticalScrollBar()->setValue(0);
    const auto SourceCaptures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!SourceCaptures.isEmpty()) {
      QVERIFY(QDir().mkpath(SourceCaptures));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(40);
        const auto Name =
            Mode == Theme::Mode::Dark ? "dark-source" : "light-source";
        QVERIFY(View.grab().save(
            QDir(SourceCaptures).filePath(QString(Name) + ".png")));
      }
    }
    if (!SourceCaptures.isEmpty()) {
      const QString LongName = "saved_check_" +
                               QString("template_argument_").repeated(32) +
                               "suffix";
      Source->setFunctions({{"name", LongName}, {"address", "0x1000"}},
                           {{"name", LongName}, {"address", "0x1000"}});
      QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
      View.resize(View.minimumSize());
      QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->viewport()->height() >=
                                   PrimarySource->fontMetrics().height() * 6,
                               Timeout);
      auto *Title = View.findChild<QLabel *>("comparisonPrimarySourceTitle");
      QTRY_VERIFY_WITH_TIMEOUT(Title->text().contains(QChar(0x2026)), Timeout);
      QVERIFY(Title->toolTip().contains(LongName));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(40);
        QVERIFY(View.grab().save(
            QDir(SourceCaptures)
                .filePath(Mode == Theme::Mode::Dark
                              ? "dark-source-minimum-long.png"
                              : "light-source-minimum-long.png")));
      }
      View.resize(1280, 800);
      Source->setFunctions({{"name", "saved_check"}, {"address", "0x1000"}},
                           {{"name", "saved_check"}, {"address", "0x1000"}});
      QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    }
    Tabs->setCurrentWidget(Instructions);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (const auto &ThemeName : {QString("light"), QString("dark")}) {
        Theme::instance().setMode(ThemeName == "dark" ? Theme::Mode::Dark
                                                      : Theme::Mode::Light);
        QTest::qWait(40);
        QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
        QVERIFY(View.grab().save(Captures + "/" + ThemeName +
                                 "-saved-projects.png"));
      }
    }
    QVERIFY(QFile::remove(Sentinel));
  }

  void counterpartPickerPagesSearchesAndPreservesMappingIntent() {
    constexpr int Count = 417;
    const auto A = Directory.filePath("candidate-before.bin");
    const auto B = Directory.filePath("candidate-after.bin");
    auto Bytes =
        QByteArray::fromHex("b80700000083c003c3cccccccccccccc").repeated(Count);
    QVERIFY(write(A, Bytes));
    Bytes.back() = char(0xdd);
    QVERIFY(write(B, Bytes));
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [](const QString &Path, const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"platform", "sysv"},
                                                      {"base", Base}}}};
    };
    Session.start(Input(A, "0x1000"), Input(B, "0x0"), {{"use_names", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    Session.command(
        "comparison_override",
        {{"kind", "match"}, {"primary", "0x1000"}, {"secondary", "0x0"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    const auto OriginalCounts = Session.summary()["counts"];
    const auto OriginalRevision = Session.revision();
    auto *Inventory = View.findChild<QTableView *>("comparisonFunctions");
    auto *Functions = qobject_cast<ComparisonModel *>(Inventory->model());
    Inventory->sortByColumn(0, Qt::AscendingOrder);
    QTRY_VERIFY_WITH_TIMEOUT(Functions->rowCount() >= Count, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Functions->row(1)["address"].toString(),
                              QString("0x1010"), Timeout);
    Inventory->selectRow(1);
    auto *Open = View.findChild<QPushButton *>("comparisonPairWith");
    QTRY_VERIFY_WITH_TIMEOUT(Open->isEnabled(), Timeout);
    QTest::mouseClick(Open, Qt::LeftButton);
    QTRY_VERIFY(View.findChild<ComparisonCounterpartDialog *>());
    auto *Picker = View.findChild<ComparisonCounterpartDialog *>();
    auto *Table = Picker->findChild<QTableView *>("counterpartFunctions");
    auto *Model = qobject_cast<ComparisonModel *>(Table->model());
    auto *Search = Picker->findChild<QLineEdit *>("counterpartSearch");
    auto *Sort = Picker->findChild<QComboBox *>("counterpartSort");
    auto *Pair = Picker->findChild<QPushButton *>("counterpartPair");
    auto *Status = Picker->findChild<QLabel *>("counterpartStatus");
    QTRY_VERIFY_WITH_TIMEOUT(Model->rowCount() >= Count - 1, Timeout);
    QVERIFY(Status->text().contains("hypotheses"));
    QVERIFY(!Pair->isEnabled());
    const int Last = Model->rowCount() - 1;
    Table->scrollTo(Model->index(Last, 1));
    QTRY_VERIFY_WITH_TIMEOUT(!Model->row(Last).isEmpty(), Timeout);
    const auto LastAddress = Model->row(Last)["address"].toString();
    Table->selectRow(Last);
    QTRY_VERIFY(Pair->isEnabled());
    QVERIFY(Picker->findChild<QLabel *>("counterpartSelection")
                ->text()
                .contains(LastAddress));
    QVERIFY(Model->cachedPages() >= 2);
    Sort->setCurrentIndex(1);
    QVERIFY(!Pair->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(Model->row(0)["address"].toString(), LastAddress,
                              Timeout);
    Search->setText("0x20");
    QTest::keyClick(Search, Qt::Key_Return);
    QCoreApplication::processEvents();
    Search->setText("no matching function");
    QVERIFY(!Pair->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(Status->text().contains("No ambiguity candidates"),
                             Timeout);
    QCOMPARE(Model->rowCount(), 0);
    QCOMPARE(Session.revision(), OriginalRevision);
    Search->setText(LastAddress);
    QTRY_COMPARE_WITH_TIMEOUT(Model->rowCount(), 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Model->row(0)["address"].toString(), LastAddress,
                              Timeout);
    Table->selectRow(0);
    QTRY_VERIFY(Pair->isEnabled());
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        const auto Prefix = Mode == Theme::Mode::Dark ? "dark" : "light";
        Picker->resize(940, 620);
        QTest::qWait(100);
        QVERIFY(
            Picker->grab().save(Captures + '/' + Prefix + "-counterpart.png"));
        Picker->resize(Picker->minimumSize());
        QTest::qWait(100);
        QVERIFY(Picker->grab().save(Captures + '/' + Prefix +
                                    "-counterpart-minimum.png"));
      }
    }
    QTest::mouseClick(Pair, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        !Session.busy() && Session.revision() != OriginalRevision, Timeout);
    QJsonObject Paired;
    Session.request("comparison_functions",
                    {{"side", "primary"}, {"search", "0x1010"}}, &View,
                    [&](const QJsonObject &Message) {
                      Paired = Message["payload"].toObject();
                    });
    QTRY_VERIFY_WITH_TIMEOUT(Paired.contains("items"), Timeout);
    QCOMPARE(Paired["items"]
                 .toArray()
                 .first()
                 .toObject()["counterpart"]
                 .toObject()["address"]
                 .toString(),
             LastAddress);
    Session.command("comparison_undo");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.summary()["counts"], OriginalCounts);
    // All-function search admits address zero and explains displaced pairs.
    ComparisonCounterpartDialog Manual(Session, "primary", "0x1010", {}, &View);
    Manual.show();
    auto *All = Manual.findChild<QComboBox *>("counterpartScope");
    auto *ManualSearch = Manual.findChild<QLineEdit *>("counterpartSearch");
    auto *ManualTable = Manual.findChild<QTableView *>("counterpartFunctions");
    auto *ManualModel = qobject_cast<ComparisonModel *>(ManualTable->model());
    auto *ManualPair = Manual.findChild<QPushButton *>("counterpartPair");
    All->setCurrentIndex(1);
    ManualSearch->setText("0x0");
    QTRY_COMPARE_WITH_TIMEOUT(ManualModel->rowCount(), 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(ManualModel->row(0)["address"].toString(),
                              QString("0x0"), Timeout);
    ManualTable->selectRow(0);
    QTRY_VERIFY(ManualPair->isEnabled());
    QVERIFY(Manual.findChild<QLabel *>("counterpartSelection")
                ->text()
                .contains("0x1000"));
    const auto BeforeManual = Session.revision();
    QTest::keyClick(ManualSearch, Qt::Key_Return);
    QCOMPARE(Session.revision(), BeforeManual);
    QTRY_COMPARE_WITH_TIMEOUT(ManualModel->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!ManualModel->row(0).isEmpty(), Timeout);
    ManualTable->selectRow(0);
    QTest::mouseClick(ManualPair, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        !Session.busy() && Session.revision() != BeforeManual, Timeout);
    Session.command("comparison_undo");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.summary()["counts"], OriginalCounts);
    // Browsing the reverse direction and cancelling never records an edit.
    ComparisonCounterpartDialog Existing(Session, "primary", "0x1000", "0x0",
                                         &View);
    Existing.show();
    QTRY_VERIFY_WITH_TIMEOUT(Existing.findChild<QLabel *>("counterpartStatus")
                                 ->text()
                                 .contains("no retained ambiguity group"),
                             Timeout);
    Existing.findChild<QComboBox *>("counterpartScope")->setCurrentIndex(1);
    Existing.findChild<QLineEdit *>("counterpartSearch")->setText("0x0");
    auto *ExistingTable =
        Existing.findChild<QTableView *>("counterpartFunctions");
    auto *ExistingModel =
        qobject_cast<ComparisonModel *>(ExistingTable->model());
    QTRY_COMPARE_WITH_TIMEOUT(ExistingModel->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!ExistingModel->row(0).isEmpty(), Timeout);
    ExistingTable->selectRow(0);
    QVERIFY(!Existing.findChild<QPushButton *>("counterpartPair")->isEnabled());
    QVERIFY(Existing.findChild<QLabel *>("counterpartSelection")
                ->text()
                .contains("already paired"));
    QTest::keyClick(&Existing, Qt::Key_Escape);
    ComparisonCounterpartDialog Cancelled(Session, "secondary", "0x10", {},
                                          &View);
    Cancelled.show();
    auto *ReverseTable =
        Cancelled.findChild<QTableView *>("counterpartFunctions");
    auto *ReverseModel = qobject_cast<ComparisonModel *>(ReverseTable->model());
    QTRY_VERIFY_WITH_TIMEOUT(ReverseModel->rowCount() >= Count - 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(ReverseModel->row(0)["address"].toString(),
                              QString("0x1010"), Timeout);
    const auto BeforeCancel = Session.revision();
    QTest::keyClick(&Cancelled, Qt::Key_Escape);
    QVERIFY(!Cancelled.isVisible());
    QCOMPARE(Session.revision(), BeforeCancel);
    ComparisonCounterpartDialog Stale(Session, "primary", "0x1010", {}, &View);
    Stale.show();
    auto *StaleTable = Stale.findChild<QTableView *>("counterpartFunctions");
    auto *StaleModel = qobject_cast<ComparisonModel *>(StaleTable->model());
    auto *StalePair = Stale.findChild<QPushButton *>("counterpartPair");
    QTRY_VERIFY_WITH_TIMEOUT(!StaleModel->row(0).isEmpty(), Timeout);
    StaleTable->selectRow(0);
    QTRY_VERIFY(StalePair->isEnabled());
    Session.command(
        "comparison_override",
        {{"kind", "match"}, {"primary", "0x1010"}, {"secondary", "0x20"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(!StalePair->isEnabled());
    QCOMPARE(StaleModel->rowCount(), 0);
    QVERIFY(Stale.findChild<QLabel *>("counterpartStatus")
                ->text()
                .contains("comparison changed"));
    QCOMPARE(Failures.count(), 0);
  }
  void scalarValidationShowsAssumptionsAndClearsStaleResults() {
    const auto A = Directory.filePath("scalar-before.bin");
    const auto B = Directory.filePath("scalar-after.bin");
    QVERIFY(write(A, QByteArray::fromHex("89f883c003c3")));
    QVERIFY(write(B, QByteArray::fromHex("89f883c00183c002c3")));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [](const QString &Path) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"platform", "sysv"},
                                                      {"base", 4096}}}};
    };
    Session.start(Input(A), Input(B), {{"discover", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    Session.command(
        "comparison_override",
        {{"kind", "match"}, {"primary", "0x1000"}, {"secondary", "0x1000"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    ComparisonValidationDialog View(Session, nullptr);
    View.show();
    View.review("primary", "0x1000");
    auto *Report = View.findChild<QPlainTextEdit *>("validationReport");
    auto *Evidence = View.findChild<QPlainTextEdit *>("validationEvidence");
    auto *Status = View.findChild<QLabel *>("validationStatus");
    auto *Run = View.findChild<QPushButton *>("validationRun");
    QTRY_VERIFY_WITH_TIMEOUT(!Report->toPlainText().isEmpty(), Timeout);
    auto Result =
        QJsonDocument::fromJson(Report->toPlainText().toUtf8()).object();
    QCOMPARE(Result["status"].toString(), QString("proved"));
    QCOMPARE(Result["input_source_binding"].toString(),
             QString("owned_sessions"));
    QVERIFY(Result["inputs_bound"].toBool());
    QVERIFY(!Result["inputs_revalidated"].toBool());
    QVERIFY(Status->text().contains("lifted scalar models"));
    QVERIFY(Evidence->toPlainText().contains("defined integer bitvectors"));
    QVERIFY(Evidence->toPlainText().contains("register offset 56"));
    QVERIFY(Evidence->toPlainText().contains("does not assess vulnerability"));
    QVERIFY(Evidence->toPlainText().contains("retained comparison sources"));
    QVERIFY(Run->isEnabled());
    auto *Work = View.findChild<QSpinBox *>("validationWork");
    auto *Instructions = View.findChild<QSpinBox *>("validationInstructions");
    QVERIFY(Work && Instructions);
    const auto Limits = Result["limits"].toObject();
    QCOMPARE(Work->value(), Limits["proof_work"].toInt());
    QCOMPARE(Instructions->value(),
             Limits["native_instructions_per_function"].toInt());
    for (auto *Limit : {Work, Instructions}) {
      const auto Previous = Limit->value();
      Limit->setValue(1);
      QVERIFY(Report->toPlainText().isEmpty());
      QVERIFY(Evidence->toPlainText().isEmpty());
      QVERIFY(Status->text().contains("Limits changed"));
      QTest::mouseClick(Run, Qt::LeftButton);
      QVERIFY(!Work->isEnabled());
      QVERIFY(!Instructions->isEnabled());
      QTRY_VERIFY_WITH_TIMEOUT(!Report->toPlainText().isEmpty(), Timeout);
      Result = QJsonDocument::fromJson(Report->toPlainText().toUtf8()).object();
      QCOMPARE(Result["status"].toString(), QString("budget_exceeded"));
      QCOMPARE(
          Result["limits"]
              .toObject()[Limit == Work ? "proof_work"
                                        : "native_instructions_per_function"]
              .toInt(),
          1);
      QVERIFY(Status->text().contains("Adjust the limits"));
      Limit->setValue(Previous);
      QVERIFY(Report->toPlainText().isEmpty());
    }
    QTest::mouseClick(Run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Report->toPlainText().isEmpty(), Timeout);
    Result = QJsonDocument::fromJson(Report->toPlainText().toUtf8()).object();
    QCOMPARE(Result["status"].toString(), QString("proved"));
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        const auto Prefix = Mode == Theme::Mode::Dark ? "dark" : "light";
        View.resize(920, 680);
        QTest::qWait(100);
        QVERIFY(View.grab().save(Captures + '/' + Prefix + "-validation.png"));
        View.resize(View.minimumSize());
        QTest::qWait(100);
        QVERIFY(View.grab().save(Captures + '/' + Prefix +
                                 "-validation-minimum.png"));
      }
    }
    Session.command(
        "comparison_override",
        {{"kind", "confirm"}, {"primary", "0x1000"}, {"secondary", "0x1000"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_VERIFY(Report->toPlainText().isEmpty());
    QVERIFY(Evidence->toPlainText().isEmpty());
    QVERIFY(!Run->isEnabled());
    View.review("primary", "0x1000");
    QTRY_VERIFY_WITH_TIMEOUT(!Report->toPlainText().isEmpty(), Timeout);
    QCOMPARE(QJsonDocument::fromJson(Report->toPlainText().toUtf8())
                 .object()["status"]
                 .toString(),
             QString("proved"));
    const auto Unsupported = Directory.filePath("scalar-memory.bin");
    QVERIFY(write(Unsupported, QByteArray::fromHex("8b07c3")));
    Session.start(Input(Unsupported), Input(Unsupported),
                  {{"discover", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_VERIFY(Report->toPlainText().isEmpty());
    View.review("primary", "0x1000");
    QTRY_VERIFY_WITH_TIMEOUT(!Report->toPlainText().isEmpty(), Timeout);
    QCOMPARE(QJsonDocument::fromJson(Report->toPlainText().toUtf8())
                 .object()["status"]
                 .toString(),
             QString("unsupported"));
    QVERIFY(Status->text().contains("outside the supported scalar model"));
  }

  void largeScalarValidationRemainsInteractive() {
    const auto Input = qEnvironmentVariable("NEVERD_DIFF_SCALAR_PROBE_INPUT");
    const auto Address =
        qEnvironmentVariable("NEVERD_DIFF_SCALAR_PROBE_ADDRESS");
    if (Input.isEmpty() && Address.isEmpty())
      QSKIP("Opt-in local large-input scalar GUI probe.");
    QVERIFY2(!Input.isEmpty() && !Address.isEmpty(),
             "Set both probe input and scalar function address.");
    const auto WaitUntil = [](auto Ready) {
      QElapsedTimer Wait;
      Wait.start();
      while (!Ready() && Wait.elapsed() < Timeout)
        QTest::qWait(1);
      return Ready();
    };
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QVERIFY(WaitUntil([&] { return Session.ready(); }));
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(Input);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(Input);
    QElapsedTimer Clock;
    Clock.start();
    qint64 LastTick = 0, MaxGap = 0;
    int Ticks = 0;
    QTimer Heartbeat;
    Heartbeat.setInterval(1);
    connect(&Heartbeat, &QTimer::timeout, this, [&] {
      const auto Now = Clock.elapsed();
      MaxGap = std::max(MaxGap, Now - LastTick);
      LastTick = Now;
      ++Ticks;
    });
    Heartbeat.start();
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QVERIFY(WaitUntil([&] {
      return !Session.busy() && !Session.cancellable() &&
             !Session.revision().isEmpty();
    }));
    QVERIFY(Failures.empty());
    const auto Preparation = Clock.elapsed();
    ComparisonValidationDialog Validation(Session, &View);
    Validation.show();
    auto *Report = Validation.findChild<QPlainTextEdit *>("validationReport");
    auto *Run = Validation.findChild<QPushButton *>("validationRun");
    QJsonArray Times;
    QJsonObject Result;
    for (int Attempt = 0; Attempt < 3; ++Attempt) {
      const auto Begin = Clock.elapsed();
      if (!Attempt)
        Validation.review("primary", Address);
      else {
        QVERIFY(Run->isEnabled());
        QTest::mouseClick(Run, Qt::LeftButton);
      }
      QVERIFY(WaitUntil([&] { return !Report->toPlainText().isEmpty(); }));
      QTest::qWait(1); // Include the result's next paint/event-loop turn.
      Times.append(Clock.elapsed() - Begin);
      Result = QJsonDocument::fromJson(Report->toPlainText().toUtf8()).object();
      QCOMPARE(Result["status"].toString(), QString("proved"));
      QCOMPARE(Result["input_source_binding"].toString(),
               QString("owned_sessions"));
    }
    Heartbeat.stop();
    MaxGap = std::max(MaxGap, Clock.elapsed() - LastTick);
    QVERIFY(Ticks > 0);
    QVERIFY(Failures.empty());
    const auto Measurement =
        QJsonDocument(
            QJsonObject{
                {"track",
                 "local offscreen GUI probe; not competitive qualification"},
                {"input", Input},
                {"address", Address},
                {"preparation_ms", Preparation},
                {"fresh_query_round_trip_ms", Times},
                {"heartbeat_max_gap_ms", MaxGap},
                {"heartbeat_ticks", Ticks},
                {"primary_sha256", Result["primary_sha256"]},
                {"primary_lift", Result["primary_lift"]},
                {"proof_work", Result["work"]}})
            .toJson(QJsonDocument::Indented);
    qInfo().noquote() << Measurement;
    const auto Output = qEnvironmentVariable("NEVERD_DIFF_SCALAR_PROBE_REPORT");
    if (!Output.isEmpty())
      QVERIFY(write(Output, Measurement));
  }

  void inputsPagesEditingAndRender() {
    const auto A = Directory.filePath("original.bin"),
               B = Directory.filePath("patched.bin");
    // cmp edi, limit; je second; mov eax, 2; ret; second: mov eax, 3; ret
    auto Bytes = QByteArray::fromHex("83ff117406b802000000c3b803000000c3");
    QVERIFY(write(A, Bytes));
    Bytes[2] = 9;
    QVERIFY(write(B, Bytes));
    QPointer<ComparisonDialog> Dialog =
        new ComparisonDialog(TEST_COMPARISON_WORKER);
    Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    QSignalSpy Failure(&Dialog->session(), &ComparisonSession::failed);
    Dialog->show();
    QTRY_VERIFY_WITH_TIMEOUT(Dialog->session().ready(), Timeout);
    auto *Primary = Dialog->findChild<QLineEdit *>("comparisonPrimary");
    auto *Secondary = Dialog->findChild<QLineEdit *>("comparisonSecondary");
    Primary->setText(A);
    Secondary->setText(B);
    for (const char *Name : {"primaryLoader", "secondaryLoader"})
      Dialog->findChild<QComboBox *>(Name)->setCurrentIndex(1);
    for (const char *Name : {"primaryBase", "secondaryBase"})
      Dialog->findChild<QLineEdit *>(Name)->setText("0x1000");
    auto *Start = Dialog->findChild<QPushButton *>("comparisonStart");
    QVERIFY(Start->isEnabled());
    int Ticks = 0;
    QTimer Heartbeat;
    Heartbeat.setInterval(1);
    connect(&Heartbeat, &QTimer::timeout, this, [&] { ++Ticks; });
    Heartbeat.start();
    QTest::mouseClick(Start, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog->session().busy(), Timeout);
    QVERIFY2(Failure.empty(),
             qPrintable(Failure.empty() ? QString()
                                        : Failure.first()[0].toString()));
    QVERIFY(Ticks > 0);
    QCOMPARE(
        Dialog->session().summary()["counts"].toObject()["changed"].toInt(), 1);
    auto *Functions = Dialog->findChild<QTableView *>("comparisonFunctions");
    auto *Instructions =
        Dialog->findChild<QTableView *>("comparisonInstructions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->setCurrentIndex(Functions->model()->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(Instructions->model()->rowCount() >= 6, Timeout);
    auto *Rows = qobject_cast<ComparisonModel *>(Instructions->model());
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QVERIFY(Rows->cachedPages() <= 16);
    QCOMPARE(Rows->row(0)["kind"].toString(), QString("constant"));
    const auto Revision = Dialog->session().revision();
    Dialog->session().command(
        "comparison_override",
        {{"kind", "confirm"}, {"primary", "0x1000"}, {"secondary", "0x1000"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog->session().busy(), Timeout);
    QVERIFY(Revision != Dialog->session().revision());
    auto *Undo = Dialog->findChild<QPushButton *>("comparisonUndo");
    QTRY_VERIFY(Undo->isEnabled());
    QTest::mouseClick(Undo, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog->session().busy(), Timeout);
    QVERIFY(Dialog->findChild<QPushButton *>("comparisonRedo")->isEnabled());
    const auto Saved = Directory.filePath("review.nddiff");
    Dialog->session().command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog->session().busy(), Timeout);
    QVERIFY(QFile::exists(Saved));
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->setCurrentIndex(Functions->model()->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(Instructions->model()->rowCount() >= 6, Timeout);

    auto *GraphTabs = Dialog->findChild<QTabWidget *>();
    // The offscreen platform has a small virtual screen; choose a known
    // review window before checking fit rather than assuming its screen size.
    Dialog->resize(1280, 800);
    GraphTabs->setCurrentIndex(1);
    for (auto *Graph : Dialog->findChildren<ComparisonGraph *>())
      QTRY_VERIFY_WITH_TIMEOUT(Graph->zoom() >= 0.4, Timeout);
    GraphTabs->setCurrentIndex(0);
    // Real renders use the production palette, fonts, icons and native widgets.
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        const auto Prefix = Mode == Theme::Mode::Dark ? "dark" : "light";
        Dialog->resize(1280, 800);
        QTest::qWait(100);
        QVERIFY(
            Dialog->grab().save(Captures + '/' + Prefix + "-disassembly.png"));
        auto *Tabs = Dialog->findChild<QTabWidget *>();
        Tabs->setCurrentIndex(1);
        QTest::qWait(200);
        QVERIFY(Dialog->grab().save(Captures + '/' + Prefix + "-cfg.png"));
        Tabs->setCurrentIndex(0);
        Dialog->resize(760, 540);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(Captures + '/' + Prefix + "-minimum.png"));
        auto *Settings = Dialog->findChild<QToolButton *>("comparisonSettings");
        Settings->setChecked(true);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(Captures + '/' + Prefix + "-settings.png"));
        Settings->setChecked(false);
      }
    }
    const auto Before = Instructions->size();
    Dialog->resize(1400, 900);
    QTest::qWait(50);
    QVERIFY(Instructions->width() > Before.width() ||
            Instructions->height() > Before.height());
    Primary->setFocus();
    QTest::keyClick(Primary, Qt::Key_Tab);
    QVERIFY(QApplication::focusWidget() != Primary);
    Dialog->close();
    delete Dialog;
    QCOMPARE(Failure.count(), 0);
  }
  void cancellingInputLoadingKeepsThePreviousComparison() {
    const auto Large = Directory.filePath("loading-large.bin");
    QFile Sparse(Large);
    QVERIFY(Sparse.open(QIODevice::WriteOnly));
    QVERIFY(Sparse.resize(64 * 1024 * 1024));
    Sparse.close();
    const auto Small = Directory.filePath("loading-small.bin");
    QVERIFY(write(Small, QByteArray::fromHex("b807000000c3")));
    ComparisonDialog Dialog(TEST_COMPARISON_WORKER);
    Dialog.show();
    QTRY_VERIFY_WITH_TIMEOUT(Dialog.session().ready(), Timeout);
    QSignalSpy Failures(&Dialog.session(), &ComparisonSession::failed);
    auto *Primary = Dialog.findChild<QLineEdit *>("comparisonPrimary");
    auto *Secondary = Dialog.findChild<QLineEdit *>("comparisonSecondary");
    for (const char *Name : {"primaryLoader", "secondaryLoader"})
      Dialog.findChild<QComboBox *>(Name)->setCurrentIndex(1);
    auto *Start = Dialog.findChild<QPushButton *>("comparisonStart");
    auto *Cancel = Dialog.findChild<QPushButton *>("comparisonCancel");
    auto *Status = Dialog.findChild<QLabel *>("comparisonStatus");
    for (const bool Retain : {false, true}) {
      const auto Before = Dialog.session().summary();
      QCOMPARE(!Before.isEmpty(), Retain);
      Primary->setText(Large);
      Secondary->setText(Small);
      bool Clicked = false, StillRunning = false, Enabled = false;
      QString StageText;
      const auto Connection = connect(
          &Dialog.session(), &ComparisonSession::progress, &Dialog,
          [&](const auto &P) {
            if (Clicked || P["phase"] != QJsonValue("primary:load:hash") ||
                P["done"].toDouble() <= 0)
              return;
            Clicked = true;
            StageText = Status->text();
            Enabled = Cancel->isEnabled();
            QTest::mouseClick(Cancel, Qt::LeftButton);
            StillRunning = Dialog.session().busy();
          });
      QTest::mouseClick(Start, Qt::LeftButton);
      QTRY_VERIFY_WITH_TIMEOUT(Clicked && !Dialog.session().busy(), Timeout);
      disconnect(Connection);
      QVERIFY(Enabled);
      QVERIFY(StillRunning);
      QVERIFY(StageText.contains("Checking file identity"));
      QVERIFY(!StageText.contains("primary:load"));
      QCOMPARE(Dialog.session().summary(), Before);
      QVERIFY(Status->text().contains("Loading stopped"));
      QVERIFY(Status->text().contains(Retain ? "previous comparison"
                                             : "try again"));
      QVERIFY(Start->isEnabled());
      QVERIFY(!Cancel->isEnabled());
      QCOMPARE(Failures.count(), 0);
      const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
      if (Retain && !Captures.isEmpty()) {
        QDir().mkpath(Captures);
        QVERIFY(Dialog.grab().save(Captures + "/loading-stopped-retained.png"));
      }
      // The same UI can start again, clearing the stopped state on completion.
      Primary->setText(Small);
      QTest::mouseClick(Start, Qt::LeftButton);
      QTRY_VERIFY_WITH_TIMEOUT(!Dialog.session().busy(), Timeout);
      QCOMPARE(Dialog.session().summary()["state"].toString(),
               QString("complete"));
      QVERIFY(Dialog.session().operationMessage().isEmpty());
      QCOMPARE(Failures.count(), 0);
    }
    // A failed replacement has the same retention guarantee and its reason
    // remains visible after the request bookkeeping emits another update.
    const auto BeforeFailure = Dialog.session().summary();
    Primary->setText(Directory.filePath("missing-input.bin"));
    QTest::mouseClick(Start, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog.session().busy(), Timeout);
    QCOMPARE(Failures.count(), 1);
    QCOMPARE(Dialog.session().summary(), BeforeFailure);
    QVERIFY(Status->text().contains("Could not replace"));
    QVERIFY(Status->text().contains("previous comparison"));
    Dialog.close();
  }

  void cancelButtonWaitsForActualStop() {
    const auto Path = Directory.filePath("large.bin");
    QVERIFY(write(Path, QByteArray(8 * 1024 * 1024, char(0x90)) + char(0xc3)));
    auto *Dialog = new ComparisonDialog(TEST_COMPARISON_WORKER);
    Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    Dialog->show();
    QTRY_VERIFY_WITH_TIMEOUT(Dialog->session().ready(), Timeout);
    Dialog->findChild<QLineEdit *>("comparisonPrimary")->setText(Path);
    Dialog->findChild<QLineEdit *>("comparisonSecondary")->setText(Path);
    for (const char *Name : {"primaryLoader", "secondaryLoader"})
      Dialog->findChild<QComboBox *>(Name)->setCurrentIndex(1);
    auto *Cancel = Dialog->findChild<QPushButton *>("comparisonCancel");
    bool Clicked = false, Enabled = false, StillRunning = false;
    connect(&Dialog->session(), &ComparisonSession::progress, Dialog,
            [&](const auto &P) {
              if (Clicked || P["phase"] != QJsonValue("primary:discovery"))
                return;
              Clicked = true;
              Enabled = Cancel->isEnabled();
              QTest::mouseClick(Cancel, Qt::LeftButton);
              StillRunning = Dialog->session().busy();
            });
    QTest::mouseClick(Dialog->findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(Clicked && !Dialog->session().busy(), Timeout);
    QVERIFY(Enabled);
    QVERIFY(StillRunning);
    QCOMPARE(Dialog->session().summary()["state"].toString(),
             QString("cancelled"));
    const auto Job = Dialog->session().summary()["job"].toObject();
    QVERIFY(Job["computation_stopped"].toBool());
    QVERIFY(Job["partial_result_retained"].toBool());
    QVERIFY(Dialog->findChild<QLabel *>("comparisonStatus")
                ->text()
                .contains("cancelled"));
    Dialog->close();
    delete Dialog;
  }

  void regionReviewUsesBoundedPagesAndOriginalBytes() {
    const auto Path = Directory.filePath("region.bin");
    QVERIFY(write(Path, QByteArray::fromHex("89f883c00383f0176bc00783c009c3")));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"), {{"regions", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Failure.empty());
    QCOMPARE(
        Session.summary()["regions"].toObject()["hypothesis_count"].toInt(), 1);
    auto *Dialog = new ComparisonRegionsDialog(Session, nullptr);
    Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    Dialog->show();
    auto *Functions = Dialog->findChild<QTableView *>("regionHypotheses");
    auto *Spans = Dialog->findChild<QTableView *>("regionAnchors");
    auto *Instructions = Dialog->findChild<QTableView *>("regionInstructions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Spans->model()->rowCount(), 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Instructions->model()->rowCount(), 5, Timeout);
    auto *Rows = qobject_cast<ComparisonModel *>(Instructions->model());
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["primary"].toObject()["bytes"].toString(),
             QString("89F8"));
    QCOMPARE(Rows->row(0)["secondary"].toObject()["address"].toString(),
             QString("0x8000"));
    QVERIFY(Rows->cachedPages() <= 16);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(Captures + (Mode == Theme::Mode::Dark
                                                    ? "/dark-regions.png"
                                                    : "/light-regions.png")));
      }
    auto *Kind = Dialog->findChild<QComboBox *>("regionKind");
    Kind->setCurrentIndex(
        1); // This one-to-one fixture has no split hypothesis.
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 0, Timeout);
    QCOMPARE(Spans->model()->rowCount(), 0);
    QCOMPARE(Instructions->model()->rowCount(), 0);
    QVERIFY(write(
        Path,
        QByteArray::fromHex(
            "89f883c00383f0176bc00783c009eb0089f283c21383f25d6bd20b83c21bc3")));
    Session.start(
        Input("0x1000"), Input("0x8000"),
        {{"discover", false}, {"regions", QJsonObject{{"max_anchors", 1}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.summary()["state"].toString(),
             QString("resource_limited"));
    QTRY_COMPARE_WITH_TIMEOUT(Kind->currentData().toString(),
                              QString("raw_anchors"), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Spans->model()->rowCount(), 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Instructions->model()->rowCount(), 5, Timeout);
    QVERIFY(!Functions->isEnabled());
    Session.start(Input("0x1000"), Input("0x8000"), {{"regions", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Spans->model()->rowCount(), 0, Timeout);
    QCOMPARE(Instructions->model()->rowCount(), 0);
    QCOMPARE(Functions->model()->rowCount(), 0);
    QVERIFY(!Kind->isEnabled());
    Dialog->close();
    delete Dialog;
    QCOMPARE(Failure.count(), 0);
  }

  void ssaRegionReviewPagesNodePairsAndRetiresDisabledEvidence() {
    const auto Path = Directory.filePath("ssa-regions.bin");
    QVERIFY(write(Path, QByteArray::fromHex("89f883c00383f0176bc00783c009c3")));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base},
                                                      {"platform", "sysv"}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"), {{"flow_regions", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Failure.empty());
    auto *Dialog = new ComparisonRegionsDialog(Session, nullptr, true);
    Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    Dialog->show();
    auto *Hypotheses = Dialog->findChild<QTableView *>("regionHypotheses");
    auto *Anchors = Dialog->findChild<QTableView *>("regionAnchors");
    auto *Nodes = Dialog->findChild<QTableView *>("regionInstructions");
    auto *Rows = qobject_cast<ComparisonModel *>(Nodes->model());
    QTRY_VERIFY_WITH_TIMEOUT(Hypotheses->model()->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Anchors->model()->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() >= 4, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    int Original = 0;
    for (int I = 0; I < Rows->rowCount(); ++I) {
      const auto A = Rows->row(I)["primary"].toObject()["original"].toObject();
      if (!A.isEmpty()) {
        QVERIFY(!A["bytes"].toString().isEmpty());
        ++Original;
      }
    }
    QVERIFY(Original >= 2);
    QVERIFY(Rows->cachedPages() <= 16);
    QVERIFY(Dialog->findChild<QLabel *>("regionSummary")
                ->text()
                .contains("Dependency scope"));
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(
            Captures + (Mode == Theme::Mode::Dark ? "/dark-ssa-regions.png"
                                                  : "/light-ssa-regions.png")));
      }
    auto *Kind = Dialog->findChild<QComboBox *>("regionKind");
    Session.start(Input("0x1000"), Input("0x8000"),
                  {{"flow_regions", QJsonObject{{"max_anchors", 1}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Kind->currentData().toString(),
                              QString("raw_anchors"), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Anchors->model()->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() >= 4, Timeout);
    Session.start(Input("0x1000"), Input("0x8000"), {{"flow_regions", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 0, Timeout);
    QCOMPARE(Anchors->model()->rowCount(), 0);
    QVERIFY(!Kind->isEnabled());
    Dialog->close();
    delete Dialog;
    QCOMPARE(Failure.count(), 0);
  }

  void stoppedFunctionDifferenceCanBeRetriedWithoutOldGraphClaims() {
    const auto Path = Directory.filePath("large-detail.bin");
    auto Code = QByteArray(60000, char(0x90));
    Code.append(char(0xc3));
    QVERIFY(write(Path, Code));
    auto *Dialog = new ComparisonDialog(TEST_COMPARISON_WORKER);
    Dialog->show();
    auto &Session = Dialog->session();
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base},
                                                      {"platform", "sysv"}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"), {{"discover", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    auto *Table = Dialog->findChild<QTableView *>("comparisonInstructions");
    auto *Rows = qobject_cast<ComparisonModel *>(Table->model());
    auto *Retry = Dialog->findChild<QPushButton *>("comparisonRetryDetail");
    Rows->setQuery(
        {{"address", "0x1000"}, {"timeout_ms", 1}, {"refresh", true}});
    QTRY_VERIFY_WITH_TIMEOUT(Retry->isEnabled(), Timeout);
    QCOMPARE(Rows->rowCount(), 0);
    QVERIFY(Dialog->findChild<QLabel *>("comparisonStatus")
                ->text()
                .contains("Function difference stopped"));
    QVERIFY(Dialog->findChild<QLabel *>("comparisonFunctionTitle")
                ->text()
                .contains("timed out"));
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(Captures +
                                    (Mode == Theme::Mode::Dark
                                         ? "/dark-detail-stopped.png"
                                         : "/light-detail-stopped.png")));
      }
    QTest::mouseClick(Retry, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), int(Code.size()), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Retry->isEnabled() && !Session.cancellable(),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["kind"].toString(), QString("identical"));
    QVERIFY(!Dialog->findChild<QLabel *>("comparisonFunctionTitle")
                 ->text()
                 .contains("timed out"));
    QCOMPARE(Failure.count(), 0);
    delete Dialog;
  }

  void changeReviewPreservesSyntheticAnchorsAndNavigatesEvidence() {
    const auto A = Directory.filePath("change-review-a.bin"),
               B = Directory.filePath("change-review-b.bin");
    auto Bytes = QByteArray::fromHex("83ff117406b802000000c3b803000000c3");
    QVERIFY(write(A, Bytes));
    Bytes[2] = 9;
    QVERIFY(write(B, Bytes));
    auto *Parent = new ComparisonDialog(TEST_COMPARISON_WORKER);
    Parent->setAttribute(Qt::WA_DeleteOnClose, false);
    Parent->show();
    auto &Session = Parent->session();
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [](const QString &Path, const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"platform", "sysv"},
                                                      {"base", Base}}}};
    };
    Session.start(Input(A, "0x1000"), Input(B, "0x8000"), {{"dataflow", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    auto *Functions = Parent->findChild<QTableView *>("comparisonFunctions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->selectRow(0);
    auto *Open = Parent->findChild<QPushButton *>("comparisonChangesReview");
    QTRY_VERIFY_WITH_TIMEOUT(Open->isEnabled(), Timeout);
    QTest::mouseClick(Open, Qt::LeftButton);
    auto *Dialog = Parent->findChild<ComparisonChangesDialog *>();
    QVERIFY(Dialog);
    auto *Table = Dialog->findChild<QTableView *>("changesTable");
    auto *Rows = qobject_cast<ComparisonModel *>(Table->model());
    QSignalSpy ReviewFailure(Rows, &ComparisonModel::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() || !ReviewFailure.empty(),
                             Timeout);
    QVERIFY2(ReviewFailure.empty(),
             qPrintable(Dialog->findChild<QLabel *>("changesSummary")->text()));
    QCOMPARE(Rows->rowCount(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["association"].toString(),
             QString("direct_synthetic_operand"));
    auto *Evidence = Dialog->findChild<QPlainTextEdit *>("changesEvidence");
    QTRY_VERIFY_WITH_TIMEOUT(
        Evidence->toPlainText().contains("Synthetic operation"), Timeout);
    QVERIFY(Evidence->toPlainText().contains("constant 0x11"));
    auto *Context = Dialog->findChild<QComboBox *>("changesContext");
    QTRY_VERIFY_WITH_TIMEOUT(Context->isEnabled(), Timeout);
    Context->setCurrentIndex(1);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    auto *Backward = Dialog->findChild<QPushButton *>("changesBackward");
    QTRY_VERIFY_WITH_TIMEOUT(Backward->isEnabled(), Timeout);
    QTest::mouseClick(Backward, Qt::LeftButton);
    auto *SliceTable = Dialog->findChild<QTableView *>("changesSlice");
    QTRY_VERIFY_WITH_TIMEOUT(SliceTable->model()->rowCount() > 0, Timeout);
    auto *Side = Dialog->findChild<QComboBox *>("changesSide");
    QTRY_VERIFY_WITH_TIMEOUT(Side->isEnabled(), Timeout);
    Side->setCurrentIndex(1);
    QSignalSpy Inspect(Dialog, &ComparisonChangesDialog::inspectFunction);
    QTest::mouseClick(Dialog->findChild<QPushButton *>("changesInspect"),
                      Qt::LeftButton);
    QCOMPARE(Inspect.count(), 1);
    QCOMPARE(Inspect[0][1].toString(), QString("0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Dialog->raise();
    auto *Details = Dialog->findChild<QTabWidget *>("changesDetails");
    Details->setCurrentIndex(0);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        Dialog->resize(1180, 820);
        QTest::qWait(100);
        const auto Prefix =
            Captures +
            (Mode == Theme::Mode::Dark ? "/dark-changes" : "/light-changes");
        QVERIFY(Dialog->grab().save(Prefix + ".png"));
        Dialog->resize(Dialog->minimumSize());
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-minimum.png"));
      }
    }
    auto *Calls = Dialog->findChild<QPushButton *>("changesCalls");
    QTRY_VERIFY_WITH_TIMEOUT(Calls->isEnabled(), Timeout);
    QTest::mouseClick(Calls, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(Parent->findChild<ComparisonImpactDialog *>(),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Session.start(Input(A, "0x1000"), Input(B, "0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 0, Timeout);
    QVERIFY(!Backward->isEnabled());
    Dialog->review("primary", "0x1000");
    // A context filter with no SSA evidence must not imply unchanged code.
    QTRY_VERIFY_WITH_TIMEOUT(Context->isEnabled(), Timeout);
    Context->setCurrentIndex(0);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["association"].toString(), QString("native_only"));
    QVERIFY(Dialog->findChild<QLabel *>("changesSummary")
                ->text()
                .contains("not requested"));
    QVERIFY(!Backward->isEnabled());
    QCOMPARE(Failure.count(), 0);
    delete Parent;
  }

  void analystHistoryKeepsDraftBindingAndSurvivesMappingChanges() {
    const auto A = Directory.filePath("review-a.bin"),
               B = Directory.filePath("review-b.bin");
    QVERIFY(write(A, QByteArray::fromHex("b807000000c3")));
    QVERIFY(write(B, QByteArray::fromHex("b809000000c3")));
    auto *Parent = new ComparisonDialog(TEST_COMPARISON_WORKER);
    Parent->setAttribute(Qt::WA_DeleteOnClose, false);
    Parent->show();
    auto &Session = Parent->session();
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [](const QString &Path, const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"platform", "sysv"},
                                                      {"base", Base}}}};
    };
    Session.start(Input(A, "0x1000"), Input(B, "0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    auto *Functions = Parent->findChild<QTableView *>("comparisonFunctions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->selectRow(0);
    QTRY_VERIFY_WITH_TIMEOUT(
        Parent->findChild<QPushButton *>("comparisonChangesReview")
            ->isEnabled(),
        Timeout);
    QTest::mouseClick(
        Parent->findChild<QPushButton *>("comparisonReviewHistory"),
        Qt::LeftButton);
    auto *Dialog = Parent->findChild<ComparisonReviewDialog *>();
    QVERIFY(Dialog);
    auto *Rows = qobject_cast<ComparisonModel *>(
        Dialog->findChild<QTableView *>("reviewHistory")->model());
    auto *Case = Dialog->findChild<QLineEdit *>("reviewCase");
    auto *Author = Dialog->findChild<QLineEdit *>("reviewAuthor");
    auto *Note = Dialog->findChild<QPlainTextEdit *>("reviewNote");
    auto *Assessment = Dialog->findChild<QComboBox *>("reviewAssessment");
    auto *Append = Dialog->findChild<QPushButton *>("reviewAppend");
    Case->setText("owned-gui-case");
    Author->setText("GUI fixture analyst");
    Note->setPlainText(
        "Observed local change; assessment needs further investigation.");
    Assessment->setCurrentIndex(3);
    QTRY_VERIFY_WITH_TIMEOUT(Append->isEnabled(), Timeout);
    QTest::mouseClick(Append, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["source"].toString(), QString("analyst"));
    QVERIFY(Note->toPlainText().isEmpty());
    Note->setPlainText(
        "Manual patch-present fixture claim, without engine validation.");
    Assessment->setCurrentIndex(4);
    QTRY_VERIFY_WITH_TIMEOUT(Append->isEnabled(), Timeout);
    QTest::mouseClick(Append, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["assessment"].toString(), QString("patch_present"));
    QCOMPARE(Rows->row(0)["semantic_validation"].toString(),
             QString("not_performed"));
    auto *Latest = Dialog->findChild<QCheckBox *>("reviewLatest");
    Latest->setChecked(true);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    const auto Saved = Directory.filePath("analyst-review.nddiff");
    Session.command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QFile File(Saved);
    QVERIFY(File.open(QIODevice::ReadOnly));
    const auto Document = QJsonDocument::fromJson(File.readAll()).object();
    QCOMPARE(Document["reviews"].toObject()["records"].toArray().size(), 2);
    Note->setPlainText("Draft retained across pairing and input changes.");
    Session.command(
        "comparison_override",
        {{"kind", "unmatch"}, {"primary", "0x1000"}, {"secondary", "0x8000"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QVERIFY(!Rows->row(0)["pair_active"].toBool());
    QVERIFY(!Append->isEnabled());
    QVERIFY(Note->toPlainText().contains("Draft retained"));
    Session.command("comparison_undo");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Append->isEnabled(), Timeout);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        Dialog->resize(1100, 800);
        QTest::qWait(100);
        const auto Prefix =
            Captures +
            (Mode == Theme::Mode::Dark ? "/dark-reviews" : "/light-reviews");
        QVERIFY(Dialog->grab().save(Prefix + ".png"));
        Dialog->resize(Dialog->minimumSize());
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-minimum.png"));
        auto *Tabs = Dialog->findChild<QTabWidget *>("reviewTabs");
        Tabs->setCurrentIndex(1);
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-record.png"));
        Tabs->setCurrentIndex(0);
      }
    }
    QVERIFY(write(B, QByteArray::fromHex("b80b000000c3")));
    Session.start(Input(A, "0x1000"), Input(B, "0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 0, Timeout);
    QVERIFY(Note->toPlainText().contains("Draft retained"));
    QVERIFY(!Append->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(
        Dialog->findChild<QPushButton *>("reviewNewDraft")->isEnabled(),
        Timeout);
    QTest::mouseClick(Dialog->findChild<QPushButton *>("reviewNewDraft"),
                      Qt::LeftButton);
    QVERIFY(Note->toPlainText().isEmpty());
    QCOMPARE(Failure.count(), 0);
    delete Parent;
  }

  void callImpactPathsKeepWitnessesAndUnknownScope() {
    const auto Path = Directory.filePath("call-paths.bin");
    auto Bytes = QByteArray::fromHex(
        "e80b000000ffd0c3ccccccccccccccccb80700000083c003c3");
    QVERIFY(write(Path, Bytes));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base},
                                                      {"platform", "sysv"}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Failure.empty());
    auto *Dialog = new ComparisonImpactDialog(Session, nullptr);
    Dialog->show();
    Dialog->trace("primary", "0x1010");
    auto *Functions = Dialog->findChild<QTableView *>("impactFunctions");
    auto *Sites = Dialog->findChild<QTableView *>("impactSites");
    auto *Rows = qobject_cast<ComparisonModel *>(Functions->model());
    auto *SiteRows = qobject_cast<ComparisonModel *>(Sites->model());
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(1).isEmpty(), Timeout);
    Functions->selectRow(1);
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!SiteRows->row(0).isEmpty(), Timeout);
    QCOMPARE(SiteRows->row(0)["instruction"].toObject()["bytes"].toString(),
             QString("E80B000000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QSignalSpy Inspect(Dialog, &ComparisonImpactDialog::inspectFunction);
    QTest::mouseClick(Dialog->findChild<QPushButton *>("impactInspect"),
                      Qt::LeftButton);
    QCOMPARE(Inspect.count(), 1);
    QCOMPARE(Inspect[0][1].toString(), QString("0x1000"));
    auto *FunctionKind = Dialog->findChild<QComboBox *>("impactFunctionKind");
    FunctionKind->setCurrentIndex(1);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["address"].toString(), QString("0x1000"));
    FunctionKind->setCurrentIndex(0);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 2, Timeout);
    auto *Kind = Dialog->findChild<QComboBox *>("impactSiteKind");
    Kind->setCurrentIndex(2);
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!SiteRows->row(0).isEmpty(), Timeout);
    QCOMPARE(SiteRows->row(0)["instruction"].toObject()["address"].toString(),
             QString("0x1005"));
    QVERIFY(Dialog->findChild<QLabel *>("impactSiteStatus")
                ->text()
                .contains("Global"));
    Kind->setCurrentIndex(Kind->findData("effects"));
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!SiteRows->row(1).isEmpty(), Timeout);
    QCOMPARE(SiteRows->row(0)["effect"].toObject()["status"].toString(),
             QString("internal_target"));
    QCOMPARE(SiteRows->row(1)["effect"].toObject()["status"].toString(),
             QString("unresolved_target"));
    Sites->selectRow(0);
    QVERIFY(Dialog->findChild<QPlainTextEdit *>("impactSiteDetails")
                ->toPlainText()
                .contains("Internal callee"));
    Kind->setCurrentIndex(Kind->findData("boundaries"));
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 1, Timeout);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        const auto Prefix =
            Captures +
            (Mode == Theme::Mode::Dark ? "/dark-impact" : "/light-impact");
        QVERIFY(Dialog->grab().save(Prefix + ".png"));
        const auto Size = Dialog->size();
        Dialog->resize(Dialog->minimumSize());
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-minimum.png"));
        Dialog->resize(Size);
      }
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Dialog->findChild<QSpinBox *>("impactDepth")->setValue(0);
    QCOMPARE(Rows->rowCount(), 0);
    QTest::mouseClick(Dialog->findChild<QPushButton *>("impactTrace"),
                      Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    QVERIFY(Dialog->findChild<QLabel *>("impactSummary")
                ->text()
                .contains("Analysis limit reached"));
    Kind->setCurrentIndex(3);
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Dialog->trace("secondary", "0x8010");
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["address"].toString(), QString("0x8010"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    int Replies = 0;
    const QJsonObject Query{{"seeds", QJsonArray{"0x1010"}}};
    for (int I = 0; I < 2; ++I)
      Session.request("comparison_impact", Query, Dialog,
                      [&](const auto &) { ++Replies; });
    QVERIFY(Session.cancellable());
    Session.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(Replies, 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Session.start(Input("0x1000"), Input("0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 0, Timeout);
    QCOMPARE(SiteRows->rowCount(), 0);
    QVERIFY(!Dialog->findChild<QPushButton *>("impactInspect")->isEnabled());
    delete Dialog;
    QCOMPARE(Failure.count(), 0);
  }

  void importContractsShowAssumptionsRolesAndMissingSSA() {
#ifndef TEST_COMPARISON_EFFECT_FIXTURE
    QSKIP("Owned ELF import fixture requires a Linux x64 C compiler");
#else
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const QJsonObject Input{{"path", TEST_COMPARISON_EFFECT_FIXTURE}};
    Session.start(Input, Input, {{"dataflow", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Failure.empty());
    auto *Dialog = new ComparisonImpactDialog(Session, nullptr);
    QJsonObject InventoryReply;
    Session.request(
        "comparison_functions", {{"search", "effect_entry"}}, Dialog,
        [&](const QJsonObject &Message) { InventoryReply = Message; });
    QTRY_VERIFY_WITH_TIMEOUT(!InventoryReply.isEmpty(), Timeout);
    QCOMPARE(InventoryReply["status"].toString(), QString("ok"));
    const auto Items = InventoryReply["payload"].toObject()["items"].toArray();
    QCOMPARE(Items.size(), 1);
    const auto Entry = Items.first().toObject()["address"].toString();
    QVERIFY(!Entry.isEmpty());
    Dialog->show();
    Dialog->findChild<QComboBox *>("impactDirection")->setCurrentIndex(1);
    Dialog->trace("primary", Entry);
    auto *Functions = Dialog->findChild<QTableView *>("impactFunctions");
    QTRY_VERIFY_WITH_TIMEOUT(Functions->model()->rowCount() > 0, Timeout);
    auto *Kind = Dialog->findChild<QComboBox *>("impactSiteKind");
    Kind->setCurrentIndex(Kind->findData("effects"));
    auto *Sites = Dialog->findChild<QTableView *>("impactSites");
    auto *Rows = qobject_cast<ComparisonModel *>(Sites->model());
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0 && !Rows->row(0).isEmpty(),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->row(0).contains("effect"), Timeout);
    int Copy = -1;
    for (int I = 0; I < Rows->rowCount(); ++I)
      if (Rows->row(I)["effect"].toObject()["family"].toString() == "copy")
        Copy = I;
    QVERIFY2(
        Copy >= 0,
        qPrintable(Dialog->findChild<QLabel *>("impactSiteStatus")->text()));
    Sites->selectRow(Copy);
    auto *Details = Dialog->findChild<QPlainTextEdit *>("impactSiteDetails");
    QVERIFY(Details->toPlainText().contains("Qualified copy contract"));
    QVERIFY(Details->toPlainText().contains("Length: argument 2"));
    QVERIFY(Details->toPlainText().contains("Assumption:"));
    QVERIFY(Details->toPlainText().contains("memory alias"));
    QVERIFY(Dialog->findChild<QLabel *>("impactSiteStatus")
                ->text()
                .contains("2 qualified"));
    auto *Tabs = Dialog->findChild<QTabWidget *>("impactSiteTabs");
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        Tabs->setCurrentIndex(0);
        QTest::qWait(100);
        const auto Prefix =
            Captures +
            (Mode == Theme::Mode::Dark ? "/dark-effects" : "/light-effects");
        QVERIFY(Dialog->grab().save(Prefix + ".png"));
        const auto Size = Dialog->size();
        Dialog->resize(Dialog->minimumSize());
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-minimum.png"));
        Tabs->setCurrentIndex(1);
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-contract.png"));
        Dialog->resize(Size);
      }
    Dialog->findChild<QComboBox *>("impactSide")->setCurrentIndex(1);
    QCOMPARE(Rows->rowCount(), 0);
    QVERIFY(Details->toPlainText().isEmpty());
    Dialog->trace("secondary", Entry);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0 && !Rows->row(0).isEmpty(),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Session.start(Input, Input);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Rows->rowCount(), 0);
    QVERIFY(Details->toPlainText().isEmpty());
    Dialog->trace("primary", Entry);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0 && !Rows->row(0).isEmpty(),
                             Timeout);
    QCOMPARE(Rows->row(0)["effect"].toObject()["status"].toString(),
             QString("ssa_unavailable"));
    QVERIFY(Dialog->findChild<QLabel *>("impactSiteStatus")
                ->text()
                .contains("0 qualified"));
    QCOMPARE(Failure.count(), 0);
    delete Dialog;
#endif
  }

  void dependencyReviewSlicesNativeOriginsAndClearsOldEvidence() {
    const auto Path = Directory.filePath("dependencies.bin");
    // Arithmetic result becomes a recovered argument of a local helper.
    QVERIFY(write(Path, QByteArray::fromHex(
                            "89f883c00383f0176bc00783c0095089c7e80600000059c3"
                            "cccccccc8d4705c3")));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base},
                                                      {"platform", "sysv"}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"), {{"dataflow", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Failure.empty());
    auto *Dialog = new ComparisonFlowDialog(Session, nullptr);
    Dialog->show();
    auto *Functions = Dialog->findChild<QTableView *>("flowFunctions");
    auto *Nodes = Dialog->findChild<QTableView *>("flowNodes");
    auto *Slice = Dialog->findChild<QTableView *>("flowSlice");
    auto *Rows = qobject_cast<ComparisonModel *>(Nodes->model());
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    int Mul = -1;
    int Call = -1;
    for (int I = 0; I < Rows->rowCount(); ++I) {
      if (Rows->row(I)["opcode"] == QJsonValue("INT_MULT"))
        Mul = I;
      if (Rows->row(I)["opcode"] == QJsonValue("CALL"))
        Call = I;
    }
    QVERIFY(Mul >= 0);
    QVERIFY(Call >= 0);
    QCOMPARE(Rows->row(Call)["call_argument_count"].toInt(), 1);
    QVERIFY(Rows->data(Rows->index(Call, 4), Qt::DisplayRole)
                .toString()
                .contains("arg 1:"));
    QCOMPARE(Rows->row(Mul)["address"].toString(), QString("0x1008"));
    Nodes->setCurrentIndex(Rows->index(Call, 0));
    auto *Trace = Dialog->findChild<QPushButton *>("flowBackward");
    QTRY_VERIFY(Trace->isEnabled());
    QTest::mouseClick(Trace, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(Slice->model()->rowCount() > 0, Timeout);
    auto *SliceRows = qobject_cast<ComparisonModel *>(Slice->model());
    QTRY_VERIFY_WITH_TIMEOUT(!SliceRows->row(0).isEmpty(), Timeout);
    bool ContainsSetup = false;
    for (int I = 0; I < SliceRows->rowCount(); ++I) {
      ContainsSetup |= SliceRows->row(I)["opcode"] == QJsonValue("INT_MULT");
      if (SliceRows->row(I)["opcode"] == QJsonValue("CALL"))
        QVERIFY(SliceRows->data(SliceRows->index(I, 4), Qt::DisplayRole)
                    .toString()
                    .contains("arg 1:"));
    }
    QVERIFY(ContainsSetup);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QVERIFY(Dialog->findChild<QLabel *>("flowSliceStatus")
                ->text()
                .contains("complete"));
    int Replies = 0;
    const QJsonObject Query{{"address", "0x1000"}, {"seeds", QJsonArray{0}}};
    Session.request("comparison_slice", Query, Dialog,
                    [&](const auto &) { ++Replies; });
    Session.request("comparison_slice", Query, Dialog,
                    [&](const auto &) { ++Replies; });
    QVERIFY(Session.cancellable());
    Session.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(Replies, 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(Captures + (Mode == Theme::Mode::Dark
                                                    ? "/dark-flow.png"
                                                    : "/light-flow.png")));
      }
    Dialog->findChild<QComboBox *>("flowSide")->setCurrentIndex(1);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Dialog->findChild<QLabel *>("flowFunctionStatus")
                                 ->text()
                                 .contains("0x8000"),
                             Timeout);
    QCOMPARE(Slice->model()->rowCount(), 0);
    Session.start(Input("0x1000"), Input("0x8000"), {{"dataflow", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 0, Timeout);
    QCOMPARE(Nodes->model()->rowCount(), 0);
    QCOMPARE(Slice->model()->rowCount(), 0);
    QVERIFY(!Trace->isEnabled());
    Dialog->close();
    delete Dialog;
    QCOMPARE(Failure.count(), 0);
  }
};

QTEST_MAIN(ComparisonTests)
#include "ComparisonTests.moc"
