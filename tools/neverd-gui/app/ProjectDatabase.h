#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <optional>

namespace neverd::gui {

/// A NeverD database (`.nddb`): one SQLite file holding the analyzed input,
/// the project's sidecar state (comments, renames, edit history) and the
/// workbench state (location, bookmarks, desktop).  It is the packed form of
/// a project; while a binary is open the worker keeps working on the
/// unpacked sidecars beside it, and saving packs them again.
///
/// Every write is one SQLite transaction, so a database is never left half
/// written.  The input is stored in independently compressed chunks that are
/// compressed and expanded in parallel.  A file that is not a NeverD database
/// is never written to.
class ProjectDatabase final {
public:
  static constexpr char Extension[] = ".nddb";
  /// The SQLite application id in every database header: "NDDB" in ASCII,
  /// so NeverD and tools such as file(1) tell a database from other SQLite
  /// files whatever its name.
  static constexpr qint32 ApplicationId = 0x4E444442;
  /// The database of \p binary: the binary's file name plus `.nddb`.
  static QString pathFor(const QString &binary);
  /// Whether \p path is opened as a database: it has the `.nddb` suffix or
  /// its header carries the application id.
  static bool isDatabase(const QString &path);
  /// Whether NeverD keeps a project for \p path: \p path is a database, or
  /// the binary has a database or a sidecar beside it.
  static bool hasState(const QString &path);

  struct Contents {
    QString inputName, inputSha256;
    qint64 inputSize = 0;
    QString generator, savedAt;
    /// Sidecar file suffix (".neverd-annotations.json") -> bytes.
    QHash<QString, QByteArray> sidecars;
    /// Workbench state key -> JSON bytes.
    QHash<QString, QByteArray> state;
  };

  /// Pack \p binary, the sidecars beside it and \p state into \p database.
  /// The input is rewritten only when its digest changed.  Returns an error
  /// message, or an empty string on success.
  static QString save(const QString &database, const QString &binary,
                      const QHash<QString, QByteArray> &state);
  /// Replace only the workbench state of an existing database.
  static QString saveState(const QString &database,
                           const QHash<QString, QByteArray> &state);
  /// Read everything but the input bytes.
  static std::optional<Contents> read(const QString &database,
                                      QString *error = nullptr);
  /// Unpack the input and its sidecars into \p directory, returning the
  /// input path.  An existing input with the recorded digest is reused, and
  /// sidecars with a pending crash-recovery journal are left for the worker.
  static QString unpack(const QString &database, const QString &directory,
                        QString *error = nullptr);
  /// Where a database without its original input is unpacked.
  static QString workingDirectory(const QString &database);

  /// Sidecar suffixes the worker keeps beside an input.
  static const QStringList &sidecarSuffixes();
};

} // namespace neverd::gui
