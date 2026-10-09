// Unit tests of workbench building blocks that need no worker.
#include "ActionRegistry.h"
#include "Address.h"
#include "AddressSpace.h"
#include "Docking.h"
#include "Expression.h"
#include "Icons.h"
#include "LoadFileDialog.h"
#include "ProjectDatabase.h"
#include "StyledText.h"
#include "Theme.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFile>
#include <QFontInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QMetaEnum>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <kddockwidgets/KDDockWidgets.h>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/MainWindow.h>
#include <kddockwidgets/qtwidgets/views/Separator.h>
#include <memory>

using namespace neverd::gui;

namespace {
/// Two docks side by side in a main window, each filled with a plain color.
struct DockPair {
  static constexpr QColor LeftColor{0x10, 0x20, 0x30};
  static constexpr QColor RightColor{0x30, 0x20, 0x10};

  KDDockWidgets::QtWidgets::MainWindow window{QStringLiteral("DockPair")};
  KDDockWidgets::QtWidgets::DockWidget *left = dock("left", LeftColor);
  KDDockWidgets::QtWidgets::DockWidget *right = dock("right", RightColor);

  DockPair() {
    window.setCenterWidgetMargins(dockAreaMargins());
    window.addDockWidget(left, KDDockWidgets::Location_OnLeft);
    window.addDockWidget(right, KDDockWidgets::Location_OnRight);
    window.resize(640, 480);
    window.show();
  }
  static KDDockWidgets::QtWidgets::DockWidget *dock(const char *name,
                                                    QColor color) {
    auto *dock =
        new KDDockWidgets::QtWidgets::DockWidget(QString::fromLatin1(name));
    auto *content = new QWidget;
    content->setAutoFillBackground(true);
    QPalette palette = content->palette();
    palette.setColor(QPalette::Window, color);
    content->setPalette(palette);
    dock->setWidget(content);
    return dock;
  }
  QRect contentRect(KDDockWidgets::QtWidgets::DockWidget *dock) {
    auto *content = dock->widget();
    return {content->mapTo(&window, QPoint()), content->size()};
  }
  KDDockWidgets::QtWidgets::Separator *separator() {
    return window.findChild<KDDockWidgets::QtWidgets::Separator *>();
  }
  /// Column \p x of the window's middle row.
  QColor pixel(int x) {
    return window.grab().toImage().pixelColor(x,
                                              contentRect(left).center().y());
  }
};

/// The keys IDA's configuration means by \p spelling: "Ctrl-Shift-Down",
/// "Enter", or a platform key such as "sys(ZoomOut)".
QList<QKeySequence> idaKeys(QLatin1StringView spelling) {
  QString rest = spelling.toString();
  if (rest.isEmpty())
    return {};
  if (rest.startsWith(u"sys(") && rest.endsWith(u')')) {
    bool known = false;
    const int standard =
        QMetaEnum::fromType<QKeySequence::StandardKey>().keyToValue(
            rest.mid(4, rest.size() - 5).toLatin1().constData(), &known);
    return known ? QKeySequence::keyBindings(
                       static_cast<QKeySequence::StandardKey>(standard))
                 : QList<QKeySequence>{};
  }
  // IDA joins the modifiers with '-' and calls Return "Enter".
  QStringList parts;
  for (bool modifier = true; modifier;) {
    modifier = false;
    for (const auto name :
         {QLatin1StringView("Ctrl-"), QLatin1StringView("Shift-"),
          QLatin1StringView("Alt-"), QLatin1StringView("Meta-")})
      if (rest.size() > name.size() &&
          rest.startsWith(name, Qt::CaseInsensitive)) {
        parts << name.chopped(1).toString();
        rest.remove(0, name.size());
        modifier = true;
      }
  }
  parts << (rest == u"Enter" ? QStringLiteral("Return") : rest);
  const auto key =
      QKeySequence::fromString(parts.join(u'+'), QKeySequence::PortableText);
  if (key.isEmpty() || key[0].key() == Qt::Key_unknown)
    return {};
  return {key};
}
} // namespace

class WorkbenchUnitTests : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    // Docking chrome is fixed before the first dock exists.
    configureDocking();
  }

  void addressSpellings() {
    QCOMPARE(hexAddress(0xffff800012340000ULL),
             QStringLiteral("0xffff800012340000"));
    QCOMPARE(displayAddress(0x401000, 16), QStringLiteral("0000000000401000"));
    QCOMPARE(parseAddress(QStringLiteral("0x401000")),
             std::optional<Address>(0x401000));
    QCOMPARE(parseAddress(QStringLiteral("401000h")),
             std::optional<Address>(0x401000));
    QCOMPARE(parseAddress(QStringLiteral("FFFFFFFFFFFFFFFF")),
             std::optional<Address>(~Address(0)));
    QVERIFY(!parseAddress(QStringLiteral("10000000000000000")));
    QVERIFY(!parseAddress(QStringLiteral("main")));
    QVERIFY(!addressValue(QJsonValue(4096)));
    QCOMPARE(addressValue(QJsonValue(QStringLiteral("0x10"))),
             std::optional<Address>(16));
  }

  void expressions_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<qulonglong>("value");
    QTest::newRow("bare hex") << "401000" << 0x401000ULL;
    QTest::newRow("0x") << "0x10" << 0x10ULL;
    QTest::newRow("h suffix") << "10h" << 0x10ULL;
    QTest::newRow("decimal #") << "#10" << 10ULL;
    QTest::newRow("decimal dot") << "10." << 10ULL;
    QTest::newRow("precedence") << "1+2*3" << 7ULL;
    QTest::newRow("parentheses") << "(1+2)*3" << 9ULL;
    QTest::newRow("shift") << "1<<4" << 0x10ULL;
    QTest::newRow("bitwise") << "0xf0 & 0x3c | 1" << 0x31ULL;
    QTest::newRow("unary") << "-1" << ~0ULL;
    QTest::newRow("complement") << "~0" << ~0ULL;
    QTest::newRow("wrapping") << "0xffffffffffffffff+2" << 1ULL;
  }
  void expressions() {
    QFETCH(QString, text);
    QFETCH(qulonglong, value);
    Expression expression(text);
    QVERIFY(expression.identifiers().isEmpty());
    const auto result = expression.evaluate();
    QVERIFY2(result, qPrintable(expression.error()));
    QCOMPARE(qulonglong(*result), value);
  }

  void expressionNamesAndErrors() {
    Expression named(QStringLiteral("main+0x10"));
    QCOMPARE(named.identifiers(), QStringList{QStringLiteral("main")});
    QVERIFY(!named.evaluate());
    QVERIFY(!named.error().isEmpty());
    named.bind(QStringLiteral("main"), 0x401000);
    QCOMPARE(named.evaluate(), std::optional<Address>(0x401010));
    for (const auto &bad : {QStringLiteral("1/0"), QStringLiteral("(1"),
                            QStringLiteral("1+"), QStringLiteral("")}) {
      Expression expression(bad);
      QVERIFY2(!expression.evaluate(), qPrintable(bad));
      QVERIFY(!expression.error().isEmpty());
    }
  }

  void addressSpaceMapsAcrossGaps() {
    AddressSpace space;
    space.reset(QJsonArray{
        QJsonObject{{"name", ".text"}, {"start", "0x1000"}, {"end", "0x2000"}},
        QJsonObject{{"name", ".data"},
                    {"start", "0x8000"},
                    {"end", "0x8100"},
                    {"file_offset", "0x2000"}}});
    QCOMPARE(space.total(), Address(0x1100));
    QCOMPARE(space.first(), Address(0x1000));
    QCOMPARE(space.last(), Address(0x80ff));
    QCOMPARE(space.linearOf(0x1000), Address(0));
    QCOMPARE(space.linearOf(0x8010), Address(0x1010));
    // Unmapped addresses take the next mapped position.
    QCOMPARE(space.linearOf(0x3000), Address(0x1000));
    QCOMPARE(space.addressAt(0x1010), Address(0x8010));
    QCOMPARE(space.regionOf(0x8050)->name, QStringLiteral(".data"));
    QVERIFY(!space.regionOf(0x3000));
    QCOMPARE(space.fileOffsetOf(0x8010), std::optional<Address>(0x2010));
    QVERIFY(!space.fileOffsetOf(0x1000));
  }

  void workerSpansUseUtf8Offsets() {
    StyledLine line;
    const QString text = QString::fromUtf8("call 中文_😀 ; x");
    // Bytes: "call " 5, "中文" 6, "_" 1, "😀" 4 -> the name spans 11 bytes.
    line.setFromWorker(text, QJsonArray{QJsonArray{5, 11, 3, "0x401000"},
                                        QJsonArray{17, 3, 4}});
    QCOMPARE(line.spans.size(), 2);
    QCOMPARE(line.spans[0].start, 5);
    QCOMPARE(text.mid(line.spans[0].start, line.spans[0].length),
             QString::fromUtf8("中文_😀"));
    QCOMPARE(line.spans[0].address, std::optional<Address>(0x401000));
    QCOMPARE(text.mid(line.spans[1].start, line.spans[1].length),
             QStringLiteral("; x"));
    QCOMPARE(line.tokenAt(1), QStringLiteral("call"));
    QCOMPARE(tokenOccurrences(QStringLiteral("rax, [rax+8] ; raxx"),
                              QStringLiteral("rax")),
             (QVector<int>{0, 6}));
    // Copies share text and spans but not the cached layout.
    StyledLine copy = line;
    QCOMPARE(copy.text, line.text);
    QCOMPARE(copy.spans.size(), line.spans.size());
  }

  void commandsHaveUniqueIdsAndUnambiguousShortcuts() {
    QMainWindow window;
    ActionRegistry registry;
    registry.buildMenus(window.menuBar(), [](QMenu *, const QString &) {});
    registry.buildToolbars(&window);
    QSet<QString> ids;
    QHash<QString, QString> globalKeys, viewKeys;
    const auto &views = registry.viewActions();
    for (auto *action : registry.allActions()) {
      QVERIFY2(!ids.contains(action->objectName()),
               qPrintable(action->objectName()));
      ids.insert(action->objectName());
      QVERIFY2(!action->text().isEmpty(), qPrintable(action->objectName()));
      QVERIFY2(!action->icon().isNull(), qPrintable(action->objectName()));
      const auto key = action->shortcut().toString(QKeySequence::PortableText);
      if (key.isEmpty())
        continue;
      auto &keys = views.contains(action) ? viewKeys : globalKeys;
      QVERIFY2(!keys.contains(key),
               qPrintable(key + QStringLiteral(": ") + keys.value(key) +
                          QStringLiteral(" and ") + action->objectName()));
      keys.insert(key, action->objectName());
      // A view shortcut must not also be a window-wide one.
      if (views.contains(action))
        QVERIFY2(!globalKeys.contains(key), qPrintable(key));
    }
    QCOMPARE(ids.size(), int(ActionId::Count));
    // Every command is reachable from a menu.
    QSet<QAction *> reachable;
    std::function<void(QMenu *)> walk = [&](QMenu *menu) {
      for (auto *action : menu->actions()) {
        reachable.insert(action);
        if (action->menu())
          walk(action->menu());
      }
    };
    for (auto *action : window.menuBar()->actions())
      if (action->menu())
        walk(action->menu());
    for (auto *action : registry.allActions())
      QVERIFY2(reachable.contains(action), qPrintable(action->objectName()));
  }

  void commandsKeepIdasShortcuts() {
    ActionRegistry registry;
    QSet<QAction *> shared;
    // Every key IDA gives a command, so that a command IDA does not have
    // takes none of them.
    QHash<QKeySequence, QString> idaCommands;
#define NEVERD_IDA_ACTION(IdaAction, Shortcut, NeverDAction)                   \
  {                                                                            \
    auto *action = registry.action(ActionId::NeverDAction);                    \
    const auto keys = idaKeys(QLatin1StringView(Shortcut));                    \
    QVERIFY2(keys.isEmpty() ? action->shortcut().isEmpty()                     \
                            : keys.contains(action->shortcut()),               \
             qPrintable(QStringLiteral("%1 has \"%2\", IDA's %3 \"%4\"")       \
                            .arg(action->objectName(),                         \
                                 action->shortcut().toString(                  \
                                     QKeySequence::PortableText),              \
                                 QLatin1StringView(IdaAction),                 \
                                 QLatin1StringView(Shortcut))));               \
    shared.insert(action);                                                     \
    for (const auto &key : keys)                                               \
      idaCommands.insert(key, QStringLiteral(IdaAction));                      \
  }
#define NEVERD_IDA_PLANNED(IdaAction, Shortcut, Group)                         \
  for (const auto &key : idaKeys(QLatin1StringView(Shortcut)))                 \
    idaCommands.insert(key, QStringLiteral(IdaAction));
#include "IdaActions.def"
    QVERIFY(shared.size() > int(ActionId::Count) * 3 / 4);
    for (auto *action : registry.allActions())
      if (!shared.contains(action) && !action->shortcut().isEmpty())
        QVERIFY2(!idaCommands.contains(action->shortcut()),
                 qPrintable(action->objectName() +
                            QStringLiteral(" takes the key of IDA's ") +
                            idaCommands.value(action->shortcut())));
    // The spellings the table uses mean the keys Qt reads.
    QCOMPARE(idaKeys(QLatin1StringView("Ctrl-Shift-Down")),
             QList{QKeySequence(QStringLiteral("Ctrl+Shift+Down"))});
    QCOMPARE(idaKeys(QLatin1StringView("Enter")),
             QList{QKeySequence(Qt::Key_Return)});
    QCOMPARE(idaKeys(QLatin1StringView("sys(ZoomOut)")),
             QKeySequence::keyBindings(QKeySequence::ZoomOut));
  }

  void everyIconRenders() {
    QDirIterator icons(QStringLiteral(":/neverd/icons"),
                       {QStringLiteral("*.svg")});
    int count = 0;
    while (icons.hasNext()) {
      const auto name = QFileInfo(icons.next()).completeBaseName();
      const auto rendered = icon(name).pixmap(QSize(16, 16));
      QVERIFY2(!rendered.isNull(), qPrintable(name));
      ++count;
    }
    QVERIFY(count >= 60);
    QVERIFY(icon(QStringLiteral("no-such-icon")).isNull());
  }

  void iconsAreRecordedOriginalArtwork() {
    QSet<QString> palette;
    QHash<QString, QString> groups;
#define NEVERD_ICON_COLOR(Hex, Use) palette.insert(QStringLiteral(Hex));
#define NEVERD_ICON(Name, Group, Shows)                                        \
  groups.insert(QStringLiteral(Name), QStringLiteral(#Group));
#include "IconSet.def"
    // Drawn by hand on the grid: no raster image, text, external reference,
    // script or editor metadata, and colors only from the record.
    static const QRegularExpression Foreign(
        QStringLiteral("<image|<text|<foreignObject|<metadata|<script|href=|"
                       "url\\(|@import|style=|rgb\\(|inkscape:|sodipodi:"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression Color(QStringLiteral("#[0-9A-Fa-f]{3,8}"));
    static const QRegularExpression Paint(
        QStringLiteral("(?:fill|stroke|stop-color)=\"([^\"]*)\""));
    QSet<QString> files;
    QDirIterator icons(QStringLiteral(":/neverd/icons"),
                       {QStringLiteral("*.svg")});
    while (icons.hasNext()) {
      const QFileInfo info(icons.next());
      const auto name = info.completeBaseName();
      files.insert(name);
      QVERIFY2(groups.contains(name),
               qPrintable(name + QStringLiteral(" is not in IconSet.def")));
      if (groups.value(name) == QLatin1String("Brand"))
        continue;
      QFile file(info.filePath());
      QVERIFY(file.open(QIODevice::ReadOnly));
      const auto svg = QString::fromUtf8(file.readAll());
      // Control chrome takes the size of the part it draws.
      if (groups.value(name) != QLatin1String("Chrome"))
        QVERIFY2(svg.contains(QStringLiteral("viewBox=\"0 0 16 16\"")),
                 qPrintable(name));
      const auto foreign = Foreign.match(svg);
      QVERIFY2(!foreign.hasMatch(),
               qPrintable(name + QStringLiteral(": ") + foreign.captured()));
      for (auto paints = Paint.globalMatch(svg); paints.hasNext();) {
        const auto paint = paints.next().captured(1);
        QVERIFY2(paint == QLatin1String("none") ||
                     (Color.match(paint).captured() == paint &&
                      palette.contains(paint.toUpper())),
                 qPrintable(name + QStringLiteral(" paints ") + paint));
      }
    }
    for (auto it = groups.cbegin(); it != groups.cend(); ++it)
      QVERIFY2(files.contains(it.key()),
               qPrintable(it.key() + QStringLiteral(" is recorded but has no "
                                                    "file")));
  }

  void projectDatabaseRoundTrip() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    // More than one compression chunk, with incompressible content.
    QByteArray input(9 * 1024 * 1024 + 123, Qt::Uninitialized);
    QRandomGenerator generator(3389);
    for (auto &byte : input)
      byte = char(generator.bounded(256));
    const auto binary = directory.filePath(QStringLiteral("sample.bin"));
    const auto write = [](const QString &path, const QByteArray &bytes) {
      QFile file(path);
      return file.open(QIODevice::WriteOnly) &&
             file.write(bytes) == bytes.size();
    };
    QVERIFY(write(binary, input));
    QVERIFY(write(binary + ".neverd-annotations.json",
                  R"([{"addr":"0x10","text":"one"}])"));
    QVERIFY(write(binary + ".neverd-renames.json", "[]"));
    // Every table the worker keeps, and how a binary file is read.
    QVERIFY(write(binary + ".neverd-items.json",
                  R"([{"addr":"0x20","kind":"data","size":4}])"));
    QVERIFY(write(binary + ".neverd-load.json",
                  R"({"loader":"binary","processor":"x86_64"})"));
    const auto database = ProjectDatabase::pathFor(binary);
    QCOMPARE(database, binary + QStringLiteral(".nddb"));
    QVERIFY(ProjectDatabase::isDatabase(database));
    QCOMPARE(ProjectDatabase::save(
                 database, binary,
                 {{QStringLiteral("location"), R"({"address":"0x10"})"}}),
             QString());
    QVERIFY(QFileInfo::exists(database));
    QVERIFY(!QFileInfo::exists(database + QStringLiteral(".saving")));
    QString error;
    auto contents = ProjectDatabase::read(database, &error);
    QVERIFY2(contents, qPrintable(error));
    QCOMPARE(contents->inputName, QStringLiteral("sample.bin"));
    QCOMPARE(contents->inputSize, input.size());
    QCOMPARE(contents->inputSha256,
             QString::fromLatin1(
                 QCryptographicHash::hash(input, QCryptographicHash::Sha256)
                     .toHex()));
    QCOMPARE(contents->sidecars.size(), 4);
    QCOMPARE(contents->state.value(QStringLiteral("location")),
             QByteArray(R"({"address":"0x10"})"));

    // A second save updates sidecars and state in place.
    QVERIFY(write(binary + ".neverd-annotations.json",
                  R"([{"addr":"0x10","text":"two"}])"));
    QCOMPARE(ProjectDatabase::saveState(database,
                                        {{QStringLiteral("bookmarks"), "[]"}}),
             QString());
    QCOMPARE(ProjectDatabase::save(database, binary, {}), QString());
    contents = ProjectDatabase::read(database, &error);
    QVERIFY(contents->sidecars.value(QStringLiteral(".neverd-annotations.json"))
                .contains("two"));
    QCOMPARE(contents->state.value(QStringLiteral("bookmarks")),
             QByteArray("[]"));

    // Unpacking elsewhere restores the input and its sidecars exactly.
    QTemporaryDir elsewhere;
    const auto unpacked =
        ProjectDatabase::unpack(database, elsewhere.path(), &error);
    QVERIFY2(!unpacked.isEmpty(), qPrintable(error));
    QFile copy(unpacked);
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), input);
    QFile annotations(unpacked + ".neverd-annotations.json");
    QVERIFY(annotations.open(QIODevice::ReadOnly));
    QVERIFY(annotations.readAll().contains("two"));
    QFile items(unpacked + ".neverd-items.json");
    QVERIFY(items.open(QIODevice::ReadOnly));
    QVERIFY(items.readAll().contains("0x20"));
    QFile load(unpacked + ".neverd-load.json");
    QVERIFY(load.open(QIODevice::ReadOnly));
    QVERIFY(load.readAll().contains("x86_64"));
    QVERIFY(!QFileInfo::exists(unpacked + ".neverd-history.json"));

    // Damage is reported, never unpacked as a different input.
    {
      auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                          QStringLiteral("damage"));
      db.setDatabaseName(database);
      QVERIFY(db.open());
      QSqlQuery query(db);
      QVERIFY(query.exec(QStringLiteral(
          "UPDATE blobs SET data = X'00000001ff' WHERE chunk = 1")));
      db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("damage"));
    QTemporaryDir damaged;
    QVERIFY(
        ProjectDatabase::unpack(database, damaged.path(), &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(damaged.filePath(QStringLiteral("sample.bin"))));

    // A database cannot name an input outside its directory.
    {
      auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                          QStringLiteral("escape"));
      db.setDatabaseName(database);
      QVERIFY(db.open());
      QSqlQuery query(db);
      QVERIFY(query.exec(QStringLiteral(
          "UPDATE meta SET value = '../escape.bin' WHERE key = 'input_name'")));
      db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("escape"));
    QVERIFY(!ProjectDatabase::read(database, &error));
    QVERIFY(!ProjectDatabase::read(
        directory.filePath(QStringLiteral("missing.nddb")), &error));
  }

  void projectDatabaseIdentity() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto write = [](const QString &path, const QByteArray &bytes) {
      QFile file(path);
      return file.open(QIODevice::WriteOnly) &&
             file.write(bytes) == bytes.size();
    };
    const auto contents = [](const QString &path) {
      QFile file(path);
      return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto applicationId = [&](const QString &path) {
      return contents(path).mid(68, 4);
    };
    // Statements run on a SQLite file the way another application would.
    const auto sql = [](const QString &path, const QStringList &statements) {
      bool ok = false;
      {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                            QStringLiteral("identity"));
        db.setDatabaseName(path);
        ok = db.open();
        for (const auto &statement : statements)
          ok = ok && QSqlQuery(db).exec(statement);
        db.close();
      }
      QSqlDatabase::removeDatabase(QStringLiteral("identity"));
      return ok;
    };
    // GeoPackage's application id, "GPKG".
    const auto otherId =
        QStringLiteral("PRAGMA application_id = %1").arg(0x47504B47);
    const auto binary = directory.filePath(QStringLiteral("sample.bin"));
    QVERIFY(write(binary, QByteArray(4096, '\x90')));
    const auto database = ProjectDatabase::pathFor(binary);
    QCOMPARE(ProjectDatabase::save(database, binary, {}), QString());
    QCOMPARE(applicationId(database), QByteArray("NDDB"));

    // The header, not the name, makes a database.
    const auto renamed = directory.filePath(QStringLiteral("project.sqlite"));
    QVERIFY(QFile::copy(database, renamed));
    QVERIFY(ProjectDatabase::isDatabase(renamed));
    QVERIFY(!ProjectDatabase::isDatabase(binary));
    QString error;
    QVERIFY2(ProjectDatabase::read(renamed, &error), qPrintable(error));
    const auto other = directory.filePath(QStringLiteral("other.sqlite"));
    QVERIFY(sql(other, {otherId, QStringLiteral("CREATE TABLE t(x)")}));
    QVERIFY(!ProjectDatabase::isDatabase(other));

    // A database written before the id was stamped still reads, and its next
    // write stamps the id.
    QVERIFY(sql(database, {QStringLiteral("PRAGMA application_id = 0")}));
    QCOMPARE(applicationId(database), QByteArray(4, '\0'));
    QVERIFY2(ProjectDatabase::read(database, &error), qPrintable(error));
    QCOMPARE(ProjectDatabase::saveState(database, {}), QString());
    QCOMPARE(applicationId(database), QByteArray("NDDB"));

    // Another application's SQLite file is neither read nor written, even
    // with a NeverD format row and the .nddb suffix.
    const auto foreign = directory.filePath(QStringLiteral("foreign.nddb"));
    QVERIFY(sql(foreign, {otherId,
                          QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY "
                                         "KEY, value TEXT NOT NULL)"),
                          QStringLiteral("INSERT INTO meta VALUES('format', "
                                         "'1')")}));
    const auto foreignBytes = contents(foreign);
    QVERIFY(!ProjectDatabase::read(foreign, &error));
    QCOMPARE(error,
             QStringLiteral("%1 is not a NeverD database.").arg(foreign));
    QVERIFY(!ProjectDatabase::saveState(foreign, {}).isEmpty());
    QVERIFY(!ProjectDatabase::save(foreign, binary, {}).isEmpty());
    QCOMPARE(contents(foreign), foreignBytes);

    // Nor is a SQLite file without an id that NeverD did not write.
    const auto plain = directory.filePath(QStringLiteral("plain.nddb"));
    QVERIFY(sql(plain, {QStringLiteral("CREATE TABLE t(x)")}));
    const auto plainBytes = contents(plain);
    QCOMPARE(ProjectDatabase::save(plain, binary, {}),
             QStringLiteral("%1 is not a NeverD database.").arg(plain));
    QCOMPARE(contents(plain), plainBytes);
  }

  void loadDialogOffersWhatTheEngineLoads() {
    // An ELF file NeverD loads, and the binary file it cannot load yet.
    const QJsonArray rows{
        QJsonObject{{"loader", "elf"},
                    {"text", "ELF64 for x86-64 (Shared object)"},
                    {"processor", "x86_64"},
                    {"loadable", true}},
        QJsonObject{{"loader", "binary"},
                    {"text", "Binary file"},
                    {"processor", ""},
                    {"loadable", false},
                    {"reason", "Loading a binary file is not supported yet"}}};
    LoadFileDialog dialog(QStringLiteral("/tmp/xxd"), rows);
    auto *loaders = dialog.findChild<QListWidget *>(QStringLiteral("loaders"));
    QVERIFY(loaders);
    QCOMPARE(loaders->count(), 2);
    QCOMPARE(loaders->item(0)->text(),
             QStringLiteral("ELF64 for x86-64 (Shared object) [elf]"));
    QCOMPARE(loaders->item(1)->text(), QStringLiteral("Binary file"));
    // The first loadable row is chosen; a row NeverD cannot load says why.
    QCOMPARE(dialog.row(), 0);
    QVERIFY(!(loaders->item(1)->flags() & Qt::ItemIsEnabled));
    QCOMPARE(loaders->item(1)->toolTip(),
             QStringLiteral("Loading a binary file is not supported yet"));
    // The header's processor shows, and cannot be changed.
    auto *processors =
        dialog.findChild<QTreeWidget *>(QStringLiteral("processors"));
    QVERIFY(processors && !processors->isEnabled());
    QVERIFY(processors->currentItem());
    QCOMPARE(processors->currentItem()->data(0, Qt::UserRole).toString(),
             QStringLiteral("x86_64"));
    // Loading reads debug information and analyzes in idle time by default.
    QVERIFY(dialog.options().debugInfo && dialog.options().analysis);
    dialog.findChild<QCheckBox *>(QStringLiteral("analysis"))
        ->setChecked(false);
    QVERIFY(!dialog.options().analysis);
    dialog.setIndicator(false);
    QVERIFY(!dialog.indicator());
    QVERIFY(dialog.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());

    // A file no row can load offers nothing to accept, and says why.
    const QJsonArray mips{
        QJsonObject{{"loader", "elf"},
                    {"text", "ELF64 for MIPS (Executable)"},
                    {"processor", ""},
                    {"loadable", false},
                    {"reason", "NeverD has no MIPS processor"}},
        rows[1]};
    // A row a loader took for the file's name alone is never the default.
    const QJsonArray named{QJsonObject{{"loader", "evm"},
                                       {"text", "EVM bytecode"},
                                       {"processor", "evm"},
                                       {"loadable", true},
                                       {"by_name", true}},
                           rows[1]};
    LoadFileDialog byName(QStringLiteral("/tmp/firmware.bin"), named);
    QCOMPARE(byName.row(), -1);
    auto *accept = byName.findChild<QPushButton *>(QStringLiteral("ok"));
    QVERIFY(!accept->isEnabled());
    byName.findChild<QListWidget *>(QStringLiteral("loaders"))
        ->setCurrentRow(0);
    QCOMPARE(byName.row(), 0);
    QVERIFY(accept->isEnabled());

    LoadFileDialog refused(QStringLiteral("/tmp/mips"), mips);
    QCOMPARE(refused.row(), -1);
    QVERIFY(
        !refused.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());
    QVERIFY(refused.findChild<QLabel *>(QStringLiteral("note"))
                ->text()
                .contains(QStringLiteral("NeverD has no MIPS processor")));
  }

  void loadDialogReadsABinaryFileAsTheUserSays() {
    // Data no header names: a binary file, read as the processor and at the
    // address the user picks.
    const QJsonArray rows{QJsonObject{{"loader", "binary"},
                                      {"text", "Binary file"},
                                      {"processor", ""},
                                      {"loadable", true}}};
    LoadFileDialog dialog(QStringLiteral("/tmp/firmware.bin"), rows);
    QCOMPARE(dialog.row(), 0);
    auto *processors =
        dialog.findChild<QTreeWidget *>(QStringLiteral("processors"));
    auto *accept = dialog.findChild<QPushButton *>(QStringLiteral("ok"));
    QVERIFY(processors->isEnabled());
    // No processor is assumed.
    QVERIFY(!accept->isEnabled());
    QTreeWidgetItem *aarch64 = nullptr, *evm = nullptr;
    for (int family = 0; family < processors->topLevelItemCount(); ++family)
      for (int child = 0;
           child < processors->topLevelItem(family)->childCount(); ++child) {
        auto *item = processors->topLevelItem(family)->child(child);
        const auto name = item->data(0, Qt::UserRole).toString();
        if (name == QLatin1String("aarch64"))
          aarch64 = item;
        if (name == QLatin1String("evm"))
          evm = item;
      }
    QVERIFY(aarch64 && evm);
    // Bytecode machines read no binary file.
    QVERIFY(!(evm->flags() & Qt::ItemIsEnabled));
    processors->setCurrentItem(aarch64);
    QVERIFY(accept->isEnabled());
    dialog.findChild<QLineEdit *>(QStringLiteral("base"))
        ->setText(QStringLiteral("0x80000"));
    const auto options = dialog.options();
    QCOMPARE(options.processor, QStringLiteral("aarch64"));
    QCOMPARE(options.base, quint64(0x80000));
    QCOMPARE(options.offset, quint64(0));
    QVERIFY(!options.entry);
    // A field that holds no number stops the load and says so.
    dialog.findChild<QLineEdit *>(QStringLiteral("entry"))
        ->setText(QStringLiteral("start"));
    QVERIFY(!accept->isEnabled());
    QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("note"))
                ->text()
                .contains(QStringLiteral("start")));
    QCOMPARE(options.loader, QStringLiteral("binary"));
    // The platform is read from the code unless the user names one.
    auto *platform = dialog.findChild<QComboBox *>(QStringLiteral("platform"));
    QVERIFY(platform && platform->isEnabled());
    QVERIFY(options.platform.isEmpty());
    platform->setCurrentIndex(platform->findData(QStringLiteral("windows")));
    QCOMPARE(dialog.options().platform, QStringLiteral("windows"));
    // The processor picked last time is picked again.
    LoadFileDialog again(QStringLiteral("/tmp/firmware.bin"), rows);
    again.setBinaryProcessor(QStringLiteral("thumb"));
    QCOMPARE(again.options().processor, QStringLiteral("thumb"));
    QVERIFY(again.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());
    // A loader the file's name alone suggests is never the default, and
    // reads the file only when the user chooses it.
    const QJsonArray named{QJsonObject{{"loader", "evm"},
                                       {"text", "EVM bytecode"},
                                       {"processor", "evm"},
                                       {"loadable", true},
                                       {"by_name", true}},
                           rows[0]};
    LoadFileDialog chosen(QStringLiteral("/tmp/firmware.bin"), named);
    QCOMPARE(chosen.row(), 1);
    chosen.findChild<QListWidget *>(QStringLiteral("loaders"))
        ->setCurrentRow(0);
    QCOMPARE(chosen.options().loader, QStringLiteral("evm"));
    QVERIFY(!chosen.findChild<QComboBox *>(QStringLiteral("platform"))
                 ->isEnabled());
    QVERIFY(chosen.options().processor.isEmpty());
    QVERIFY(chosen.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());
  }

  void loadDialogPreselectsTheProcessorTheBytesName() {
    const auto binaryRow = [](QJsonObject identified) {
      identified.insert("loader", "binary");
      identified.insert("text", "Binary file");
      identified.insert("processor", "");
      identified.insert("loadable", true);
      return QJsonArray{identified};
    };
    // The bytes read as ARM64 beat the processor picked for another file,
    // and keeping it lets the engine record it as read.
    LoadFileDialog dialog(
        QStringLiteral("/tmp/firmware.bin"),
        binaryRow(
            {{"guesses", QJsonArray{QJsonObject{{"isa", "aarch64"},
                                                {"name", "ARM64 (AArch64)"},
                                                {"processor", "aarch64"},
                                                {"share", 0.97}}}},
             {"code_share", 0.6},
             {"status", "settled"},
             {"detected", "aarch64"},
             {"evidence", "ARM64 (AArch64) 97% of the code"}}));
    dialog.setBinaryProcessor(QStringLiteral("thumb"));
    QCOMPARE(dialog.options().processor, QStringLiteral("auto"));
    QVERIFY(dialog.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());
    QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("note"))
                ->text()
                .contains(QStringLiteral("ARM64")));
    // Another processor, chosen, is the user's.
    auto *processors =
        dialog.findChild<QTreeWidget *>(QStringLiteral("processors"));
    for (int family = 0; family < processors->topLevelItemCount(); ++family)
      for (int child = 0;
           child < processors->topLevelItem(family)->childCount(); ++child) {
        auto *item = processors->topLevelItem(family)->child(child);
        if (item->data(0, Qt::UserRole).toString() == QLatin1String("x86_64"))
          processors->setCurrentItem(item);
      }
    QCOMPARE(dialog.options().processor, QStringLiteral("x86_64"));

    // A set NeverD cannot decode preselects nothing and says what it is.
    LoadFileDialog foreign(
        QStringLiteral("/tmp/router.bin"),
        binaryRow({{"guesses",
                    QJsonArray{QJsonObject{{"isa", "mips"},
                                           {"name", "MIPS big-endian (32-bit)"},
                                           {"processor", ""},
                                           {"share", 0.94}}}},
                   {"code_share", 0.7},
                   {"status", "settled"},
                   {"detected", ""},
                   {"evidence", "MIPS big-endian (32-bit) 94% of the code"}}));
    QVERIFY(
        !foreign.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());
    QVERIFY(foreign.findChild<QLabel *>(QStringLiteral("note"))
                ->text()
                .contains(QStringLiteral("NeverD cannot decode")));

    // A family whose instructions do not tell its width names both sets.
    LoadFileDialog wide(
        QStringLiteral("/tmp/dump.bin"),
        binaryRow(
            {{"guesses", QJsonArray{QJsonObject{{"isa", "x86"},
                                                {"name", "Intel x86 (32-bit)"},
                                                {"processor", "x86"},
                                                {"share", 0.6}},
                                    QJsonObject{{"isa", "x86_64"},
                                                {"name", "x86-64"},
                                                {"processor", "x86_64"},
                                                {"share", 0.4}}}},
             {"status", "width_unclear"},
             {"detected", ""}}));
    const QString both =
        wide.findChild<QLabel *>(QStringLiteral("note"))->text();
    QVERIFY(both.contains(QStringLiteral("Intel x86 (32-bit)")) &&
            both.contains(QStringLiteral("x86-64")));
    QVERIFY(!wide.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());

    // Code whose instructions start two bytes into its words reads from
    // there.
    LoadFileDialog shifted(
        QStringLiteral("/tmp/carved.bin"),
        binaryRow(
            {{"guesses", QJsonArray{QJsonObject{{"isa", "aarch64"},
                                                {"name", "ARM64 (AArch64)"},
                                                {"processor", "aarch64"},
                                                {"share", 1.0}}}},
             {"status", "settled"},
             {"code_unit", 4},
             {"code_offset", 2},
             {"detected", "aarch64"},
             {"evidence", "ARM64 (AArch64) 100% of the code"}}));
    QCOMPARE(shifted.findChild<QLineEdit *>(QStringLiteral("offset"))->text(),
             QStringLiteral("0x2"));
    QCOMPARE(shifted.options().offset, quint64(2));
    QCOMPARE(shifted.options().processor, QStringLiteral("auto"));

    // Bytes that look like no known code say so.
    LoadFileDialog data(QStringLiteral("/tmp/data.bin"),
                        binaryRow({{"guesses", QJsonArray()},
                                   {"status", "no_code"},
                                   {"detected", ""}}));
    QVERIFY(data.findChild<QLabel *>(QStringLiteral("note"))
                ->text()
                .startsWith(QStringLiteral("No part of the file")));

    // A Cortex-M vector table says where the code runs.
    LoadFileDialog firmware(
        QStringLiteral("/tmp/stm32.bin"),
        binaryRow(
            {{"guesses", QJsonArray()},
             {"code_share", 0.0},
             {"detected", "thumb"},
             {"evidence", "a Cortex-M vector table at the start"},
             {"fingerprint", QJsonObject{{"name", "Cortex-M vector table"},
                                         {"processor", "thumb"},
                                         {"entry", "0x8000040"},
                                         {"base", "0x8000000"}}}}));
    QCOMPARE(firmware.findChild<QLineEdit *>(QStringLiteral("base"))->text(),
             QStringLiteral("0x8000000"));
    QCOMPARE(firmware.findChild<QLineEdit *>(QStringLiteral("entry"))->text(),
             QStringLiteral("0x8000040"));
    QCOMPARE(firmware.options().processor, QStringLiteral("auto"));
    QCOMPARE(firmware.options().base, quint64(0x8000000));
  }

  void dockSeparatorsAreHairlines() {
    auto &theme = Theme::instance();
    theme.setMode(Theme::Mode::Dark);
    theme.apply();
    DockPair pair;
    QVERIFY(QTest::qWaitForWindowExposed(&pair.window));
    // The docks' contents meet at a one-pixel gap, with no frame of their own.
    const QRect left = pair.contentRect(pair.left);
    const QRect right = pair.contentRect(pair.right);
    const int gap = left.right() + 1;
    QCOMPARE(right.left(), gap + 1);
    QCOMPARE(pair.pixel(gap), theme.chrome(QStringLiteral("DockSeparator")));
    QCOMPARE(pair.pixel(gap - 1), DockPair::LeftColor);
    QCOMPARE(pair.pixel(gap + 1), DockPair::RightColor);
    // The separator's grab area spreads two pixels over either neighbor.
    auto *separator = pair.separator();
    QVERIFY(separator);
    QCOMPARE(separator->mapTo(&pair.window, QPoint()).x(), gap - 2);
    QCOMPARE(separator->width(), 5);

    theme.setMode(Theme::Mode::Light);
    QCOMPARE(pair.pixel(gap), theme.chrome(QStringLiteral("DockSeparator")));
    theme.setMode(Theme::Mode::Dark);
  }

  void dockSeparatorsLightUpWhileHoveredOrDragged() {
    auto &theme = Theme::instance();
    theme.setMode(Theme::Mode::Dark);
    theme.apply();
    DockPair pair;
    QVERIFY(QTest::qWaitForWindowExposed(&pair.window));
    auto *separator = pair.separator();
    QVERIFY(separator);
    const int gap = pair.contentRect(pair.left).right() + 1;
    const QColor hover = theme.chrome(QStringLiteral("DockSeparatorHover"));
    const QPoint hairline(2, separator->height() / 2);

    // A pointer that only crosses the separator leaves it alone.
    QEnterEvent enter(hairline, separator->mapTo(&pair.window, hairline),
                      separator->mapToGlobal(hairline));
    QCoreApplication::sendEvent(separator, &enter);
    QCOMPARE(pair.pixel(gap - 1), DockPair::LeftColor);
    // One that rests on it lights a bar over the hairline and its neighbors.
    QTRY_COMPARE(pair.pixel(gap - 1), hover);
    QCOMPARE(pair.pixel(gap), hover);
    QCOMPARE(pair.pixel(gap + 1), hover);
    QCOMPARE(pair.pixel(gap - 2), DockPair::LeftColor);
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(separator, &leave);
    QTRY_COMPARE(pair.pixel(gap),
                 theme.chrome(QStringLiteral("DockSeparator")));
    QCOMPARE(pair.pixel(gap - 1), DockPair::LeftColor);

    // Dragging moves the gap with the pointer and keeps the bar lit.  On
    // Windows the layout asks the system whether the button is really down,
    // and a synthesized press is not.
#ifndef Q_OS_WIN
    const int width = pair.left->widget()->width();
    QTest::mousePress(separator, Qt::LeftButton, {}, hairline);
    QTest::mouseMove(separator, hairline + QPoint(40, 0));
    QTest::mouseRelease(separator, Qt::LeftButton, {}, hairline);
    QCOMPARE(pair.left->widget()->width(), width + 40);
    QTRY_COMPARE(pair.pixel(gap + 40), hover);
#endif
  }

  void floatingWindowsTakeTheThemeOutline() {
    auto &theme = Theme::instance();
    theme.setMode(Theme::Mode::Dark);
    theme.apply();
    auto dock = std::unique_ptr<KDDockWidgets::QtWidgets::DockWidget>(
        DockPair::dock("floating", DockPair::LeftColor));
    dock->show();
    QWidget *window = dock->QWidget::window();
    QVERIFY(window != dock.get());
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QCOMPARE(window->grab().toImage().pixelColor(0, window->height() / 2),
             theme.chrome(QStringLiteral("FloatingWindowBorder")));
  }

  void themeDrawsFlatControls() {
    auto &theme = Theme::instance();
    for (const auto mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
      theme.setMode(mode);
      theme.apply();
      QDialog dialog;
      auto *layout = new QVBoxLayout(&dialog);
      auto *box = new QCheckBox(QStringLiteral("Unchecked"), &dialog);
      auto *combo = new QComboBox(&dialog);
      combo->addItem(QStringLiteral("Item"));
      auto *edit = new QLineEdit(&dialog);
      auto *log = new QPlainTextEdit(&dialog);
      log->setReadOnly(true);
      auto *buttons = new QDialogButtonBox(
          QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
      for (QWidget *widget :
           {static_cast<QWidget *>(box), static_cast<QWidget *>(combo),
            static_cast<QWidget *>(edit), static_cast<QWidget *>(log),
            static_cast<QWidget *>(buttons)})
        layout->addWidget(widget);
      dialog.show();
      QVERIFY(QTest::qWaitForWindowExposed(&dialog));
      const QImage image = dialog.grab().toImage();
      const auto pixel = [&](QWidget *widget, QPoint point) {
        return image.pixelColor(widget->mapTo(&dialog, point));
      };

      // An unchecked box stands out from the dialog behind it.
      QStyleOptionButton option;
      option.initFrom(box);
      const QRect indicator = box->style()->subElementRect(
          QStyle::SE_CheckBoxIndicator, &option, box);
      const QPoint middle(indicator.left(), indicator.center().y());
      QCOMPARE(pixel(box, middle),
               theme.chrome(QStringLiteral("CheckboxBorder")));
      QCOMPARE(pixel(box, indicator.center()),
               theme.chrome(QStringLiteral("CheckboxBackground")));
      QVERIFY(theme.chrome(QStringLiteral("CheckboxBorder")) !=
              theme.chrome(QStringLiteral("Window")));

      // The combo box draws its chevron on the field.
      QStyleOptionComboBox comboOption;
      comboOption.initFrom(combo);
      const QRect arrow = combo->style()->subControlRect(
          QStyle::CC_ComboBox, &comboOption, QStyle::SC_ComboBoxArrow, combo);
      int glyph = 0;
      for (int y = arrow.top(); y <= arrow.bottom(); ++y)
        for (int x = arrow.left(); x <= arrow.right(); ++x)
          glyph +=
              pixel(combo, {x, y}) != theme.chrome(QStringLiteral("Input"));
      QVERIFY(glyph > 4);

      // Text fields type in the code font; read-only text reads as content.
      QVERIFY(QFontInfo(edit->font()).fixedPitch());
      QVERIFY(!QFontInfo(box->font()).fixedPitch());
      QCOMPARE(pixel(log, log->rect().center()),
               theme.chrome(QStringLiteral("Base")));
      // Dialog buttons carry no icons.
      for (auto *button : buttons->buttons())
        QVERIFY(button->icon().isNull());
    }
    theme.setMode(Theme::Mode::Dark);
  }

  void themesDefineEveryColor() {
    auto &theme = Theme::instance();
    for (const auto mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
      theme.setMode(mode);
      for (int role = 0; role < int(ColorRole::Count); ++role)
        QVERIFY2(theme.color(ColorRole(role)).isValid(),
                 qPrintable(QString::number(role)));
    }
    theme.setMode(Theme::Mode::Dark);
    QVERIFY(theme.dark());
    // Visual Studio Code Dark+ editor background.
    QCOMPARE(theme.color(ColorRole::ListingBackground),
             QColor(0x1e, 0x1e, 0x1e));
  }
};

QTEST_MAIN(WorkbenchUnitTests)
#include "WorkbenchUnitTests.moc"
