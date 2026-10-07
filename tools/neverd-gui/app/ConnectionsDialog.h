#pragma once

#include <QDialog>
#include <QVariantMap>

class GuiSessionBroker;
class McpConnectionManager;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabBar;

namespace neverd::gui {

class Session;

/// MCP connections: share this session with local agents through the
/// authenticated broker, and connect to MCP servers to browse and call their
/// tools and resources.
class ConnectionsDialog final : public QDialog {
  Q_OBJECT
public:
  ConnectionsDialog(Session &session, McpConnectionManager &client,
                    GuiSessionBroker &broker, QWidget *parent = nullptr);

private:
  void refresh();
  void refreshCatalog();
  void connectOrDisconnect();
  void selectEntry();
  void showSchema();
  static QString callStatus(const QString &status);

  Session &session_;
  McpConnectionManager &client_;
  GuiSessionBroker &broker_;
  QLabel *sharingStatus_;
  QPushButton *sharing_, *copyCredential_;
  QComboBox *transport_;
  QLineEdit *endpoint_, *arguments_, *token_, *caFile_;
  QPushButton *connect_;
  QLabel *clientStatus_;
  QTabBar *tabs_;
  QListWidget *catalog_;
  QLabel *selectedTitle_, *selectedDescription_;
  QPlainTextEdit *parameters_, *result_;
  QPushButton *schema_, *call_, *cancelCall_;
  QVariantMap selectedTool_;
  QString selectedCall_, validationError_;
};

} // namespace neverd::gui
