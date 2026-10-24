//===- DockingTests.cpp - Floating pane drag regressions ------------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the production docking chrome with native window drag targets.
//===----------------------------------------------------------------------===//

#include "Docking.h"
#include "SettingsKeys.h"
#include "Theme.h"
#include "WindowFrame.h"

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWindow>
#include <kddockwidgets/KDDockWidgets.h>
#include <kddockwidgets/core/DropArea.h>
#include <kddockwidgets/core/DropIndicatorOverlay.h>
#include <kddockwidgets/core/Group.h>
#include <kddockwidgets/core/MainWindow.h>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/MainWindow.h>
#include <kddockwidgets/qtwidgets/views/TabBar.h>
#include <kddockwidgets/qtwidgets/views/TitleBar.h>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

using namespace neverd::gui;
namespace KD = KDDockWidgets;

namespace {
bool onWayland() {
  return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

QString quoted(const QString &text) {
  const QByteArray json =
      QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(json.mid(1, json.size() - 2));
}

/// Only enabled by the disposable GNOME test compositor. QtTest alone cannot
/// provide the trusted pointer serial that Wayland requires to start QDrag.
bool shell(const QString &script) {
  QProcess process;
  process.start(QStringLiteral("gdbus"),
                {QStringLiteral("call"), QStringLiteral("--session"),
                 QStringLiteral("--dest"), QStringLiteral("org.gnome.Shell"),
                 QStringLiteral("--object-path"),
                 QStringLiteral("/org/gnome/Shell"), QStringLiteral("--method"),
                 QStringLiteral("org.gnome.Shell.Eval"), script});
  if (!process.waitForFinished() || process.exitCode() != 0)
    return false;
  const QByteArray result = process.readAllStandardOutput();
  if (!result.startsWith("(true,"))
    qWarning() << result;
  return result.startsWith("(true,");
}

QString nativeWindow(const QWidget &window) {
  return QStringLiteral(
             "global.get_window_actors().map(a => a.meta_window)"
             ".filter(w => w.get_pid() === %1 && w.get_title() === %2).at(-1)")
      .arg(QCoreApplication::applicationPid())
      .arg(quoted(window.windowTitle()));
}

bool place(QWidget &window, QPoint position) {
  if (!onWayland()) {
    window.move(position);
    window.raise();
    window.activateWindow();
    return QTest::qWaitForWindowActive(&window);
  }
  return shell(
      QStringLiteral(
          "(() => { const w = %1; if (!w) throw Error('missing window');"
          "w.move_frame(true, %2, %3); Main.activateWindow(w); })()")
          .arg(nativeWindow(window))
          .arg(position.x())
          .arg(position.y()));
}

class Pointer {
  QPointer<QWidget> Pressed;

public:
  bool move(QWidget &widget, QPoint point, bool held) {
    if (onWayland()) {
      QWidget *window = widget.window();
      const QPoint local = widget.mapTo(window, point) +
                           QPoint(window->windowHandle()->frameMargins().left(),
                                  window->windowHandle()->frameMargins().top());
      const qreal scale = window->devicePixelRatioF();
      return shell(
          QStringLiteral(
              "(() => { const w = %1; if (!w) throw Error('missing window');"
              "const r = w.get_buffer_rect(); "
              "global.neverdTestPointer.notify_absolute_motion("
              "imports.gi.GLib.get_monotonic_time(), r.x + %2, r.y + %3); })()")
              .arg(nativeWindow(*window))
              .arg(qRound(local.x() * scale))
              .arg(qRound(local.y() * scale)));
    }
    const QPoint global = widget.mapToGlobal(point);
    QCursor::setPos(global);
    if (QGuiApplication::platformName() == QLatin1String("xcb"))
      return true;
#ifdef Q_OS_WIN
    Q_UNUSED(held);
    return true; // Native buttons also drive the system move loop.
#else
    QWidget *receiver = Pressed ? Pressed.data() : &widget;
    QMouseEvent event(QEvent::MouseMove, receiver->mapFromGlobal(global),
                      global, Qt::NoButton,
                      held ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(receiver, &event);
    return true;
#endif
  }

  bool button(QWidget &widget, QPoint point, bool pressed) {
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
      QProcess process;
      process.start(
          QStringLiteral("xdotool"),
          {pressed ? QStringLiteral("mousedown") : QStringLiteral("mouseup"),
           QStringLiteral("1")});
      return process.waitForFinished() && process.exitCode() == 0;
    }
    if (onWayland())
      return shell(QStringLiteral("global.neverdTestPointer.notify_button("
                                  "imports.gi.GLib.get_monotonic_time(), 1, "
                                  "imports.gi.Clutter.ButtonState.%1)")
                       .arg(pressed ? QStringLiteral("PRESSED")
                                    : QStringLiteral("RELEASED")));
#ifdef Q_OS_WIN
    Q_UNUSED(widget);
    Q_UNUSED(point);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    return SendInput(1, &input, sizeof(input)) == 1;
#else
    if (pressed) {
      Pressed = &widget;
      QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier, point);
    } else {
      QWidget *receiver = Pressed ? Pressed.data() : &widget;
      QTest::mouseRelease(receiver, Qt::LeftButton, Qt::NoModifier,
                          receiver->mapFromGlobal(widget.mapToGlobal(point)));
      Pressed.clear();
    }
    return true;
#endif
  }
};

struct Desktop {
  KD::QtWidgets::MainWindow Window{QStringLiteral("drag-workbench")};
  KD::QtWidgets::DockWidget Target{QStringLiteral("target")};
  KD::QtWidgets::DockWidget Source{QStringLiteral("source")};

  Desktop() {
    Window.setWindowTitle(QStringLiteral("Docking test workbench"));
    Window.resize(950, 650);
    WindowFrame::attach(Window);
    Target.setTitle(QStringLiteral("Target pane"));
    Target.setWidget(new QLabel(QStringLiteral("Target")));
    Source.setTitle(QStringLiteral("Source pane"));
    Source.setIcon(QApplication::style()->standardIcon(QStyle::SP_FileIcon));
    Source.setWidget(new QLabel(QStringLiteral("Source")));
    Window.addDockWidget(&Target, KD::Location_OnLeft);
    Target.addDockWidgetAsTab(&Source);
    Window.show();
    Source.setFloating(true);
    Source.QWidget::window()->resize(450, 350);
  }

  KD::QtWidgets::TitleBar *titleBar() const {
    for (auto *bar :
         Source.QWidget::window()->findChildren<KD::QtWidgets::TitleBar *>())
      if (bar->isVisible())
        return bar;
    return nullptr;
  }
};
} // namespace

class DockingTests : public QObject {
  Q_OBJECT
  QTemporaryDir Settings;

private slots:
  void initTestCase() {
    QVERIFY(Settings.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("DockingTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Settings.path());
    configureDocking();
    if (onWayland() && qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER")) {
      QVERIFY(shell(QStringLiteral(
          "Main.overview.hide(); global.neverdTestPointer = "
          "imports.gi.Clutter.get_default_backend().get_default_seat()"
          ".create_virtual_device(imports.gi.Clutter.InputDeviceType.POINTER_"
          "DEVICE);")));
      QTest::qWait(750); // Finish leaving the compositor's startup overview.
    }
  }

  void floatingCaption_data() {
    QTest::addColumn<bool>("nativeCaption");
    QTest::newRow("custom") << false;
    QTest::newRow("native-preference") << true;
  }

  void floatingCaption() {
    QFETCH(bool, nativeCaption);
    QSettings().setValue(settings::NativeTitleBar, nativeCaption);
    Desktop desktop;
    auto *floating = desktop.Source.QWidget::window();
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    QVERIFY(desktop.titleBar());
    if (onWayland()) {
      // This catches the native caption that swallowed docking drags.
      QVERIFY(floating->windowFlags() & Qt::FramelessWindowHint);
      QCOMPARE(floating->windowHandle()->frameMargins(), QMargins());
      auto *move = floating->findChild<QLabel *>(
          QStringLiteral("floatingWindowMoveHandle"));
      QVERIFY(move && move->isVisible());
      QCOMPARE(move->cursor().shape(), Qt::SizeAllCursor);
    }
    QSettings().remove(settings::NativeTitleBar);
  }

  void floatingWindowStillMovesAndResizes() {
    if (!onWayland() || !qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER"))
      QSKIP(
          "Exercises the replacement Wayland frame with native pointer input.");
    Desktop desktop;
    QWidget *floating = desktop.Source.QWidget::window();
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    QVERIFY(place(*floating, QPoint(1100, 180)));
    QTest::qWait(100);
    auto *handle = floating->findChild<QLabel *>(
        QStringLiteral("floatingWindowMoveHandle"));
    QVERIFY(handle);
    QVERIFY(
        shell(QStringLiteral("global.neverdBeforeMove = (%1).get_frame_rect();")
                  .arg(nativeWindow(*floating))));
    Pointer pointer;
    const QPoint start = handle->rect().center();
    QVERIFY(pointer.move(*handle, start, false));
    QTest::qWait(50);
    QVERIFY(pointer.button(*handle, start, true));
    QTest::qWait(50);
    QVERIFY(pointer.move(*handle, start + QPoint(80, 60), true));
    QTest::qWait(100);
    QVERIFY(pointer.button(*handle, start, false));
    QVERIFY(shell(QStringLiteral("(() => {const r = (%1).get_frame_rect();"
                                 "if(r.x === global.neverdBeforeMove.x && "
                                 "r.y === global.neverdBeforeMove.y) throw "
                                 "Error('window did not move');})()")
                      .arg(nativeWindow(*floating))));
    const QSize size = floating->size();
    const QPoint corner = floating->contentsRect().bottomRight();
    QVERIFY(pointer.move(*floating, corner, false));
    QTest::qWait(50);
    QVERIFY(pointer.button(*floating, corner, true));
    QTest::qWait(50);
    QVERIFY(pointer.move(*floating, corner + QPoint(80, 60), true));
    QTest::qWait(100);
    QVERIFY(pointer.button(*floating, corner, false));
    QTRY_VERIFY(floating->width() > size.width() &&
                floating->height() > size.height());
    QVERIFY(desktop.Source.isFloating());
  }

  void dragIntoWorkbench_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<bool>("tab");
    QTest::addColumn<int>("location");
    for (bool dark : {false, true})
      for (bool tab : {false, true})
        for (auto location : {KD::DropLocation_Center, KD::DropLocation_Right,
                              KD::DropLocation_Bottom, KD::DropLocation_None}) {
          const auto name = QStringLiteral("%1-%2-%3")
                                .arg(dark ? "dark" : "light")
                                .arg(tab ? "tab" : "caption")
                                .arg(static_cast<int>(location));
          QTest::newRow(qPrintable(name))
              << dark << tab << static_cast<int>(location);
        }
  }

  void dragIntoWorkbench() {
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
      QSKIP("Dragging needs native windows; run under X11, Wayland, Windows or "
            "macOS.");
    if (onWayland() && !qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER"))
      QSKIP("Wayland needs the isolated compositor's native pointer.");
    if (QGuiApplication::platformName() == QLatin1String("xcb") &&
        QStandardPaths::findExecutable(QStringLiteral("xdotool")).isEmpty())
      QSKIP("X11 native pointer input needs xdotool.");
    QFETCH(bool, dark);
    QFETCH(bool, tab);
    QFETCH(int, location);
    Theme::instance().setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    Theme::instance().apply();
    Desktop desktop;
    QWidget *floating = desktop.Source.QWidget::window();
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    QVERIFY(place(desktop.Window, QPoint(20, 50)));
    QVERIFY(place(*floating, QPoint(qRound(desktop.Window.width() *
                                           desktop.Window.devicePixelRatioF()) +
                                        100,
                                    180)));
    QTest::qWait(100);
    QWidget *source = desktop.titleBar();
    QVERIFY(source);
    QPoint start = source->rect().center();
    if (tab) {
      auto *tabs = floating->findChild<KD::QtWidgets::TabBar *>();
      QVERIFY(tabs);
      source = tabs;
      start = tabs->tabRect(tabs->currentIndex()).center();
    }
    Pointer pointer;
    QVERIFY(pointer.move(*source, start, false));
    QTest::qWait(50);
    QVERIFY(pointer.button(*source, start, true));
    bool moved = false;
    bool preview = false;
    bool released = false;
    QPoint destination;
    auto *area = desktop.Window.mainWindow()->dropArea();
    // Separate timers can run in Wayland's nested QDrag event loop.
    QTimer::singleShot(50, &desktop.Window, [&] {
      moved = pointer.move(*source, start + QPoint(0, 50), true);
    });
    QTimer::singleShot(150, &desktop.Window, [&] {
      moved &=
          pointer.move(desktop.Window, desktop.Window.rect().center(), true);
    });
    QTimer::singleShot(350, &desktop.Window, [&] {
      destination = location == KD::DropLocation_None
                        ? QPoint(100, 100)
                        : desktop.Window.mapFromGlobal(
                              area->dropIndicatorOverlay()->posForIndicator(
                                  static_cast<KD::DropLocation>(location)));
      moved &= pointer.move(desktop.Window, destination, true);
    });
    QTimer::singleShot(550, &desktop.Window, [&] {
      const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
      if (!captures.isEmpty()) {
        QDir().mkpath(captures);
        const QString name = QString::fromLatin1(QTest::currentDataTag());
        desktop.Window.grab().save(captures + "/" + name + "-preview.png");
        floating->grab().save(captures + "/" + name + "-floating.png");
      }
      preview = area->currentDropLocation() == location;
      released = pointer.button(desktop.Window, destination, false);
    });
    QTRY_VERIFY_WITH_TIMEOUT(released, 5000);
    // Native input is queued. A cancelled drag is still floating before its
    // release arrives; wait for dispatch before destroying its drop target.
    // Wayland ends QDrag through the data-device protocol, without updating
    // Qt's ordinary mouse-button state. Its hidden preview marks completion.
    if (!onWayland())
      QTRY_COMPARE(QGuiApplication::mouseButtons(),
                   Qt::MouseButtons(Qt::NoButton));
    QTRY_VERIFY(!area->dropIndicatorOverlay()->isVisible());
    QVERIFY(moved);
    QVERIFY(preview);
    QTRY_COMPARE(desktop.Source.isFloating(),
                 location == KD::DropLocation_None);
    QVERIFY(desktop.Source.widget()->isVisible());
    if (location == KD::DropLocation_None)
      QCOMPARE(desktop.Source.QWidget::window(), floating);
    else {
      QCOMPARE(desktop.Source.QWidget::window(), &desktop.Window);
      QCOMPARE(desktop.Source.group() == desktop.Target.group(),
               location == KD::DropLocation_Center);
    }
  }
};

int main(int argc, char **argv) {
  WindowFrame::prepareNativeWindows();
  QApplication app(argc, argv);
  DockingTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "DockingTests.moc"
