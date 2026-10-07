#pragma once

#include "Address.h"
#include "EngineClient.h"
#include "QueryService.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>
#include <optional>

namespace neverd::gui {

/// One analysis worker and the binary it has open.  The session starts the
/// worker before a file is chosen (hiding engine start-up), owns the request
/// dispatcher, publishes background-index progress from heartbeats, and runs
/// edits with explicit Save/Discard/Cancel transitions for staged work.
class Session final : public QObject {
  Q_OBJECT
public:
  explicit Session(QString workerPath, QObject *parent = nullptr);
  ~Session() override;

  QueryService &queries() { return queries_; }

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
  void open(const QString &path);
  void closeFile();
  void reload();
  void restart();
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
  void rename(Address function, const QString &name,
              std::optional<quint64> epoch = {});
  void setComment(Address address, const QString &text,
                  std::optional<quint64> epoch = {});
  void undo();
  void redo();
  void loadSignatures(const QString &path, bool tree);
  void analyzeWholeProgram();
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

private:
  using Callback = std::function<void(const QJsonObject &payload)>;
  void command(const QString &operation, const QJsonObject &payload,
               Callback done, Failure failed = {});
  void receive(const QJsonObject &message);
  void startWorker();
  void openPending();
  void sendOpen(const QString &requested, const QString &path,
                const QString &database,
                const QHash<QString, QByteArray> &state);
  void packDatabase();
  void requestTransition(const QString &action);
  void finishTransition();
  void refreshHistory();
  void refreshContributions();
  void resetState();
  void setError(const QString &text);
  /// Whether an edit aimed at \p epoch may be applied now.
  bool acceptsEdit(std::optional<quint64> epoch);

  EngineClient client_;
  QueryService queries_;
  QObject reads_, external_;
  QString workerPath_, filePath_, pendingFile_, transition_, error_;
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
