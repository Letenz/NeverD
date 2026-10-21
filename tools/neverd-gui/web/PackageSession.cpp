//===- PackageSession.cpp - Revision-bound offline GUI requests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Capture, select and review retained backend evidence. Pages are published
/// only when their revision and review head agree throughout the transaction.
///
//===----------------------------------------------------------------------===//

#include "PackageSession.h"

#include "../ResourceLimits.h"

#include <QJsonDocument>
#include <QSet>

namespace neverd::gui::web {
namespace {

bool hasOperation(const QJsonObject &Capabilities, const QString &Name) {
  for (const auto &V : Capabilities["operations"].toArray())
    if (V.toObject()["name"].toString() == Name || V.toString() == Name)
      return true;
  return false;
}
} // namespace

PackageSession::PackageSession(const QString &WorkerPath, QObject *Parent)
    : QObject(Parent) {
  Deadline.setSingleShot(true);
  Deadline.setInterval(::neverd::limits::gui::kPackageSessionRequestTimeoutMs);
  connect(&Deadline, &QTimer::timeout, this, [this] {
    Client.stop();
    failure(QStringLiteral("web_request_timeout"));
  });
  connect(&Client, &EngineClient::message, this, &PackageSession::receive);
  connect(&Client, &EngineClient::failure, this, [this] {
    Ready = false;
    failure(QStringLiteral("web_worker_failed"));
  });
  connect(&Client, &EngineClient::stopped, this, [this] {
    Ready = false;
    failure(QStringLiteral("web_worker_stopped"));
  });
  Deadline.start();
  Client.start(WorkerPath);
}

void PackageSession::failure(const QString &Code) {
  Deadline.stop();
  PendingID.clear();
  PendingReply = {};
  Busy = false;
  Error = Code;
  emit changed();
  emit failed(Code);
}

void PackageSession::receive(const QJsonObject &Message) {
  if (Message["type"] == QJsonValue("hello")) {
    if (Message["protocol_major"] != QJsonValue(1)) {
      failure(QStringLiteral("unsupported_worker_protocol"));
      return;
    }
    request(QStringLiteral("web_capabilities"), {}, [this](const auto &Caps) {
      Ready = hasOperation(Caps, QStringLiteral("package_assessment_create"));
      RestoreAvailable =
          hasOperation(Caps, QStringLiteral("package_review_restore"));
      if (!Ready)
        failure(QStringLiteral("capability_unavailable"));
    });
    return;
  }
  if (Message["type"] != QJsonValue("response") ||
      Message["request_id"].toString() != PendingID || PendingID.isEmpty())
    return;
  Deadline.stop();
  auto Done = std::move(PendingReply);
  PendingID.clear();
  PendingReply = {};
  if (Message["status"] != QJsonValue("ok")) {
    failure(Message["error"].toObject()["code"].toString(
        QStringLiteral("invalid_web_response")));
    return;
  }
  const auto Payload = Message["payload"].toObject();
  if (Payload["status"] == QJsonValue("error")) {
    failure(Payload["error"].toObject()["code"].toString(
        QStringLiteral("invalid_web_response")));
    return;
  }
  if (Payload["schema_version"] != QJsonValue(1) ||
      Payload["status"] != QJsonValue("ok")) {
    failure(QStringLiteral("invalid_web_response"));
    return;
  }
  if (Done)
    Done(Payload);
  if (PendingID.isEmpty()) {
    Busy = false;
    emit changed();
  }
}

void PackageSession::request(const QString &Operation, QJsonObject Payload,
                             Reply Done) {
  if (!PendingID.isEmpty()) {
    failure(QStringLiteral("web_request_in_progress"));
    return;
  }
  Error.clear();
  Busy = true;
  Payload["schema_version"] = 1;
  if (!Revision.isEmpty() && Operation != u"web_import_preview" &&
      Operation != u"web_import_commit" && Operation != u"web_capabilities")
    Payload["revision"] = Revision;
  PendingReply = std::move(Done);
  PendingID = Client.request(Operation, Payload);
  Deadline.start();
  emit changed();
}

void PackageSession::clearAssessment() {
  Assessment = {};
  Findings = {};
  Decisions = {};
}

void PackageSession::previewCapture(const QString &Path) {
  if (!Ready || Busy)
    return;
  Preview = {};
  request(QStringLiteral("web_import_preview"), {{"path", Path}},
          [this](const auto &Result) {
            Preview = Result;
            emit previewReady();
          });
}

void PackageSession::commitPreview() {
  if (!Ready || Busy || Preview["preview_token"].toString().isEmpty())
    return;
  request(QStringLiteral("web_import_commit"),
          {{"preview_token", Preview["preview_token"]}},
          [this](const auto &Result) {
            Revision = Result["revision"].toString();
            Preview = {};
            Artifacts = {};
            Members = {};
            clearAssessment();
            pages(QStringLiteral("web_artifacts"), {},
                  ::neverd::limits::gui::kPackageSessionMaxInventoryRows,
                  ::neverd::limits::gui::kPackageSessionMaxMetadataBytes,
                  [this](auto Rows) {
                    Artifacts = std::move(Rows);
                    emit artifactsReady();
                  });
          });
}

void PackageSession::extractArchive(const QString &ArtifactID,
                                    const QString &Format) {
  if (!Ready || Busy || Revision.isEmpty())
    return;
  Members = {};
  clearAssessment();
  request(QStringLiteral("web_package_archive_extract"),
          {{"artifact_id", ArtifactID}, {"format", Format}},
          [this](const auto &Result) {
            pages(QStringLiteral("web_package_archive_records"),
                  {{"archive_id", Result["archive_id"]}},
                  ::neverd::limits::gui::kPackageSessionMaxInventoryRows,
                  ::neverd::limits::gui::kPackageSessionMaxMetadataBytes,
                  [this](auto Rows) {
                    Members = std::move(Rows);
                    emit membersReady();
                  });
          });
}

void PackageSession::assess(const QString &ManifestID, const QString &Kind,
                            RegistrySelection Registry) {
  if (!Ready || Busy || Revision.isEmpty())
    return;
  if (!Registry.empty() && !Registry.complete()) {
    failure(QStringLiteral("incomplete_registry_selection"));
    return;
  }
  clearAssessment();
  request(QStringLiteral("web_packages_analyze"),
          {{"artifact_id", ManifestID}, {"input_kind", Kind}},
          [this, Registry](const auto &Result) {
            const auto PackageID = Result["package_analysis_id"].toString();
            if (Registry.empty()) {
              createAssessment(PackageID, {});
              return;
            }
            request(QStringLiteral("web_package_integrity_verify"),
                    {{"artifact_id", Registry.Original},
                     {"declaration_id", Registry.Metadata}},
                    [this, PackageID, Registry](const auto &Integrity) {
                      const auto Receipt = Integrity["integrity_id"];
                      request(
                          QStringLiteral("web_package_attestation_import"),
                          {{"artifact_id", Registry.Metadata},
                           {"descriptor_artifact_id", Registry.Descriptor},
                           {"input_kind", "registry-dist"}},
                          [this, PackageID, Receipt](const auto &Attestation) {
                            createAssessment(
                                PackageID,
                                {{"integrity_ids", QJsonArray{Receipt}},
                                 {"attestation_ids",
                                  QJsonArray{
                                      Attestation["package_attestation_id"]}}});
                          });
                    });
          });
}

void PackageSession::createAssessment(const QString &PackageID,
                                      QJsonObject Selection) {
  Selection["package_analysis_id"] = PackageID;
  request(QStringLiteral("web_package_assessment_create"), Selection,
          [this](const auto &Result) {
            Assessment = Result;
            refreshAssessment();
          });
}

void PackageSession::refreshAssessment() {
  Findings = {};
  Decisions = {};
  const QJsonObject Query{
      {"package_assessment_id", Assessment["package_assessment_id"]},
      {"review_head", Assessment["review_head_id"]}};
  pages(QStringLiteral("web_package_assessment_findings"), Query,
        ::neverd::limits::gui::kPackageSessionMaxFindingRows,
        ::neverd::limits::gui::kPackageSessionMaxMetadataBytes,
        [this, Query](auto Rows) {
          if (Rows.size() != Assessment["finding_count"].toInteger(-1)) {
            failure(QStringLiteral("invalid_web_page_count"));
            return;
          }
          QSet<QString> IDs;
          for (const auto &Row : Rows) {
            const auto ID = Row.toObject()["finding_id"].toString();
            if (ID.size() != 64 || IDs.contains(ID)) {
              failure(QStringLiteral("invalid_web_finding_identity"));
              return;
            }
            IDs.insert(ID);
          }
          pages(QStringLiteral("web_package_review_decisions"), Query,
                MaxJournalDecisions, MaxJournalBytes,
                [this, Rows = std::move(Rows)](auto History) {
                  if (History.size() !=
                          Assessment["decision_count"].toInteger(-1) ||
                      !ReviewJournal::valid(
                          ReviewJournal::create(Assessment, History))) {
                    failure(QStringLiteral("invalid_web_review_chain"));
                    return;
                  }
                  Findings = Rows;
                  Decisions = std::move(History);
                  emit assessmentReady();
                });
        });
}

void PackageSession::review(const QString &FindingID,
                            const QString &Disposition) {
  if (!Ready || Busy || Assessment.isEmpty())
    return;
  request(QStringLiteral("web_package_review_append"),
          {{"package_assessment_id", Assessment["package_assessment_id"]},
           {"expected_review_head", Assessment["review_head_id"]},
           {"finding_id", FindingID},
           {"disposition", Disposition}},
          [this](const auto &Result) {
            Assessment = Result;
            refreshAssessment();
          });
}

void PackageSession::restore(const QJsonObject &Journal) {
  if (!Ready || Busy || !RestoreAvailable || Assessment.isEmpty())
    return;
  if (!ReviewJournal::valid(Journal) ||
      Journal["package_assessment_id"] != Assessment["package_assessment_id"] ||
      Journal["review_root_id"] != Assessment["review_root_id"]) {
    failure(QStringLiteral("review_journal_owner_mismatch"));
    return;
  }
  request(QStringLiteral("web_package_review_restore"),
          {{"package_assessment_id", Assessment["package_assessment_id"]},
           {"expected_review_head", Assessment["review_head_id"]},
           {"journal_json", QString::fromUtf8(QJsonDocument(Journal).toJson(
                                QJsonDocument::Compact))}},
          [this](const auto &Result) {
            Assessment = Result;
            refreshAssessment();
          });
}

void PackageSession::pages(const QString &Operation, QJsonObject Query,
                           qsizetype Maximum, qsizetype ByteLimit,
                           std::function<void(QJsonArray)> Done) {
  page(Operation, std::move(Query), 0, {}, 0, Maximum, ByteLimit,
       std::move(Done));
}

void PackageSession::page(const QString &Operation, QJsonObject Query,
                          qsizetype Offset, QJsonArray Rows, qsizetype Bytes,
                          qsizetype Maximum, qsizetype ByteLimit,
                          std::function<void(QJsonArray)> Done) {
  auto Payload = Query;
  Payload["offset"] = Offset;
  Payload["limit"] = ::neverd::limits::gui::kPackageSessionPageSize;
  request(Operation, Payload,
          [this, Operation, Query, Offset, Rows = std::move(Rows), Bytes,
           Maximum, ByteLimit,
           Done = std::move(Done)](const auto &Result) mutable {
            const auto Items = Result["items"].toArray();
            const auto Charge =
                QJsonDocument(Items).toJson(QJsonDocument::Compact).size();
            if (!Result["items"].isArray() ||
                Result["offset"].toInteger(-1) != Offset ||
                Result["revision"].toString() != Revision ||
                Items.size() > ::neverd::limits::gui::kPackageSessionPageSize ||
                Items.size() > Maximum - Rows.size() ||
                Charge > ByteLimit - Bytes ||
                (Query.contains("review_head") &&
                 (Result["review_head_id"] != Query["review_head"] ||
                  Result["package_assessment_id"] !=
                      Query["package_assessment_id"]))) {
              failure(QStringLiteral("invalid_web_page"));
              return;
            }
            for (const auto &Item : Items)
              Rows.append(Item);
            bool Complete = Result["page_complete"] == QJsonValue(true);
            if (Operation == u"web_package_archive_records") {
              const auto Total = Result["total"].toInteger(-1);
              if (Result["archive_id"] != Query["archive_id"] || Total < 0 ||
                  Total > Maximum || Offset + Items.size() > Total) {
                failure(QStringLiteral("invalid_web_archive_page"));
                return;
              }
              Complete = Offset + Items.size() == Total;
            } else if (Result["page_complete"] != QJsonValue(true) &&
                       Result["page_complete"] != QJsonValue(false)) {
              failure(QStringLiteral("invalid_web_page"));
              return;
            }
            if (Complete) {
              if (!Result["next_offset"].isNull()) {
                failure(QStringLiteral("invalid_web_page"));
                return;
              }
              Done(std::move(Rows));
              return;
            }
            if (Items.isEmpty() ||
                Result["next_offset"].toInteger(-1) != Offset + Items.size()) {
              failure(QStringLiteral("invalid_web_page"));
              return;
            }
            page(Operation, Query, Offset + Items.size(), std::move(Rows),
                 Bytes + Charge, Maximum, ByteLimit, std::move(Done));
          });
}

void PackageSession::cancel() {
  Client.stop();
  Ready = false;
  failure(QStringLiteral("web_worker_stopped"));
}
} // namespace neverd::gui::web
