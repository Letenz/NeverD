#include "ProjectDatabase.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent/QtConcurrentMap>
#include <vector>

namespace neverd::gui {
namespace {
constexpr int FormatVersion = 1;
constexpr qint64 ChunkBytes = 8 * 1024 * 1024;
constexpr int CompressionLevel = 6;
constexpr char InputBlob[] = "input";
constexpr char JournalSuffix[] = ".neverd-journal.json";
constexpr char DatabasesDirectory[] = "databases";

// Schema of format 1.  Text keys keep the file inspectable with any SQLite
// browser.
constexpr const char *Schema[] = {
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT "
    "NULL)",
    "CREATE TABLE IF NOT EXISTS blobs(name TEXT NOT NULL, chunk INTEGER NOT "
    "NULL, size INTEGER NOT NULL, data BLOB NOT NULL, PRIMARY KEY(name, "
    "chunk))",
    "CREATE TABLE IF NOT EXISTS sidecars(suffix TEXT PRIMARY KEY, data BLOB "
    "NOT NULL)",
    "CREATE TABLE IF NOT EXISTS state(key TEXT PRIMARY KEY, value BLOB NOT "
    "NULL)",
};

/// One SQLite connection for the calling thread, closed on scope exit.
class Connection {
public:
  explicit Connection(const QString &path)
      : name_(QStringLiteral("nddb-") + QUuid::createUuid().toString()) {
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
    database.setDatabaseName(path);
    if (!database.open())
      error_ = database.lastError().text();
  }
  ~Connection() {
    {
      auto database = QSqlDatabase::database(name_, false);
      if (database.isOpen())
        database.close();
    }
    QSqlDatabase::removeDatabase(name_);
  }
  QSqlDatabase database() const { return QSqlDatabase::database(name_, false); }
  const QString &error() const { return error_; }

private:
  QString name_, error_;
};

QString lastError(const QSqlQuery &query) { return query.lastError().text(); }

bool exec(QSqlDatabase &database, const QString &statement, QString &error) {
  QSqlQuery query(database);
  if (!query.exec(statement)) {
    error = lastError(query);
    return false;
  }
  return true;
}

QString sha256(const QString &path, QString &error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QCoreApplication::translate("ProjectDatabase", "Cannot read %1: %2")
                .arg(path, file.errorString());
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) {
    error = QCoreApplication::translate("ProjectDatabase", "Cannot read %1")
                .arg(path);
    return {};
  }
  return QString::fromLatin1(hash.result().toHex());
}

QString metaValue(QSqlDatabase &database, const QString &key) {
  QSqlQuery query(database);
  query.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
  query.addBindValue(key);
  return query.exec() && query.next() ? query.value(0).toString() : QString();
}

bool writeMeta(QSqlDatabase &database, const QString &key, const QString &value,
               QString &error) {
  QSqlQuery query(database);
  query.prepare(
      QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)"));
  query.addBindValue(key);
  query.addBindValue(value);
  if (!query.exec()) {
    error = lastError(query);
    return false;
  }
  return true;
}

bool writeState(QSqlDatabase &database, const QHash<QString, QByteArray> &state,
                QString &error) {
  for (auto it = state.cbegin(); it != state.cend(); ++it) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO state(key, value) VALUES(?, ?)"));
    query.addBindValue(it.key());
    query.addBindValue(it.value());
    if (!query.exec()) {
      error = lastError(query);
      return false;
    }
  }
  return true;
}

/// One stored piece of the input: its expanded size and compressed bytes.
struct Chunk {
  qint64 size = 0;
  QByteArray data;
};

/// The input in chunks compressed concurrently on the global thread pool.
std::optional<std::vector<Chunk>> compressInput(const QString &path,
                                                QString &error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QCoreApplication::translate("ProjectDatabase", "Cannot read %1: %2")
                .arg(path, file.errorString());
    return std::nullopt;
  }
  std::vector<Chunk> chunks;
  do {
    Chunk chunk;
    chunk.data = file.read(ChunkBytes);
    chunk.size = chunk.data.size();
    chunks.push_back(std::move(chunk));
  } while (!file.atEnd());
  if (file.error() != QFileDevice::NoError) {
    error = QCoreApplication::translate("ProjectDatabase", "Cannot read %1: %2")
                .arg(path, file.errorString());
    return std::nullopt;
  }
  QtConcurrent::blockingMap(chunks, [](Chunk &chunk) {
    chunk.data = qCompress(chunk.data, CompressionLevel);
  });
  return chunks;
}
} // namespace

const QStringList &ProjectDatabase::sidecarSuffixes() {
  static const QStringList suffixes = {
      QStringLiteral(".neverd-annotations.json"),
      QStringLiteral(".neverd-renames.json"),
      QStringLiteral(".neverd-history.json")};
  return suffixes;
}

QString ProjectDatabase::pathFor(const QString &binary) {
  return binary + QLatin1String(Extension);
}

bool ProjectDatabase::isDatabase(const QString &path) {
  return path.endsWith(QLatin1String(Extension), Qt::CaseInsensitive);
}

QString ProjectDatabase::workingDirectory(const QString &database) {
  const auto key =
      QCryptographicHash::hash(QFileInfo(database).absoluteFilePath().toUtf8(),
                               QCryptographicHash::Sha1);
  return QDir(QStandardPaths::writableLocation(
                  QStandardPaths::AppLocalDataLocation))
      .filePath(QLatin1String(DatabasesDirectory) + QLatin1Char('/') +
                QString::fromLatin1(key.toHex().left(16)));
}

QString ProjectDatabase::save(const QString &database, const QString &binary,
                              const QHash<QString, QByteArray> &state) {
  QString error;
  const QString digest = sha256(binary, error);
  if (digest.isEmpty())
    return error;
  // A new database is assembled beside its final name and renamed into place
  // only when complete; an existing one is updated in one transaction.
  const bool fresh = !QFileInfo::exists(database);
  const QString target =
      fresh ? database + QStringLiteral(".saving") : database;
  if (fresh)
    QFile::remove(target);
  {
    Connection connection(target);
    if (!connection.error().isEmpty())
      return connection.error();
    auto db = connection.database();
    for (const char *statement : Schema)
      if (!exec(db, QString::fromLatin1(statement), error))
        return error;
    const QString format = metaValue(db, QStringLiteral("format"));
    if (!format.isEmpty() && format.toInt() != FormatVersion)
      return QCoreApplication::translate("ProjectDatabase",
                                         "%1 uses database format %2, which "
                                         "this NeverD cannot update.")
          .arg(database, format);
    const bool inputCurrent =
        metaValue(db, QStringLiteral("input_sha256")) == digest;
    std::optional<std::vector<Chunk>> chunks;
    if (!inputCurrent && !(chunks = compressInput(binary, error)))
      return error;
    if (!db.transaction())
      return db.lastError().text();
    const auto fail = [&](const QString &message) {
      db.rollback();
      return message;
    };
    if (chunks) {
      QSqlQuery remove(db);
      remove.prepare(QStringLiteral("DELETE FROM blobs WHERE name = ?"));
      remove.addBindValue(QString::fromLatin1(InputBlob));
      if (!remove.exec())
        return fail(lastError(remove));
      for (std::size_t i = 0; i < chunks->size(); ++i) {
        QSqlQuery insert(db);
        insert.prepare(QStringLiteral(
            "INSERT INTO blobs(name, chunk, size, data) VALUES(?, ?, ?, ?)"));
        insert.addBindValue(QString::fromLatin1(InputBlob));
        insert.addBindValue(qint64(i));
        insert.addBindValue((*chunks)[i].size);
        insert.addBindValue((*chunks)[i].data);
        if (!insert.exec())
          return fail(lastError(insert));
      }
    }
    // The sidecars beside the input are the project's committed state.
    QSqlQuery clear(db);
    if (!clear.exec(QStringLiteral("DELETE FROM sidecars")))
      return fail(lastError(clear));
    for (const auto &suffix : sidecarSuffixes()) {
      QFile sidecar(binary + suffix);
      if (!sidecar.exists())
        continue;
      if (!sidecar.open(QIODevice::ReadOnly))
        return fail(
            QCoreApplication::translate("ProjectDatabase", "Cannot read %1: %2")
                .arg(sidecar.fileName(), sidecar.errorString()));
      QSqlQuery insert(db);
      insert.prepare(
          QStringLiteral("INSERT INTO sidecars(suffix, data) VALUES(?, ?)"));
      insert.addBindValue(suffix);
      insert.addBindValue(sidecar.readAll());
      if (!insert.exec())
        return fail(lastError(insert));
    }
    const QFileInfo input(binary);
    if (!writeMeta(db, QStringLiteral("format"), QString::number(FormatVersion),
                   error) ||
        !writeMeta(db, QStringLiteral("generator"),
                   QStringLiteral("NeverD ") +
                       QCoreApplication::applicationVersion(),
                   error) ||
        !writeMeta(db, QStringLiteral("input_name"), input.fileName(), error) ||
        !writeMeta(db, QStringLiteral("input_sha256"), digest, error) ||
        !writeMeta(db, QStringLiteral("input_size"),
                   QString::number(input.size()), error) ||
        !writeMeta(db, QStringLiteral("saved_at"),
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODate),
                   error) ||
        !writeState(db, state, error))
      return fail(error);
    if (!db.commit())
      return fail(db.lastError().text());
  }
  if (fresh) {
    QFile::remove(database);
    if (!QFile::rename(target, database))
      return QCoreApplication::translate("ProjectDatabase", "Cannot create %1")
          .arg(database);
  }
  return {};
}

QString ProjectDatabase::saveState(const QString &database,
                                   const QHash<QString, QByteArray> &state) {
  Connection connection(database);
  if (!connection.error().isEmpty())
    return connection.error();
  auto db = connection.database();
  QString error;
  if (metaValue(db, QStringLiteral("format")).toInt() != FormatVersion)
    return QCoreApplication::translate("ProjectDatabase",
                                       "%1 is not a NeverD database.")
        .arg(database);
  if (!db.transaction())
    return db.lastError().text();
  if (!writeState(db, state, error)) {
    db.rollback();
    return error;
  }
  return db.commit() ? QString() : db.lastError().text();
}

std::optional<ProjectDatabase::Contents>
ProjectDatabase::read(const QString &database, QString *error) {
  const auto failed = [&](const QString &message) {
    if (error)
      *error = message;
    return std::nullopt;
  };
  if (!QFileInfo(database).isFile())
    return failed(
        QCoreApplication::translate("ProjectDatabase", "%1 does not exist.")
            .arg(database));
  Connection connection(database);
  if (!connection.error().isEmpty())
    return failed(connection.error());
  auto db = connection.database();
  const QString format = metaValue(db, QStringLiteral("format"));
  if (format.toInt() != FormatVersion)
    return failed(format.isEmpty()
                      ? QCoreApplication::translate(
                            "ProjectDatabase", "%1 is not a NeverD database.")
                            .arg(database)
                      : QCoreApplication::translate(
                            "ProjectDatabase",
                            "%1 uses database format %2, which this NeverD "
                            "cannot read.")
                            .arg(database, format));
  Contents contents;
  contents.inputName = metaValue(db, QStringLiteral("input_name"));
  contents.inputSha256 = metaValue(db, QStringLiteral("input_sha256"));
  contents.inputSize = metaValue(db, QStringLiteral("input_size")).toLongLong();
  contents.generator = metaValue(db, QStringLiteral("generator"));
  contents.savedAt = metaValue(db, QStringLiteral("saved_at"));
  // A database names its input; never let that name leave the directory.
  if (contents.inputName.isEmpty() ||
      contents.inputName.contains(QLatin1Char('/')) ||
      contents.inputName.contains(QLatin1Char('\\')) ||
      contents.inputName == QLatin1String("..") ||
      contents.inputName == QLatin1String("."))
    return failed(QCoreApplication::translate("ProjectDatabase",
                                              "%1 names an invalid input file.")
                      .arg(database));
  QSqlQuery sidecars(db);
  if (!sidecars.exec(QStringLiteral("SELECT suffix, data FROM sidecars")))
    return failed(lastError(sidecars));
  while (sidecars.next()) {
    const QString suffix = sidecars.value(0).toString();
    if (sidecarSuffixes().contains(suffix))
      contents.sidecars.insert(suffix, sidecars.value(1).toByteArray());
  }
  QSqlQuery state(db);
  if (!state.exec(QStringLiteral("SELECT key, value FROM state")))
    return failed(lastError(state));
  while (state.next())
    contents.state.insert(state.value(0).toString(),
                          state.value(1).toByteArray());
  return contents;
}

QString ProjectDatabase::unpack(const QString &database,
                                const QString &directory, QString *error) {
  const auto failed = [&](const QString &message) {
    if (error)
      *error = message;
    return QString();
  };
  QString message;
  const auto contents = read(database, &message);
  if (!contents)
    return failed(message);
  if (!QDir().mkpath(directory))
    return failed(
        QCoreApplication::translate("ProjectDatabase", "Cannot create %1")
            .arg(directory));
  const QString input = QDir(directory).filePath(contents->inputName);
  QString digestError;
  if (!QFileInfo::exists(input) ||
      sha256(input, digestError) != contents->inputSha256) {
    Connection connection(database);
    if (!connection.error().isEmpty())
      return failed(connection.error());
    auto db = connection.database();
    QSqlQuery chunks(db);
    chunks.prepare(QStringLiteral(
        "SELECT size, data FROM blobs WHERE name = ? ORDER BY chunk"));
    chunks.addBindValue(QString::fromLatin1(InputBlob));
    if (!chunks.exec())
      return failed(lastError(chunks));
    std::vector<std::pair<qint64, QByteArray>> packed;
    while (chunks.next())
      packed.emplace_back(chunks.value(0).toLongLong(),
                          chunks.value(1).toByteArray());
    const auto expanded = QtConcurrent::blockingMapped<std::vector<QByteArray>>(
        packed, [](const std::pair<qint64, QByteArray> &chunk) {
          QByteArray bytes = qUncompress(chunk.second);
          return bytes.size() == chunk.first ? bytes : QByteArray();
        });
    QFile file(input + QStringLiteral(".unpacking"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return failed(
          QCoreApplication::translate("ProjectDatabase", "Cannot write %1: %2")
              .arg(file.fileName(), file.errorString()));
    qint64 total = 0;
    for (std::size_t i = 0; i < expanded.size(); ++i) {
      if (expanded[i].size() != packed[i].first ||
          file.write(expanded[i]) != expanded[i].size()) {
        file.remove();
        return failed(
            QCoreApplication::translate("ProjectDatabase",
                                        "The input stored in %1 is damaged.")
                .arg(database));
      }
      total += expanded[i].size();
    }
    file.close();
    if (total != contents->inputSize ||
        sha256(file.fileName(), digestError) != contents->inputSha256) {
      file.remove();
      return failed(QCoreApplication::translate(
                        "ProjectDatabase", "The input stored in %1 is damaged.")
                        .arg(database));
    }
    QFile::remove(input);
    if (!QFile::rename(file.fileName(), input))
      return failed(
          QCoreApplication::translate("ProjectDatabase", "Cannot write %1")
              .arg(input));
  }
  // A pending journal means the worker will recover newer edits itself.
  if (!QFileInfo::exists(input + QLatin1String(JournalSuffix)))
    for (const auto &suffix : sidecarSuffixes()) {
      const QString path = input + suffix;
      const auto stored = contents->sidecars.constFind(suffix);
      if (stored == contents->sidecars.cend()) {
        QFile::remove(path);
        continue;
      }
      QFile sidecar(path + QStringLiteral(".unpacking"));
      if (!sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
          sidecar.write(*stored) != stored->size()) {
        sidecar.remove();
        return failed(
            QCoreApplication::translate("ProjectDatabase", "Cannot write %1")
                .arg(path));
      }
      sidecar.close();
      QFile::remove(path);
      if (!QFile::rename(sidecar.fileName(), path))
        return failed(
            QCoreApplication::translate("ProjectDatabase", "Cannot write %1")
                .arg(path));
    }
  return input;
}

} // namespace neverd::gui
