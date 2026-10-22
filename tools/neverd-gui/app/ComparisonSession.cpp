//===- ComparisonSession.cpp - Asynchronous comparison workspace ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// UI-thread dispatch; native work and framing run in the existing worker
/// transport. Cancellation is acknowledged separately from terminal completion.
//===----------------------------------------------------------------------===//

#include "ComparisonSession.h"

#include "../ResourceLimits.h"

#include <QJsonArray>
#include <QTimer>

namespace neverd::gui {
ComparisonSession::ComparisonSession(const QString &Worker, QObject *Parent)
    : QObject(Parent), Client(this) {
  connect(&Client, &EngineClient::message, this, &ComparisonSession::receive);
  connect(&Client, &EngineClient::failure, this,
          [this](const QString &Message) {
            Ready = Busy = false;
            Cancellable.clear();
            Requests.clear();
            emit changed();
            emit failed(tr("Comparison worker: %1").arg(Message));
          });
  Client.start(Worker);
}
void ComparisonSession::receive(const QJsonObject &Message) {
  if (Message["type"] == QJsonValue("hello")) {
    CancellableOperations.clear();
    for (const auto &Operation :
         Message["comparison_cancellable_operations"].toArray())
      if (Operation.isString())
        CancellableOperations.append(Operation.toString());
    Ready = Message["protocol_major"] == QJsonValue(1) &&
            Message["capabilities"].toArray().contains("comparison_start") &&
            CancellableOperations.contains("comparison_start");
    emit changed();
    if (!Ready)
      emit failed(tr("This worker does not support binary comparison."));
    return;
  }
  if (Message["type"] != QJsonValue("response"))
    return;
  const auto ID = Message["request_id"].toString();
  if (Message["status"] == QJsonValue("progress")) {
    if (Cancellable.contains(ID))
      emit progress(Message["payload"].toObject());
    return;
  }
  auto It = Requests.find(ID);
  if (It == Requests.end())
    return;
  auto Pending = std::move(It.value());
  Requests.erase(It);
  const bool WasCancellable = Cancellable.removeAll(ID) != 0;
  if (Pending.Owner && Pending.Complete)
    Pending.Complete(Message);
  if (WasCancellable) {
    emit changed();
  }
}
void ComparisonSession::request(const QString &Operation, QJsonObject Payload,
                                QObject *Owner, Reply Complete) {
  if (CancellableOperations.contains(Operation) && CancelRequested &&
      cancellable()) {
    QTimer::singleShot(0, Owner, [Complete = std::move(Complete)] {
      Complete(
          {{"status", "error"},
           {"error", QJsonObject{{"code", "cancelled"},
                                 {"message", "Analysis request cancelled"}}}});
    });
    return;
  }
  if (!Ready ||
      (Operation != "cancel" &&
       Requests.size() >= ::neverd::limits::gui::kComparisonPendingRequests)) {
    QTimer::singleShot(0, Owner, [Complete = std::move(Complete)] {
      Complete({{"status", "error"},
                {"error",
                 QJsonObject{{"code", "comparison_busy"},
                             {"message",
                              "Comparison worker is unavailable or busy"}}}});
    });
    return;
  }
  if (!revision().isEmpty() && Operation != "comparison_start" &&
      Operation != "cancel")
    Payload["expected_comparison_revision"] = revision();
  const auto ID = Client.request(Operation, Payload);
  Requests.insert(ID, Pending{Owner, std::move(Complete)});
  if (CancellableOperations.contains(Operation)) {
    if (Cancellable.isEmpty())
      CancelRequested = false;
    Cancellable.append(ID);
    emit changed();
  }
}
void ComparisonSession::start(const QJsonObject &Primary,
                              const QJsonObject &Secondary,
                              const QJsonObject &Options,
                              const QString &Saved) {
  QJsonObject Payload{
      {"primary", Primary}, {"secondary", Secondary}, {"options", Options}};
  if (!Saved.isEmpty())
    Payload["restore_path"] = Saved;
  command("comparison_start", Payload);
}
void ComparisonSession::command(const QString &Operation,
                                const QJsonObject &Payload) {
  if (!Ready || Busy)
    return;
  Busy = true;
  CancelRequested = false;
  emit changed();
  request(Operation, Payload, this, [this, Operation](const auto &Message) {
    Busy = false;
    if (Message["status"] != QJsonValue("ok")) {
      emit changed();
      const auto Error = Message["error"].toObject();
      emit failed(tr("%1: %2").arg(Error["code"].toString(),
                                   Error["message"].toString()));
      return;
    }
    const auto Result = Message["payload"].toObject();
    if (Result["schema"] == QJsonValue("neverd.bindiff.summary")) {
      const auto Previous = revision();
      Summary = Result;
      if (Previous != revision())
        emit mappingChanged();
    }
    emit changed();
    if (Operation == "comparison_save")
      emit saved(Result["path"].toString());
  });
}
void ComparisonSession::cancel() {
  if (Cancellable.isEmpty() || CancelRequested)
    return;
  CancelRequested = true;
  for (const auto &ID : Cancellable)
    request("cancel", {{"request_id", ID}}, this, [this](const auto &Message) {
      if (Message["status"] != QJsonValue("ok"))
        emit failed(tr("The worker could not acknowledge cancellation."));
    });
  emit changed();
}
} // namespace neverd::gui
