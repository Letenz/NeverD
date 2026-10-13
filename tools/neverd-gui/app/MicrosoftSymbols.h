//===- MicrosoftSymbols.h - Optional Microsoft PDB acquisition -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Consent, asynchronous transport and caching for symbols whose identity the
/// engine publishes. Inspection and application stay in the worker/C API.
//===----------------------------------------------------------------------===//

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <memory>

class QMessageBox;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QProgressDialog;
class QTemporaryDir;
class QTemporaryFile;
class QWidget;

namespace neverd::gui {
class Session;

class MicrosoftSymbols final : public QObject {
  Q_OBJECT
public:
  MicrosoftSymbols(Session &session, QWidget *window,
                   QNetworkAccessManager *network = nullptr,
                   const QString &cacheRoot = {});
  ~MicrosoftSymbols() override;
  /// Also called before the user starts a manual PDB selection.
  void cancel();
  /// Validate transport path components; never accept a server URL from input.
  static bool validRequest(const QJsonObject &request);
  static QUrl downloadUrl(const QJsonObject &request, bool compressed = false);

private:
  void opened();
  bool current(quint64 serial) const;
  void ask(quint64 serial);
  void download(quint64 serial, bool compressed = false);
  void consume(quint64 serial);
  void expand(quint64 serial);
  void inspect(const QString &path, quint64 serial, bool cached);
  void apply(const QString &path, quint64 serial);
  void fail(const QString &message);

  Session &session_;
  QWidget *window_;
  QNetworkAccessManager *network_;
  QPointer<QNetworkReply> reply_;
  QPointer<QMessageBox> prompt_;
  QPointer<QProgressDialog> progress_;
  QPointer<QProcess> expand_;
  QTimer deadline_;
  std::shared_ptr<QTemporaryFile> file_;
  std::shared_ptr<QTemporaryDir> expanded_;
  QJsonObject request_;
  QString cachePath_;
  QString cacheRoot_;
  quint64 serial_ = 0, epoch_ = 0;
  qint64 received_ = 0;
};
} // namespace neverd::gui
