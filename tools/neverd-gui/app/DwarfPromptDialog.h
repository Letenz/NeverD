//===- DwarfPromptDialog.h - Load the DWARF of an image ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IDA's "DWARF info found" question, asked before an ELF or Mach-O image
/// loads with the DWARF that debug discovery finds for it.
//===----------------------------------------------------------------------===//

#pragma once

#include "LoadQuestionDialog.h"

#include <QJsonObject>

namespace neverd::gui {

class DwarfPromptDialog final : public LoadQuestionDialog {
  Q_OBJECT
public:
  /// \p reference is the "dwarf" of the worker's dwarf_reference for the
  /// input.  Yes accepts the dialog; No, Escape and closing it reject it,
  /// which loads the input without its DWARF.
  explicit DwarfPromptDialog(const QJsonObject &reference,
                             QWidget *parent = nullptr);

private:
  static Text textFor(const QJsonObject &reference);
};

} // namespace neverd::gui
