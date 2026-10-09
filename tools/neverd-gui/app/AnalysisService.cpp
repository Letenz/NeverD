#include "AnalysisService.h"

#include <QJsonArray>
#include <QTimer>
#include <utility>

namespace neverd::gui {
AnalysisService::AnalysisService(QueryService &owner, QString workerPath,
                                 QObject *parent)
    : QObject(parent), owner_(owner),
      queries_([this](const QString &op, const QJsonObject &payload,
                      const QString &) { return send(op, payload); }),
      workerPath_(std::move(workerPath)) {
  connect(&owner_, &QueryService::contextChanged, this,
          &AnalysisService::reset);
  connect(&client_, &EngineClient::message, this, &AnalysisService::receive);
  connect(&client_, &EngineClient::diagnostic, this,
          &AnalysisService::diagnostic);
  connect(&client_, &EngineClient::failure, this, [this](const QString &text) {
    fail(QStringLiteral("analysis_failed"), text);
  });
  connect(&client_, &EngineClient::stopped, this, [this] {
    ready_ = false;
    if (!active_.isEmpty())
      fail(QStringLiteral("analysis_failed"), tr("Analysis worker stopped."));
  });
}

AnalysisService::~AnalysisService() {
  disconnect(&client_, nullptr, this, nullptr);
  owner_.unsubscribeOwner(this);
}

bool AnalysisService::handles(const QString &operation) {
  return operation == QLatin1String("decompile") ||
         operation == QLatin1String("cfg") ||
         operation == QLatin1String("cfg_summary") ||
         operation == QLatin1String("cfg_viewport");
}

void AnalysisService::stopWorker() {
  ++serial_;
  if (snapshotRequest_)
    owner_.unsubscribe(std::exchange(snapshotRequest_, 0));
  wire_.clear();
  restore_.clear();
  ready_ = false;
  snapshot_ = {};
  client_.stop();
}

void AnalysisService::reset() {
  active_.clear();
  stopWorker();
  queries_.resetReadContext(owner_.projectId(), owner_.revision());
}

QString AnalysisService::send(const QString &operation,
                              const QJsonObject &payload) {
  const QString id = QString::number(++next_);
  if (operation == QLatin1String("cancel")) {
    if (payload.value("request_id").toString() == active_) {
      const QString cancelled = std::exchange(active_, {});
      const QString op = operation_;
      stopWorker();
      // Release this dispatcher's active slot only after the executor has
      // been retired. EngineClient filters all replies from its old epoch.
      QTimer::singleShot(0, this, [this, cancelled, op] {
        queries_.receive({{"type", "response"},
                          {"request_id", cancelled},
                          {"operation", op},
                          {"status", "cancelled"}});
      });
    }
    return id;
  }
  active_ = id;
  operation_ = operation;
  payload_ = payload;
  const auto serial = serial_;
  // Sender replies must be asynchronous, including validation failures.
  QTimer::singleShot(0, this, [this, serial, id] {
    if (serial != serial_ || id != active_)
      return;
    if (!handles(operation_)) {
      fail(QStringLiteral("unsupported"),
           tr("Only analysis reads are allowed."));
      return;
    }
    if (ready_) {
      dispatch();
      return;
    }
    snapshotRequest_ = owner_.subscribe(
        {QStringLiteral("analysis_snapshot"), {}}, this,
        [this, serial, id](const QJsonObject &response) {
          if (serial != serial_ || active_ != id)
            return;
          snapshotRequest_ = 0;
          if (response.value("status") != QLatin1String("ok")) {
            finish(response);
            return;
          }
          snapshot_ = response.value("payload").toObject();
          client_.start(workerPath_);
        });
  });
  return id;
}

void AnalysisService::receive(const QJsonObject &message) {
  if (active_.isEmpty())
    return;
  if (message.value("type") == QLatin1String("hello")) {
    if (!message.value("capabilities")
             .toArray()
             .contains(QStringLiteral("analysis_restore"))) {
      fail(QStringLiteral("unsupported"),
           tr("The worker cannot restore analysis state."));
      return;
    }
    restore_ = client_.request(QStringLiteral("analysis_restore"), snapshot_);
    return;
  }
  if (message.value("type") != QLatin1String("response") ||
      message.value("status") == QLatin1String("progress"))
    return;
  const auto id = message.value("request_id").toString();
  if (!restore_.isEmpty() && id == restore_) {
    restore_.clear();
    if (message.value("status") != QLatin1String("ok")) {
      finish(message);
      client_.stop();
      return;
    }
    snapshot_ = {};
    workerRevision_ = message.value("revision").toString();
    ready_ = true;
    dispatch();
  } else if (!wire_.isEmpty() && id == wire_) {
    workerRevision_ = message.value("revision").toString();
    finish(message);
  }
}

void AnalysisService::dispatch() {
  wire_ = client_.request(operation_, payload_, workerRevision_);
}

void AnalysisService::finish(QJsonObject response) {
  if (active_.isEmpty())
    return;
  response["type"] = "response";
  response["request_id"] = std::exchange(active_, {});
  response["operation"] = operation_;
  response["project_id"] = owner_.projectId();
  response["revision"] = owner_.revision();
  // A replica's local pipeline/revision never advances the project owner's
  // analysis state, revision or function discovery.
  response.remove("analysis_state");
  auto payload = response.value("payload").toObject();
  if (payload.contains("project_id"))
    payload["project_id"] = owner_.projectId();
  if (payload.contains("revision"))
    payload["revision"] = owner_.revision();
  response["payload"] = payload;
  wire_.clear();
  queries_.receive(response);
}

void AnalysisService::fail(const QString &code, const QString &message) {
  finish({{"status", "error"},
          {"error", QJsonObject{{"code", code}, {"message", message}}}});
}
} // namespace neverd::gui
