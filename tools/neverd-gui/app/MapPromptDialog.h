//===- MapPromptDialog.h - Load the linker MAP of an image ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// "MAP file found": the question asked before an image loads with the
/// linker MAP beside it, which happens only when no PDB or DWARF describes
/// the image, since their names outrank a MAP's.  It says which linker wrote
/// the MAP and whether the MAP is of the input's build.
//===----------------------------------------------------------------------===//

#pragma once

#include "LoadQuestionDialog.h"

#include <QJsonObject>

namespace neverd::gui {

class MapPromptDialog final : public LoadQuestionDialog {
  Q_OBJECT
public:
  /// \p reference is the "map" of the worker's map_reference for the input.
  /// Yes accepts the dialog; No, Escape and closing it reject it, which
  /// loads the input without the MAP.  No is the default for a MAP of
  /// another build.
  explicit MapPromptDialog(const QJsonObject &reference,
                           QWidget *parent = nullptr);

  /// What \p facts, a MAP as map_reference or map_inspect describes it, is
  /// and how it holds against the image: "link.exe map, 1234 names. Its
  /// build stamp matches the input file."
  static QString description(const QJsonObject &facts);
  /// Whether \p facts describe a MAP of another build.
  static bool mismatched(const QJsonObject &facts);

private:
  static Text textFor(const QJsonObject &reference);
};

} // namespace neverd::gui
