//===- MapPromptDialog.cpp - Load the linker MAP of an image --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IDA has no such question, as it reads no MAP.  The question names the
/// file, the linker that wrote it and how many names it gives, and says
/// whether it belongs to the input's build: a link.exe MAP records the build
/// stamp, and the other linkers' MAPs place the sections the input has.
//===----------------------------------------------------------------------===//

#include "MapPromptDialog.h"

#include <QDir>
#include <QJsonArray>
#include <QStringList>

namespace neverd::gui {
namespace {

/// The match of a MAP that describes another build.
constexpr char Mismatch[] = "mismatch";

/// The linker \p dialect names, as the user knows it (MapDialects.def).
QString linkerName(const QString &dialect) {
#define NEVERD_MAP_DIALECT(Name, Spelling, Linker)                             \
  if (dialect == QLatin1String(Spelling))                                      \
    return QStringLiteral(Linker);
#include "neverd/debug/MapDialects.def"
  return dialect;
}

} // namespace

MapPromptDialog::MapPromptDialog(const QJsonObject &reference, QWidget *parent)
    : LoadQuestionDialog(QStringLiteral("mapPrompt"), textFor(reference),
                         parent) {}

bool MapPromptDialog::mismatched(const QJsonObject &facts) {
  return facts.value("match").toString() == QLatin1String(Mismatch);
}

QString MapPromptDialog::description(const QJsonObject &facts) {
  const QString linker = linkerName(facts.value("dialect").toString());
  const qint64 names =
      facts.value("functions").toInteger() + facts.value("data").toInteger();
  const QString match = facts.value("match").toString();
  if (match == QLatin1String("verified"))
    return tr("%1 MAP, %2 names. Its build stamp matches the input file.")
        .arg(linker)
        .arg(names);
  if (match == QLatin1String("consistent"))
    return tr("%1 MAP, %2 names. Its sections are where the input file has "
              "them.")
        .arg(linker)
        .arg(names);
  if (match != QLatin1String(Mismatch))
    return tr("%1 MAP, %2 names. It records nothing to check against the "
              "input file.")
        .arg(linker)
        .arg(names);
  const QString reason = facts.value("reason").toString();
  if (reason == QLatin1String("stamp")) {
    const auto stamp = facts.value("stamp").toObject();
    return tr("%1 MAP, %2 names. It records the build stamp %3, but the "
              "input file's is %4: it belongs to another build.")
        .arg(linker)
        .arg(names)
        .arg(stamp.value("map").toString(), stamp.value("image").toString());
  }
  if (reason == QLatin1String("section")) {
    const auto section = facts.value("section").toObject();
    const QString segment = section.value("segment").toString();
    const QString name =
        segment.isEmpty()
            ? section.value("name").toString()
            : segment + QLatin1Char(',') + section.value("name").toString();
    const auto placed = section.value("map").toObject();
    const auto actual = section.value("image").toObject();
    return tr("%1 MAP, %2 names. It places %3 at %4, %5 bytes, but the input "
              "file has it at %6, %7 bytes: it belongs to another build.")
        .arg(linker)
        .arg(names)
        .arg(name, placed.value("address").toString(),
             placed.value("size").toString(),
             actual.value("address").toString(),
             actual.value("size").toString());
  }
  return tr("%1 MAP, %2 names. %3 of its %4 functions lie outside the input "
            "file's code: it belongs to another build.")
      .arg(linker)
      .arg(names)
      .arg(facts.value("outside").toInteger())
      .arg(facts.value("functions").toInteger());
}

LoadQuestionDialog::Text
MapPromptDialog::textFor(const QJsonObject &reference) {
  Text text;
  text.title = tr("MAP file found");
  const bool other = mismatched(reference);
  text.question =
      other ? tr("Load the MAP file anyway?") : tr("Load the MAP file?");
  // The file, its middle elided to fit and the whole of it in the tooltip.
  const QString shown =
      QDir::toNativeSeparators(reference.value("path").toString());
  text.detail = tr("(%1)").arg(shown);
  text.detailName = QStringLiteral("path");
  text.elide = Qt::ElideMiddle;
  text.detailToolTip = shown;
  text.note = description(reference);
  text.noteName = QStringLiteral("description");
  // Its first names, which tell planted ones, one a line.
  QStringList names;
  for (const auto &name : reference.value("names").toArray())
    names << name.toString();
  text.noteToolTip = names.join(u'\n');
  text.cautious = other;
  return text;
}

} // namespace neverd::gui
