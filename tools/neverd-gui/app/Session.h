#pragma once

#include "Address.h"
#include "AnalysisService.h"
#include "EngineClient.h"
#include "QueryService.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <functional>
#include <optional>

namespace neverd::gui {

/// How to load a binary, as the load dialog chooses it.
struct LoadOptions {
  /// Read the debug information that belongs to the input.
  bool debugInfo = true;
  /// Analyze in idle time: function discovery and the reference index.
  bool analysis = true;
  /// The loader the user chose for a file that names no format itself:
  /// "evm" for EVM bytecode, or "binary" as the fields below place it; empty
  /// reads the file as its header or contents say.
  QString loader;
  /// A binary file: the processor its code is read as ("x86_64", "thumb",
  /// ...), the address its bytes map at, the bytes from offset on (size 0
  /// for the rest of the file), and where execution starts (the base when
  /// unset).
  QString processor;
  quint64 base = 0, offset = 0, size = 0;
  std::optional<quint64> entry;
  /// The platform whose conventions a binary file's code follows: "sysv",
  /// "windows" or "darwin"; empty reads it from the code.
  QString platform;
};

/// The project's writable worker and a disposable read-only analysis worker.
/// Browsing and edits keep the owner dispatcher while expensive views use a
/// verified replica. Save/Discard/Cancel transitions belong to the owner.
class Session final : public QObject {
  Q_OBJECT
public:
  explicit Session(QString workerPath, QObject *parent = nullptr);
  ~Session() override;

  QueryService &queries() { return queries_; }
  QueryService &analysisQueries() { return analysis_.queries(); }

  bool connected() const { return connected_; }
  bool loaded() const { return loaded_; }
  bool opening() const { return opening_; }
  bool dirty() const { return dirty_ || pendingWrites_ > 0; }
  bool readOnly() const { return metadata_.value("read_only").toBool(); }
  /// The input the worker analyzes (a database's unpacked copy).
  QString filePath() const { return filePath_; }
  /// What the user opened: a binary or a NeverD database.
  QString projectPath() const { return projectPath_; }
  /// The database this project saves to (it may not exist yet).
  QString databasePath() const { return databasePath_; }
  /// Workbench state recorded in the database when the project opened.
  const QHash<QString, QByteArray> &databaseState() const {
    return databaseState_;
  }
  /// Supplies the workbench state that saving packs into the database.
  void setStateProvider(std::function<QHash<QString, QByteArray>()> provider) {
    stateProvider_ = std::move(provider);
  }
  QString fileName() const;
  const QJsonObject &metadata() const { return metadata_; }
  QString architecture() const {
    return metadata_.value("architecture").toString();
  }
  QString format() const { return metadata_.value("format").toString(); }
  /// The platform whose conventions a binary file's code follows, as the
  /// user reads it; empty for a file that names its own format.
  QString platformName() const;
  int bitness() const { return metadata_.value("bitness").toInt(64); }
  Address entryAddress() const;
  QString revision() const { return queries_.revision(); }
  QString lastError() const { return error_; }

  /// Background reference-index state from the latest heartbeat.
  const QJsonObject &background() const { return background_; }
  bool indexReady() const;
  /// Changes whenever listing text can change without a revision change.
  QString generation() const { return generation_; }
  /// Requests are queued or the background index is still building.
  bool busy() const;

  const QJsonObject &history() const { return history_; }
  bool canUndo() const { return history_.value("can_undo").toBool(); }
  bool canRedo() const { return history_.value("can_redo").toBool(); }

  // Session transitions.
  void open(const QString &path, LoadOptions options = {});
  /// Whether the worker lists the ways a file can be loaded (identify).
  bool identifiesFiles() const {
    return capabilities_.contains(QStringLiteral("identify"));
  }
  void closeFile();
  /// Read the input file again through a fresh worker, as IDA's "Reload the
  /// input file": its bytes as they are now, with the saved names, comments
  /// and other annotations.  \p options reads a binary file another way.
  void reload(std::optional<LoadOptions> options = std::nullopt);
  void restart();
  /// How the engine loaded the open file (its load_options): the loader, and
  /// for a binary file the processor, base, offset, size, entry and platform.
  QJsonObject loadOptionsJson() const {
    return metadata_.value(QStringLiteral("load_options")).toObject();
  }
  /// Returns true when quitting may proceed immediately; otherwise a
  /// transition was requested and quitApproved() follows a decision.
  bool requestQuit();
  /// "save", "discard" or "cancel" for the pending transition.
  void resolveTransition(const QString &choice);

  /// Changes whenever the worker session changes (open, restart, close).
  /// Commands captured under an older epoch are refused.
  quint64 epoch() const { return queries_.sessionEpoch(); }

  // Edits.  \p epoch is the session the user aimed at, when an edit was
  // prepared across a dialog or other asynchronous step.
  /// Commit staged edits and pack the project into its database.
  void save();
  /// Update only the workbench state of an existing database.
  void saveDatabaseState();
  /// Whether the worker's engine keeps function edits.
  bool keepsFunctionEdits() const {
    return capabilities_.contains(QStringLiteral("function_create"));
  }
  /// Whether the worker's engine keeps how the user shows operands' numbers.
  bool keepsOperandFormats() const {
    return capabilities_.contains(QStringLiteral("operand_format"));
  }
  /// Show a number operand of the instruction at \p address as \p action
  /// says: a base of OperandFormats.def, "negate" or "invert" to toggle the
  /// sign or the bits.  The worker formats \p operand if it is a number, and
  /// else the instruction's last number.  Commits at once, like a rename.
  void formatOperand(Address address, std::optional<int> operand,
                     const QString &action, std::optional<quint64> epoch = {});
  /// Whether the worker's engine keeps the user's data items.
  bool keepsDataItems() const {
    return capabilities_.contains(QStringLiteral("item_define"));
  }
  /// Define the item at \p address: \p action "data" makes a value, the
  /// next size each time; "string" the string that starts there; "undefine"
  /// shows the item's bytes as bytes.  Each commits at once, like a rename.
  void defineItem(Address address, const QString &action,
                  std::optional<quint64> epoch = {});
  /// Start a function at \p address, or stop treating the function at
  /// \p entry as one.  Each commits at once, like a rename.
  void createFunction(Address address, std::optional<quint64> epoch = {});
  void deleteFunction(Address entry, std::optional<quint64> epoch = {});
  void rename(Address function, const QString &name,
              std::optional<quint64> epoch = {});
  void editCode(const QJsonObject &edit, quint64 epoch);
  void setComment(Address address, const QString &text,
                  std::optional<quint64> epoch = {});
  void undo();
  void redo();
  void loadSignatures(const QString &path, bool tree);
  void analyzeWholeProgram();
  /// Search strings in \p encodings (engine names), C strings that are not
  /// UTF-8 in the code page \p preferred first (none when empty), and of at
  /// least \p minLength display columns.  Remembered and applied to every
  /// worker that starts.
  void setStringOptions(const QStringList &encodings, const QString &preferred,
                        int minLength);
  void cancelReads();

  // Declarative extension manifests.
  const QJsonArray &contributions() const { return contributions_; }
  void registerContributions(const QString &manifestPath);
  void unregisterContributions(const QString &nameSpace);
  void executeContribution(const QString &id, std::optional<Address> address);

  // Reads.
  using Reply = std::function<void(const QJsonObject &payload)>;
  using Failure =
      std::function<void(const QString &code, const QString &message)>;
  /// Issue a read owned by \p owner.  Failures are reported to \p failed when
  /// given, otherwise logged.  Cancellation and session changes are silent.
  QueryService::SubscriptionId read(const QString &operation,
                                    const QJsonObject &payload, QObject *owner,
                                    Reply done, Failure failed = {});

  // MCP bridge.
  void externalQuery(const QString &id, const QString &operation,
                     const QJsonObject &payload, const QString &revision);
  void publishSelection(Address address, std::optional<Address> function,
                        const QString &view);
  QJsonObject selection() const { return selection_; }

signals:
  void stateChanged();
  void opened();
  void unloaded();
  void revisionChanged();
  void generationChanged();
  /// The worker lists a different number of functions, for example after its
  /// function detector ran.
  void functionsChanged();
  void backgroundChanged();
  void historyChanged();
  void loadProgress(const QString &phase, double fraction);
  /// level: 0 information, 1 warning, 2 error.
  void message(const QString &text, int level);
  void transitionRequested(const QString &action);
  void quitApproved();
  void selectionChanged(const QJsonObject &selection);
  void externalResponse(const QString &id, const QJsonObject &response);
  void renamed(Address address, const QString &name);
  void commented(Address address, const QString &text);
  void contributionsChanged();
  void databaseSaved(bool ok);
  void contributionResult(const QJsonObject &result);
  /// The worker searches strings with new options; string views reload.
  void stringOptionsChanged();

private:
  /// The operations the worker offers (its hello).
  QSet<QString> capabilities_;
  using Callback = std::function<void(const QJsonObject &payload)>;
  void command(const QString &operation, const QJsonObject &payload,
               Callback done, Failure failed = {});
  void receive(const QJsonObject &message);
  void startWorker();
  void openPending();
  void sendOpen(const QString &requested, const QString &path,
                const QString &database,
                const QHash<QString, QByteArray> &state, LoadOptions options);
  void packDatabase();
  void requestTransition(const QString &action);
  void finishTransition();
  void refreshHistory();
  void refreshContributions();
  /// Send the remembered string options, if any; \p announce reports them.
  void applyStringOptions(bool announce);
  /// Whether the remembered string options predate the preferred code page.
  bool stringOptionsNeedMigration() const;
  /// Rewrites string options that named at most one code page, the one
  /// searched, given the engine's \p encodings: it becomes the preferred
  /// page, and the encodings searched by default join the list.
  void migrateStringOptions(const QJsonArray &encodings);
  void resetState();
  void setError(const QString &text);
  /// Whether an edit aimed at \p epoch may be applied now.
  bool acceptsEdit(std::optional<quint64> epoch);

  EngineClient client_;
  QueryService queries_;
  AnalysisService analysis_;
  QObject reads_, external_;
  QString workerPath_, filePath_, pendingFile_, transition_, error_;
  /// How the pending file loads, and how the open file loaded, which a
  /// restart repeats.
  LoadOptions pendingOptions_, loadOptions_;
  /// How a reload waiting for a Save/Discard decision reads the file.
  std::optional<LoadOptions> reloadOptions_;
  /// The open in flight reads the open file again.
  bool reloading_ = false;
  QString projectPath_, databasePath_;
  QHash<QString, QByteArray> databaseState_;
  std::function<QHash<QString, QByteArray>()> stateProvider_;
  QString generation_;
  qint64 functionCount_ = -1;
  QJsonObject metadata_, background_, history_, selection_;
  QJsonArray contributions_;
  quint64 sessionEpoch_ = 0;
  int pendingWrites_ = 0;
  bool connected_ = false, loaded_ = false, opening_ = false, dirty_ = false;
  bool transitionReady_ = false, restartPending_ = false, starting_ = false;
};

} // namespace neverd::gui
