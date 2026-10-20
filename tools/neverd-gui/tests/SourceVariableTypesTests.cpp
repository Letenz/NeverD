// Real workbench, worker and engine: Y on an ordinary local edits that value.
#include "CodeView.h"
#include "DisassemblyView.h"
#include "Docking.h"
#include "MainWindow.h"
#include "Session.h"
#include "SetTypeDialog.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QApplication>
#include <QDataStream>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace neverd::gui;

namespace {
QString fixture(const QTemporaryDir &Directory) {
  const QByteArray Code =
      QByteArray::fromHex("53e80f0000004889c38b43040343085bc3909090904889f8c3");
  QByteArray Bytes;
  QDataStream Out(&Bytes, QIODevice::WriteOnly);
  Out.setByteOrder(QDataStream::LittleEndian);
  const QByteArray Ident =
      QByteArray::fromHex("7f454c46020101000000000000000000");
  Out.writeRawData(Ident.constData(), Ident.size());
  Out << quint16(2) << quint16(62) << quint32(1) << quint64(0x400078)
      << quint64(64) << quint64(0) << quint32(0) << quint16(64) << quint16(56)
      << quint16(1) << quint16(64) << quint16(0) << quint16(0);
  Out << quint32(1) << quint32(5) << quint64(0) << quint64(0x400000)
      << quint64(0x400000) << quint64(120 + Code.size())
      << quint64(120 + Code.size()) << quint64(4096);
  Bytes += Code;
  const QString Path = Directory.filePath(QStringLiteral("source-types.elf"));
  QFile File(Path);
  return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size()
             ? Path
             : QString();
}
} // namespace

class SourceVariableTypesTests : public QObject {
  Q_OBJECT
  QTemporaryDir Settings;
private slots:
  void initTestCase() {
    QCoreApplication::setOrganizationName("NeverDTests");
    QCoreApplication::setApplicationName("SourceVariableTypes");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Settings.path());
    configureDocking();
  }
  void localType_data() {
    QTest::addColumn<QString>("Representation");
    QTest::addColumn<QString>("Name");
    QTest::newRow("pseudocode") << "source" << "v4_0";
    QTest::newRow("llvm-c") << "llvmc" << "call0";
  }
  void localType() {
    QFETCH(QString, Representation);
    QFETCH(QString, Name);
    QTemporaryDir Directory;
    const QString Path = fixture(Directory);
    QVERIFY(!Path.isEmpty());
    Session Session(QString::fromLocal8Bit(TEST_REAL_WORKER));
    McpConnectionManager Mcp;
    GuiSessionBroker Broker;
    MainWindow Window(Session, Mcp, Broker);
    Window.setAsksForDebugInfo(false);
    Window.setLayoutPath(Directory.filePath("layout.json"));
    Window.resize(1400, 900);
    Window.initializeLayout();
    Window.show();
    Session.open(Path);
    QTRY_VERIFY_WITH_TIMEOUT(Session.loaded(), 30000);
    bool Declared = false;
    Session.declareTypes("c",
                         "struct GUI_CONTEXT { unsigned int State; unsigned "
                         "int Length; unsigned int Flags; };",
                         [&](const QJsonObject &Reply) {
                           Declared =
                               Reply.value("problems").toArray().isEmpty();
                         });
    QTRY_VERIFY_WITH_TIMEOUT(Declared, 30000);
    Window.disassembly()->navigate(0x400078);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentFunction(),
                              std::optional<Address>(0x400078), 30000);
    Window.actions()
        .action(Representation == "source" ? ActionId::ViewPseudocode
                                           : ActionId::ViewLLVMC)
        ->trigger();
    CodeView *View = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
          for (auto *Candidate : Window.findChildren<CodeView *>())
            if (Candidate->representation() == Representation)
              View = Candidate;
          return View && !View->text()->loading() &&
                 View->text()->allText().contains(Name);
        })(),
        30000);
    auto *Code = View->text();
    QVERIFY(Code->findText(Name, true));
    Code->setFocus();
    QTRY_VERIFY(Code->renameTarget() &&
                Code->renameTarget()->contains("variable_id"));
    QTimer Editor;
    bool Edited = false, Refused = false;
    int Stage = 0;
    QString Diagnostic;
    connect(&Editor, &QTimer::timeout, this, [&] {
      auto *Dialog =
          qobject_cast<SetTypeDialog *>(QApplication::activeModalWidget());
      if (!Dialog)
        return;
      auto *Field = Dialog->findChild<QLineEdit *>("setTypeField");
      auto *Status = Dialog->findChild<QLabel *>("setTypeStatus");
      auto *Apply = Dialog->findChild<QPushButton *>("setTypeApply");
      Diagnostic = Dialog->findChild<QLabel *>("setTypeSubject")->text() +
                   " / " + Status->text() + " / " + Field->text();
      if (!Field || !Field->isEnabled())
        return;
      if (Stage == 0) {
        QTest::keyClicks(Field, "unsigned int");
        Stage = 1;
      } else if (Stage == 1 && Status->text().startsWith(QStringLiteral("✗"))) {
        Refused = !Apply->isEnabled();
        Field->selectAll();
        QTest::keyClicks(Field, "struct GUI_CONTEXT *");
        Stage = 2;
      } else if (Stage == 2 && Apply->isEnabled()) {
        Editor.stop();
        Edited = true;
        Apply->click();
      }
    });
    QTimer::singleShot(30000, &Window, [] {
      if (auto *Dialog =
              qobject_cast<SetTypeDialog *>(QApplication::activeModalWidget()))
        Dialog->reject();
    });
    Editor.start(10);
    Window.actions().action(ActionId::EditSetType)->trigger();
    QVERIFY2(Edited && Refused,
             qPrintable(QString::number(Stage) + ": " + Diagnostic));
    QTRY_VERIFY_WITH_TIMEOUT(Code->allText().contains("->Length") &&
                                 Code->allText().contains("->Flags"),
                             30000);
    Session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(!Code->allText().contains("->Length"), 30000);
    Session.redo();
    QTRY_VERIFY_WITH_TIMEOUT(Code->allText().contains("->Length"), 30000);
    QVERIFY2(Session.lastError().isEmpty(), qPrintable(Session.lastError()));
  }
};
QTEST_MAIN(SourceVariableTypesTests)
#include "SourceVariableTypesTests.moc"
