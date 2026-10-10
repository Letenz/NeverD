#pragma once

#include "EngineClient.h"
#include "QueryService.h"

#include <array>
#include <memory>
#include <optional>

namespace neverd::gui {

/// Disposable read-only executor for expensive views. Project writes and
/// navigation keep their own worker; a cancelled synchronous engine call
/// can be stopped here without losing the owner's staged changes.
class AnalysisService final : public QObject {
  Q_OBJECT
public:
  AnalysisService(QueryService &owner, QString workerPath,
                  QObject *parent = nullptr);
  ~AnalysisService() override;
  QueryService &queries() { return queries_; }
  bool ready() const { return ready_; }
  static bool handles(const QString &operation);

signals:
  void diagnostic(const QString &message);

private:
  QString send(const QString &operation, const QJsonObject &payload);
  void reset();
  void stopWorker();
  void receive(const QJsonObject &message);
  void dispatch();
  void finish(QJsonObject response);
  void fail(const QString &code, const QString &message);

  QueryService &owner_;
  EngineClient client_;
  QueryService queries_;
  QString workerPath_, active_, operation_, wire_, restore_, workerRevision_;
  QJsonObject payload_, snapshot_;
  QueryService::SubscriptionId snapshotRequest_ = 0;
  quint64 serial_ = 0, next_ = 0;
  bool ready_ = false;
};

/// Bounded independent replicas. A function's pages and graph transaction stay
/// on one dispatcher; another function can use the other replica concurrently.
class AnalysisPool final : public QObject {
  Q_OBJECT
public:
  AnalysisPool(QueryService &owner, const QString &workerPath,
               QObject *parent = nullptr);
  QueryService &queries(const QJsonObject &payload, QObject *owner);
  // External clients can split a graph summary and viewport across requests.
  // Keep that stateful protocol on one replica, as before the pool existed.
  QueryService &externalQueries() { return lanes_.front().service->queries(); }
  void unsubscribeOwner(QObject *owner);
  void cancelReads(const QSet<QObject *> &keep = {});

signals:
  void diagnostic(const QString &message);

private:
  struct Lane {
    std::unique_ptr<AnalysisService> service;
    std::optional<quint64> function;
  };
  struct Binding {
    quint64 function;
    size_t lane;
    QMetaObject::Connection destroyed;
  };
  std::array<Lane, 2> lanes_;
  QHash<QObject *, Binding> bindings_;
};
} // namespace neverd::gui
