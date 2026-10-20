// Real workbench, worker and engine: Y on an ordinary local edits that value.
#include "CodeView.h"
#include "DisassemblyView.h"
#include "Docking.h"
#include "ListingView.h"
#include "MainWindow.h"
#include "Session.h"
#include "SetTypeDialog.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QApplication>
#include <QDataStream>
#include <QDialogButtonBox>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace neverd::gui;

namespace {
QString fixture(const QTemporaryDir &Directory,
                const QByteArray &Override = {}) {
  const QByteArray Code =
      Override.isEmpty()
          ? QByteArray::fromHex(
                "53e80f0000004889c38b43040343085bc3909090904889f8c3")
          : Override;
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
  void stackFrameDialogs() {
    QTemporaryDir Directory;
    const auto Path = fixture(
        Directory, QByteArray::fromHex(
                       "4883ec2848897c2418e8c3ff0f00488b4424184883c428c3"));
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
    Window.disassembly()->navigate(0x40007C);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentFunction(),
                              std::optional<Address>(0x400078), 30000);
    Window.disassembly()->listing()->setFocus();
    auto *Action = Window.actions().action(ActionId::EditStackFrame);
    QTRY_VERIFY(Action->isEnabled());
    bool Opened = false;
    QTimer Editor;
    connect(&Editor, &QTimer::timeout, this, [&] {
      auto *Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!Dialog || Dialog->objectName() != "stackFrameDialog")
        return;
      auto *Table = Dialog->findChild<QTableWidget *>("stackVariablesTable");
      if (!Table) {
        Dialog->reject();
        return;
      }
      for (int Row = 0; Row < Table->rowCount(); ++Row)
        if (Table->item(Row, 0)->text() == "-16") {
          Table->item(Row, 2)->setText("gui_saved_context");
          Table->item(Row, 3)->setText("uint64_t");
          Opened = true;
        }
      Dialog->findChild<QDialogButtonBox *>()
          ->button(QDialogButtonBox::Apply)
          ->click();
    });
    Editor.start(20);
    Action->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(Opened, 30000);
    Editor.stop();
    bool Saved = false;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
                               Session.read("stack_frame",
                                            {{"address", "0x400078"}}, &Window,
                                            [&](const QJsonObject &Reply) {
                                              Saved = Reply.value("policy")
                                                          .toObject()
                                                          .value("variables")
                                                          .toArray()
                                                          .size() == 1;
                                            });
                               return Saved;
                             })(),
                             30000);
    Session.closeFile();
    Window.close();
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
  void callSiteType_data() {
    QTest::addColumn<QString>("Representation");
    QTest::newRow("pseudocode") << "source";
    QTest::newRow("llvm-c") << "llvmc";
  }
  void callSiteType() {
    QFETCH(QString, Representation);
    QTemporaryDir Directory;
    const auto Code =
        QByteArray::fromHex(
            "bf07000000be0b000000ba0d000000e82c000000bf11000000e822000000c3")
            .leftJustified(64, char(0x90)) +
        QByteArray::fromHex("89f8c3");
    const auto Path = fixture(Directory, Code);
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
                 View->text()->allText().contains("sub_4000B8(7");
        })(),
        30000);
    QVERIFY(View->text()->findText("sub_4000B8(7", true));
    View->text()->setFocus();
    QTRY_VERIFY(Window.actions().action(ActionId::EditCallType)->isEnabled());
    bool Edited = false;
    QTimer Editor;
    connect(&Editor, &QTimer::timeout, this, [&] {
      auto *Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (auto *Picker = qobject_cast<QInputDialog *>(Dialog);
          Picker && Picker->windowTitle() == "Select call instruction") {
        Picker->accept();
        return;
      }
      if (!Dialog || Dialog->objectName() != "callTypeDialog")
        return;
      Dialog->findChild<QPlainTextEdit *>("callTypeDeclaration")
          ->setPlainText("uint64_t call(uint32_t first, ...);");
      Dialog->findChild<QPlainTextEdit *>("callTypeActuals")
          ->setPlainText("unsigned int\nunsigned int");
      Edited = true;
      Editor.stop();
      Dialog->findChild<QPushButton *>("callTypeApply")->click();
    });
    Editor.start(10);
    QTimer::singleShot(30000, &Window, [] {
      auto *Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (Dialog)
        Dialog->reject();
    });
    Window.actions().action(ActionId::EditCallType)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(Edited, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        View->text()->allText().contains("(*)(uint32_t, ...)"), 30000);
    QVERIFY2(Session.lastError().isEmpty(), qPrintable(Session.lastError()));
    Session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(
        !View->text()->allText().contains("(*)(uint32_t, ...)"), 30000);
    Session.redo();
    QTRY_VERIFY_WITH_TIMEOUT(
        View->text()->allText().contains("(*)(uint32_t, ...)"), 30000);
    Window.activateWindow();
    View->text()->setFocus();
    QTRY_VERIFY_WITH_TIMEOUT(
        !View->text()->loading() &&
            Window.actions().action(ActionId::EditResetCallTypes)->isEnabled(),
        30000);
    Window.actions().action(ActionId::EditResetCallTypes)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !View->text()->allText().contains("(*)(uint32_t, ...)"), 30000);
  }
  void variableAssociations() {
    QTemporaryDir Directory;
    const auto Path = fixture(
        Directory,
        QByteArray::fromHex("4883ec2848897c2418e87aff0f00488b7c24184883c701e86c"
                            "ff0f004889442418e862ff0f00488b4424184883c428c3"));
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
    Window.disassembly()->navigate(0x400078);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentFunction(),
                              std::optional<Address>(0x400078), 30000);
    Window.actions().action(ActionId::ViewPseudocode)->trigger();
    CodeView *View = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
          for (auto *Candidate : Window.findChildren<CodeView *>())
            if (Candidate->representation() == "source")
              View = Candidate;
          return View && !View->text()->loading() &&
                 View->text()->allText().contains("var_m10");
        })(),
        30000);
    QVERIFY(View->text()->findText("var_m10", true));
    View->text()->setFocus();
    QTRY_VERIFY(View->text()->renameTarget() &&
                View->text()->renameTarget()->contains("variable_id"));
    QTRY_VERIFY(
        Window.actions().action(ActionId::EditSplitVariable)->isEnabled());
    Window.actions().action(ActionId::EditSplitVariable)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!View->text()->loading() &&
                                 !View->text()->allText().contains("var_m10"),
                             30000);
    QVERIFY2(Session.lastError().isEmpty(), qPrintable(Session.lastError()));
    Window.actions().action(ActionId::EditResetVariables)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(View->text()->allText().contains("var_m10"),
                             30000);
    Session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(!View->text()->allText().contains("var_m10"),
                             30000);
  }
};
QTEST_MAIN(SourceVariableTypesTests)
#include "SourceVariableTypesTests.moc"
