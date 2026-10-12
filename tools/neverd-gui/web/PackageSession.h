//===- PackageSession.h - Offline package workbench owner -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Serial requests to one private NeverD worker. Evidence semantics and review
/// transitions remain in the shared backend.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "EngineClient.h"
#include "ReviewJournal.h"

#include <QJsonArray>
#include <QTimer>
#include <functional>

namespace neverd::gui::web {

struct RegistrySelection {
  QString Original, Metadata, Descriptor;
  bool empty() const {
    return Original.isEmpty() && Metadata.isEmpty() && Descriptor.isEmpty();
  }
  bool complete() const {
    return !Original.isEmpty() && !Metadata.isEmpty() && !Descriptor.isEmpty();
  }
};

class PackageSession final : public QObject {
  Q_OBJECT
public:
  explicit PackageSession(const QString &WorkerPath, QObject *Parent = nullptr);
  bool ready() const { return Ready; }
  bool busy() const { return Busy; }
  bool canRestore() const { return RestoreAvailable; }
  const QString &error() const { return Error; }
  const QJsonObject &preview() const { return Preview; }
  const QJsonArray &artifacts() const { return Artifacts; }
  const QJsonArray &members() const { return Members; }
  const QJsonObject &assessment() const { return Assessment; }
  const QJsonArray &findings() const { return Findings; }
  QJsonObject journal() const {
    return ReviewJournal::create(Assessment, Decisions);
  }

  void previewCapture(const QString &Path);
  void commitPreview();
  void extractArchive(const QString &ArtifactID, const QString &Format);
  void assess(const QString &ManifestID, const QString &Kind,
              RegistrySelection Registry = {});
  void review(const QString &FindingID, const QString &Disposition);
  /// Loaded metadata is applied only by this explicit user action. The backend
  /// validates the whole chain and commits one atomic suffix transition.
  void restore(const QJsonObject &Journal);
  void cancel();

signals:
  void changed();
  void previewReady();
  void artifactsReady();
  void membersReady();
  void assessmentReady();
  void failed(const QString &Code);

private:
  using Reply = std::function<void(const QJsonObject &)>;
  void receive(const QJsonObject &Message);
  void request(const QString &Operation, QJsonObject Payload, Reply Done);
  void failure(const QString &Code);
  void clearAssessment();
  void createAssessment(const QString &PackageID, QJsonObject Selection);
  void refreshAssessment();
  void pages(const QString &Operation, QJsonObject Query, qsizetype Maximum,
             qsizetype ByteLimit, std::function<void(QJsonArray)> Done);
  void page(const QString &Operation, QJsonObject Query, qsizetype Offset,
            QJsonArray Rows, qsizetype Bytes, qsizetype Maximum,
            qsizetype ByteLimit, std::function<void(QJsonArray)> Done);

  EngineClient Client;
  QTimer Deadline;
  QString PendingID, Revision, Error;
  Reply PendingReply;
  QJsonObject Preview, Assessment;
  QJsonArray Artifacts, Members, Findings, Decisions;
  bool Ready = false, Busy = true, RestoreAvailable = false;
};

} // namespace neverd::gui::web
