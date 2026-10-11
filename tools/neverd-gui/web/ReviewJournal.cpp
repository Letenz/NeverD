//===- ReviewJournal.cpp - Bounded atomic review files -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Persist opaque evidence identities and fixed analyst dispositions without
/// source, paths, commands, notes or credentials.
///
//===----------------------------------------------------------------------===//

#include "ReviewJournal.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

namespace neverd::gui::web {
namespace {
bool identity(const QJsonValue &Value) {
  if (!Value.isString())
    return false;
  const auto Text = Value.toString();
  if (Text.size() != 64)
    return false;
  for (const auto C : Text)
    if (!((C >= u'0' && C <= u'9') || (C >= u'a' && C <= u'f')))
      return false;
  return true;
}
bool fields(const QJsonObject &O, std::initializer_list<const char *> Fields) {
  if (O.size() != qsizetype(Fields.size()))
    return false;
  for (const auto *Field : Fields)
    if (!O.contains(QLatin1StringView(Field)))
      return false;
  return true;
}
bool disposition(const QJsonValue &Value) {
  if (!Value.isString())
    return false;
  const auto Text = Value.toString();
  return Text == u"unreviewed" || Text == u"needs_review" ||
         Text == u"accepted_risk" || Text == u"false_positive";
}
} // namespace

QJsonObject ReviewJournal::create(const QJsonObject &Assessment,
                                  const QJsonArray &Decisions) {
  return {{"schema_version", 1},
          {"profile", JournalProfile},
          {"package_assessment_id", Assessment["package_assessment_id"]},
          {"review_root_id", Assessment["review_root_id"]},
          {"review_head_id", Assessment["review_head_id"]},
          {"decisions", Decisions}};
}

bool ReviewJournal::valid(const QJsonObject &Journal) {
  if (!fields(Journal, {"schema_version", "profile", "package_assessment_id",
                        "review_root_id", "review_head_id", "decisions"}) ||
      Journal["schema_version"] != QJsonValue(1) ||
      Journal["profile"] != QJsonValue(JournalProfile) ||
      !identity(Journal["package_assessment_id"]) ||
      !identity(Journal["review_root_id"]) ||
      !identity(Journal["review_head_id"]) || !Journal["decisions"].isArray())
    return false;
  const auto Decisions = Journal["decisions"].toArray();
  if (Decisions.size() > MaxJournalDecisions)
    return false;
  auto Head = Journal["review_root_id"];
  QSet<QString> IDs;
  for (qsizetype I = 0; I < Decisions.size(); ++I) {
    if (!Decisions[I].isObject())
      return false;
    const auto D = Decisions[I].toObject();
    if (!fields(D, {"decision_id", "package_assessment_id", "finding_id",
                    "parent_head_id", "disposition", "decision_index",
                    "assertion_source"}) ||
        !identity(D["decision_id"]) || !identity(D["finding_id"]) ||
        D["package_assessment_id"] != Journal["package_assessment_id"] ||
        D["parent_head_id"] != Head || D["decision_index"] != QJsonValue(I) ||
        D["assertion_source"] != QJsonValue("caller") ||
        !disposition(D["disposition"]) ||
        IDs.contains(D["decision_id"].toString()))
      return false;
    Head = D["decision_id"];
    IDs.insert(Head.toString());
  }
  return Head == Journal["review_head_id"] &&
         QJsonDocument(Journal).toJson(QJsonDocument::Compact).size() <=
             MaxJournalBytes;
}

std::optional<QJsonObject> ReviewJournal::load(const QString &Path,
                                               QString &Error) {
  QFile File(Path);
  if (!File.open(QIODevice::ReadOnly)) {
    Error = QStringLiteral("review_journal_read_failed");
    return std::nullopt;
  }
  const auto Bytes = File.read(MaxJournalBytes + 1);
  if (Bytes.size() > MaxJournalBytes || !File.atEnd()) {
    Error = QStringLiteral("review_journal_byte_budget_exceeded");
    return std::nullopt;
  }
  QJsonParseError ParseError;
  const auto Document = QJsonDocument::fromJson(Bytes, &ParseError);
  // Only the canonical form written here is admitted. Besides preventing
  // schema ambiguity, this rejects duplicate keys lost by Qt's JSON parser.
  if (ParseError.error != QJsonParseError::NoError || !Document.isObject() ||
      Document.toJson(QJsonDocument::Compact) != Bytes ||
      !valid(Document.object())) {
    Error = QStringLiteral("invalid_review_journal");
    return std::nullopt;
  }
  Error.clear();
  return Document.object();
}

bool ReviewJournal::save(const QString &Path, const QJsonObject &Journal,
                         QString &Error) {
  if (!valid(Journal)) {
    Error = QStringLiteral("invalid_review_journal");
    return false;
  }
  const auto Bytes = QJsonDocument(Journal).toJson(QJsonDocument::Compact);
  QSaveFile File(Path);
  File.setDirectWriteFallback(false);
  if (!File.open(QIODevice::WriteOnly) ||
      !File.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      File.write(Bytes) != Bytes.size() || !File.commit()) {
    Error = QStringLiteral("review_journal_write_failed");
    return false;
  }
  Error.clear();
  return true;
}
} // namespace neverd::gui::web
