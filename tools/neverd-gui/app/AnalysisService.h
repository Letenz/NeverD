#pragma once

#include "EngineClient.h"
#include "QueryService.h"

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
} // namespace neverd::gui
