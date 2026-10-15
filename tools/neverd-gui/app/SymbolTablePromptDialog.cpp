//===- SymbolTablePromptDialog.cpp - Load an image's symbols --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The question shows the table and the first names it holds, so planted
/// names can be told before they load; the tooltip lists them one a line.
//===----------------------------------------------------------------------===//

#include "SymbolTablePromptDialog.h"

#include <QJsonArray>
#include <QStringList>

namespace neverd::gui {

SymbolTablePromptDialog::SymbolTablePromptDialog(const QJsonObject &reference,
                                                 QWidget *parent)
    : LoadQuestionDialog(QStringLiteral("symbolTablePrompt"),
                         textFor(reference), parent) {}

LoadQuestionDialog::Text
SymbolTablePromptDialog::textFor(const QJsonObject &reference) {
  Text text;
  text.title = tr("Symbol table found");
  text.question = tr("Load the symbol table?");
  QStringList names;
  for (const auto &name : reference.value("names").toArray())
    names << name.toString();
  const QString table = reference.value("table").toString();
  // A table of data names alone still says which table it is.
  text.detail =
      names.isEmpty()
          ? tr("(%1)").arg(table)
          : tr("(%1: %2)").arg(table, names.join(QStringLiteral(", ")));
  text.detailName = QStringLiteral("names");
  text.elide = Qt::ElideRight;
  if (!names.isEmpty())
    text.detailToolTip = names.join(u'\n');
  return text;
}

} // namespace neverd::gui
