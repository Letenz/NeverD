//===- ReviewJournal.h - Local package review persistence -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded metadata journals. Cryptographic review validation belongs to the
/// backend; reading a journal never applies its assertions.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "../ResourceLimits.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <optional>

namespace neverd::gui::web {

inline constexpr auto JournalProfile = "offline-package-review-journal-v1";

class ReviewJournal {
public:
  static QJsonObject create(const QJsonObject &Assessment,
                            const QJsonArray &Decisions);
  /// Validate the closed metadata schema, not the backend's identity recipe.
  static bool valid(const QJsonObject &Journal);
  static std::optional<QJsonObject> load(const QString &Path, QString &Error);
  /// Atomically replace only the explicitly selected local output file.
  static bool save(const QString &Path, const QJsonObject &Journal,
                   QString &Error);
};

} // namespace neverd::gui::web

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::gui::web {
inline constexpr qsizetype MaxJournalBytes =
    ::neverd::limits::gui::kReviewJournalMaxJournalBytes;
inline constexpr qsizetype MaxJournalDecisions =
    ::neverd::limits::gui::kReviewJournalMaxJournalDecisions;
} // namespace neverd::gui::web
