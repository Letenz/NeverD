//===- FunctionEditDialog.h - Edit a function ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IDA's "Edit function" dialog (Alt+P): rename the function and state what
/// it is: whether it returns, and the notes IDA keeps of a function.
//===----------------------------------------------------------------------===//
#pragma once

#include <QColor>
#include <QDialog>
#include <QJsonObject>
#include <optional>

class QCheckBox;
class QLineEdit;
class QPushButton;

namespace neverd::gui {

/// IDA's Edit function dialog: the function's name, its bounds and frame
/// sizes as analysis found them, and what the user states it is.  The fields
/// NeverD does not model show as IDA lays them out, disabled.
class FunctionEditDialog final : public QDialog {
  Q_OBJECT
public:
  /// \p attributes is the worker's function_attributes reply; \p start and
  /// \p end are its bounds as the workbench shows addresses.
  FunctionEditDialog(const QJsonObject &attributes, const QString &start,
                     const QString &end, QWidget *parent = nullptr);

  /// The name the user gave the function, trimmed.
  QString name() const;
  /// The attributes the user changed, as function_edit takes them: each key
  /// of FunctionAttributes.def true or false, the color "#RRGGBB", or null
  /// for the default color.
  QJsonObject changedAttributes() const;

private:
  void showColor();

  QJsonObject attributes_;
  QLineEdit *name_;
  QPushButton *color_;
  QCheckBox *noReturn_, *library_, *static_;
  /// The color the user picked; none for the default.
  std::optional<QColor> chosen_;
  bool colorChanged_ = false;
};

} // namespace neverd::gui
