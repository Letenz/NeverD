// Unit tests of workbench building blocks that need no worker.
#include "ActionRegistry.h"
#include "Address.h"
#include "AddressSpace.h"
#include "Expression.h"
#include "Icons.h"
#include "ProjectDatabase.h"
#include "StyledText.h"
#include "Theme.h"

#include <QAction>
#include <QDirIterator>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenuBar>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

using namespace neverd::gui;

class WorkbenchUnitTests : public QObject {
  Q_OBJECT
private slots:
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
    QCOMPARE(contents->sidecars.size(), 2);
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
