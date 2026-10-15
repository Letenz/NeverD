//===- DwarfPromptDialog.cpp - Load the DWARF of an image -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IDA asks "Load DWARF debug information?" under the title "DWARF info
/// found", and names the file in parentheses when the DWARF is not the
/// input's own.  IDA's import options are left out: the engine reads DWARF
/// whole or not at all.
//===----------------------------------------------------------------------===//

#include "DwarfPromptDialog.h"

#include <QDir>

namespace neverd::gui {
namespace {

/// The location of DWARF read from the input's own sections.
constexpr char InputLocation[] = "input";

} // namespace

DwarfPromptDialog::DwarfPromptDialog(const QJsonObject &reference,
                                     QWidget *parent)
    : LoadQuestionDialog(QStringLiteral("dwarfPrompt"), textFor(reference),
                         parent) {}

LoadQuestionDialog::Text
DwarfPromptDialog::textFor(const QJsonObject &reference) {
  Text text;
  text.title = tr("DWARF info found");
  text.question = tr("Load DWARF debug information?");
  if (reference.value("location").toString() != QLatin1String(InputLocation)) {
    // A .dSYM bundle or a .debug file holds it: say which, its middle elided
    // to fit and the whole of it in the tooltip.
    const QString shown =
        QDir::toNativeSeparators(reference.value("path").toString());
    text.detail = tr("(%1)").arg(shown);
    text.detailName = QStringLiteral("path");
    text.elide = Qt::ElideMiddle;
    text.detailToolTip = shown;
  }
  return text;
}

} // namespace neverd::gui
