//===- SymbolTablePromptDialog.h - Load an image's symbols ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// "Symbol table found": asked before an image with a static symbol table
/// loads, since anyone can write names there to mislead the reader.
//===----------------------------------------------------------------------===//

#pragma once

#include "LoadQuestionDialog.h"

#include <QJsonObject>

namespace neverd::gui {

class SymbolTablePromptDialog final : public LoadQuestionDialog {
  Q_OBJECT
public:
  /// \p reference is the "symbols" of the worker's symbol_table_reference
  /// for the input: the table and the first names it holds.  Yes accepts
  /// the dialog; No, Escape and closing it reject it, which loads the input
  /// as if the table had been stripped.
  explicit SymbolTablePromptDialog(const QJsonObject &reference,
                                   QWidget *parent = nullptr);

private:
  static Text textFor(const QJsonObject &reference);
};

} // namespace neverd::gui
