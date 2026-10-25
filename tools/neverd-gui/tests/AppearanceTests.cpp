//===- AppearanceTests.cpp - Owned desktop appearance -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Windows, menus, fonts and icons keep the application appearance with a
/// conflicting desktop theme or a retired native-caption preference.
//===----------------------------------------------------------------------===//

#include "GnomeModalDialogs.h"
#include "Language.h"
#include "QuickStartDialog.h"
#include "SettingsKeys.h"
#include "Theme.h"
#include "TitleBar.h"
#include "WindowFrame.h"

#include <QAbstractButton>
#include <QApplication>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDialog>
#include <QFontInfo>
#include <QGlyphRun>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QSettings>
#include <QStyle>
#include <QTemporaryDir>
#include <QTest>
#include <QTextLayout>
#include <QToolButton>
#include <QWindow>
#include <memory>

using namespace neverd::gui;

class AppearanceTests final : public QObject {
  Q_OBJECT
public:
  static void initMain() {
    gnome::prepareModalDialogs();
    WindowFrame::prepareNativeWindows();
  }

private slots:
  void initTestCase() {
    QVERIFY(settings_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("AppearanceTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settings_.path());
    QSettings().setValue(settings::NativeTitleBar, true);
    WindowFrame::frameDialogs(*qApp);
    gnome::detachModalDialogs(*qApp);
  }

  void dialogsUseOwnedChrome_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("dark");
    for (const char *kind :
         {"dialog", "file", "font", "color", "message", "input", "quick-start"})
      for (const bool dark : {false, true})
        QTest::newRow(
            qPrintable(QString::fromLatin1(kind) + (dark ? "-dark" : "-light")))
            << QString::fromLatin1(kind) << dark;
  }

  void dialogsUseOwnedChrome() {
    QFETCH(QString, kind);
    QFETCH(bool, dark);
    Theme::instance().setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    Theme::instance().apply();
    std::unique_ptr<QDialog> dialog;
    if (kind == "file")
      dialog =
          std::make_unique<QFileDialog>(nullptr, "Open file", settings_.path());
    else if (kind == "font")
      dialog = std::make_unique<QFontDialog>(Theme::defaultCodeFont());
    else if (kind == "color")
      dialog = std::make_unique<QColorDialog>();
    else if (kind == "message")
      dialog = std::make_unique<QMessageBox>(QMessageBox::Information,
                                             "Analysis", "Analysis complete.");
    else if (kind == "input")
      dialog = std::make_unique<QInputDialog>();
    else if (kind == "quick-start")
      dialog = std::make_unique<QuickStartDialog>(
          [](QWidget *) { return QString(); });
    else
      dialog = std::make_unique<QDialog>();
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    QVERIFY(dialog->windowFlags() & Qt::FramelessWindowHint);
    QVERIFY(dialog->windowFlags() & Qt::NoDropShadowWindowHint);
    QVERIFY(!dialog->testAttribute(Qt::WA_DontShowOnScreen));
    auto *bar = dialog->findChild<TitleBar *>(Qt::FindDirectChildrenOnly);
    QVERIFY(bar && bar->isVisible());
    auto *close = bar->findChild<QAbstractButton *>("closeButton");
    QVERIFY(close && close->isVisible());
    QCOMPARE(dialog->windowHandle()->frameMargins(), QMargins());
    QVERIFY(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs));
    QCOMPARE(
        dialog->palette().color(QPalette::Window),
        Theme::instance().chrome(kind == "quick-start" ? "Base" : "Window"));
    const auto captures = qEnvironmentVariable("NEVERD_APPEARANCE_CAPTURES");
    if (!captures.isEmpty()) {
      QDir().mkpath(captures);
      QVERIFY(dialog->grab().save(captures + '/' + QTest::currentDataTag() +
                                  ".png"));
    }
    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY(!dialog->isVisible());
  }

  void desktopThemeCannotSupplyColorsOrClearIcon() {
    const auto previous = Theme::instance().mode();
    const auto oldName = QIcon::themeName();
    const auto oldPaths = QIcon::themeSearchPaths();
    const auto restore = qScopeGuard([&] {
      QIcon::setThemeName(oldName);
      QIcon::setThemeSearchPaths(oldPaths);
      Theme::instance().setMode(previous);
      Theme::instance().apply();
    });
    QTemporaryDir icons;
    QVERIFY(icons.isValid());
    QDir().mkpath(icons.filePath("test/16x16/actions"));
    QFile index(icons.filePath("test/index.theme"));
    QVERIFY(index.open(QIODevice::WriteOnly));
    index.write("[Icon Theme]\nName=test\nDirectories=16x16/actions\n"
                "[16x16/actions]\nSize=16\nType=Fixed\nContext=Actions\n");
    index.close();
    for (const auto *name : {"edit-clear", "edit-clear-locationbar-rtl"}) {
      QFile file(
          icons.filePath(QString("test/16x16/actions/%1.svg").arg(name)));
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" "
                 "height=\"16\">"
                 "<rect width=\"16\" height=\"16\" fill=\"#ff00ff\"/></svg>");
    }
    QIcon::setThemeSearchPaths({icons.path()});
    QIcon::setThemeName("test");
    QVERIFY(QIcon::hasThemeIcon("edit-clear"));
    for (const auto mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
      Theme::instance().setMode(mode);
      Theme::instance().apply();
      const QPalette expected = QApplication::palette();
      QPalette injected;
      for (int role = 0; role < QPalette::NColorRoles; ++role)
        if (role != QPalette::NoRole)
          injected.setColor(QPalette::ColorRole(role), Qt::magenta);
      QApplication::setPalette(injected);
      QApplication::setFont(QFont("monospace", 24));
      Theme::instance().apply();
      for (int role = 0; role < QPalette::NColorRoles; ++role)
        if (role != QPalette::NoRole)
          for (const auto group :
               {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
            QCOMPARE(
                QApplication::palette().color(group, QPalette::ColorRole(role)),
                expected.color(group, QPalette::ColorRole(role)));
      QCOMPARE(QFontInfo(QApplication::font()).family(), QString("Noto Sans"));
      QLineEdit edit("clear this");
      edit.setClearButtonEnabled(true);
      edit.show();
      QVERIFY(QTest::qWaitForWindowExposed(&edit));
      auto *clear = edit.findChild<QToolButton *>();
      QVERIFY(clear && clear->isVisible());
      const auto image = clear->icon().pixmap(16, 16).toImage();
      QVERIFY(!image.isNull());
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
          QVERIFY(image.pixelColor(x, y) != QColor(Qt::magenta));
      QTest::mouseClick(clear, Qt::LeftButton);
      QVERIFY(edit.text().isEmpty());
    }
  }

  void menusAndButtonOrderAreOwned() {
    Theme::instance().apply();
    QMenuBar bar;
    bar.addMenu("File");
    bar.ensurePolished();
    QVERIFY(!bar.isNativeMenuBar());
    QCOMPARE(bar.style()->styleHint(QStyle::SH_DialogButtonLayout),
             int(QDialogButtonBox::GnomeLayout));
    QMenu menu;
    menu.addAction("Open");
    menu.ensurePolished();
    QVERIFY(menu.windowFlags() & Qt::NoDropShadowWindowHint);
  }

  void bundledFontsCoverInterfaceLanguages() {
    const auto original = currentLanguage();
    const auto restore = qScopeGuard([&] { applyLanguage(original); });
    for (const auto &language : languages()) {
      applyLanguage(language.code);
      QTextLayout layout(
          QString::fromUtf8("Open 中國 日本語 한글 العربية Русский →"),
          QApplication::font());
      layout.beginLayout();
      auto line = layout.createLine();
      line.setLineWidth(2000);
      layout.endLayout();
      const auto runs = layout.glyphRuns(0, -1, QTextLayout::RetrieveAll);
      QVERIFY(!runs.isEmpty());
      for (const auto &run : runs) {
        const auto diagnostic =
            QStringLiteral("%1: %2 at string offset %3")
                .arg(language.code, run.rawFont().familyName())
                .arg(run.stringIndexes().isEmpty()
                         ? -1
                         : run.stringIndexes().front());
        QVERIFY2(run.rawFont().familyName().startsWith("Noto "),
                 qPrintable(diagnostic));
        for (const auto glyph : run.glyphIndexes())
          QVERIFY(glyph != 0);
      }
    }
    const auto code = Theme::defaultCodeFont();
    QCOMPARE(QFontInfo(code).family(), QString("Noto Sans Mono"));
    const QFontMetrics metrics(code);
    QCOMPARE(metrics.horizontalAdvance("WWWW"),
             metrics.horizontalAdvance("iiii"));
  }

  void clientResizeKeepsOppositeEdgesAndBounds() {
    const QString platform = QGuiApplication::platformName();
    if (platform != "offscreen" && platform != "cocoa")
      QSKIP("These platforms use native resize; docking tests drive it.");
    QWidget window;
    window.setMinimumSize(240, 180);
    window.setMaximumSize(500, 400);
    QVERIFY(WindowFrame::attach(window));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (const auto edges :
         {Qt::Edges(Qt::LeftEdge), Qt::Edges(Qt::RightEdge),
          Qt::Edges(Qt::TopEdge), Qt::Edges(Qt::BottomEdge),
          Qt::LeftEdge | Qt::TopEdge, Qt::RightEdge | Qt::TopEdge,
          Qt::LeftEdge | Qt::BottomEdge, Qt::RightEdge | Qt::BottomEdge}) {
      window.setGeometry(100, 100, 320, 240);
      QCoreApplication::processEvents();
      const QRect original = window.geometry();
      const QPoint press(edges & Qt::LeftEdge    ? 2
                         : edges & Qt::RightEdge ? window.width() - 3
                                                 : window.width() / 2,
                         edges & Qt::TopEdge      ? 2
                         : edges & Qt::BottomEdge ? window.height() - 3
                                                  : window.height() / 2);
      const QPoint global = window.mapToGlobal(press);
      const auto send = [&](QEvent::Type type, QPoint at) {
        QMouseEvent event(
            type, window.mapFromGlobal(at), at,
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
            Qt::NoModifier);
        QCoreApplication::sendEvent(window.windowHandle(), &event);
      };
      send(QEvent::MouseButtonPress, global);
      const QPoint delta(edges & Qt::LeftEdge    ? -40
                         : edges & Qt::RightEdge ? 40
                                                 : 0,
                         edges & Qt::TopEdge      ? -30
                         : edges & Qt::BottomEdge ? 30
                                                  : 0);
      send(QEvent::MouseMove, global + delta * 100);
      QCOMPARE(window.width(), delta.x() ? 500 : original.width());
      QCOMPARE(window.height(), delta.y() ? 400 : original.height());
      send(QEvent::MouseMove, global - delta * 100);
      QCOMPARE(window.width(), delta.x() ? 240 : original.width());
      QCOMPARE(window.height(), delta.y() ? 180 : original.height());
      send(QEvent::MouseMove, global + delta);
      send(QEvent::MouseButtonRelease, global + delta);
      QCOMPARE(window.width(), original.width() + (delta.x() ? 40 : 0));
      QCOMPARE(window.height(), original.height() + (delta.y() ? 30 : 0));
      if (edges & Qt::LeftEdge)
        QCOMPARE(window.geometry().right(), original.right());
      else
        QCOMPARE(window.x(), original.x());
      if (edges & Qt::TopEdge)
        QCOMPARE(window.geometry().bottom(), original.bottom());
      else
        QCOMPARE(window.y(), original.y());
    }
  }

private:
  QTemporaryDir settings_;
};

QTEST_MAIN(AppearanceTests)
#include "AppearanceTests.moc"
