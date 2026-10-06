#pragma once

#include "Address.h"

#include <QWidget>
#include <functional>
#include <optional>

class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace neverd::gui {

class Session;

/// Declarative extension manifests: their commands, run against the current
/// address, and the last command's result.
class ExtensionsView final : public QWidget {
  Q_OBJECT
public:
  explicit ExtensionsView(Session &session, QWidget *parent = nullptr);
  void setLocationProvider(std::function<std::optional<Address>()> provider) {
    location_ = std::move(provider);
  }
  void importManifest();

private:
  void refresh();
  void updateButtons();
  void run();
  Session &session_;
  QListWidget *list_;
  QPlainTextEdit *result_;
  QPushButton *import_, *run_, *unload_;
  std::function<std::optional<Address>()> location_;
};

} // namespace neverd::gui
