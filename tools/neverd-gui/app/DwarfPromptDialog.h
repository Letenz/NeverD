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

#include <QDialog>
#include <QJsonObject>

class QLabel;

namespace neverd::gui {

class DwarfPromptDialog final : public QDialog {
  Q_OBJECT
public:
  /// \p reference is the "dwarf" of the worker's dwarf_reference for the
  /// input.  Yes accepts the dialog; No, Escape and closing it reject it,
  /// which loads the input without its DWARF.
  explicit DwarfPromptDialog(const QJsonObject &reference,
                             QWidget *parent = nullptr);

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  /// Show the path line's middle elided to the room it has.
  void elidePath();

  /// The file that holds the DWARF when it is not the input, else null.
  QLabel *path_ = nullptr;
};

} // namespace neverd::gui
