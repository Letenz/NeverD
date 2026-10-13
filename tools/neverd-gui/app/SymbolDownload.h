//===- SymbolDownload.h - Download a PDB from symbol servers ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The transport of a PDB from the symbol servers the engine names into the
/// store path it names for each.  The file arrives under a temporary name
/// beside its destination and takes the destination's name only once it is
/// complete; whether it is the PDB the input names is the engine's to say.
//===----------------------------------------------------------------------===//

#pragma once

#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryDir;
class QTemporaryFile;

namespace neverd::gui {

class SymbolDownload final : public QObject {
  Q_OBJECT
public:
  /// Downloads with \p network, or with a network of its own when null.
  explicit SymbolDownload(QNetworkAccessManager *network = nullptr,
                          QObject *parent = nullptr);
  ~SymbolDownload() override;

  /// Ask \p servers, the "servers" of the worker's pdb_reference, in order
  /// for the PDB named \p file, until one serves it.
  void start(const QJsonArray &servers, const QString &file);
  /// Stop, leaving no file behind; no signal follows.
  void cancel();
  bool running() const { return running_; }
  /// The host the current request goes to.
  QString host() const { return url_.host(); }

signals:
  /// \p total is -1 while the size is unknown.
  void progress(qint64 received, qint64 total);
  /// The file is complete at \p path, the server's store path.
  void downloaded(const QString &path);
  /// No server served the file; \p notFound when each one said it has none.
  void failed(const QString &message, bool notFound);

private:
  void next();
  void request(bool compressed);
  void consume();
  void finish();
  void expand();
  void complete(const QString &path);
  /// The current server served nothing: ask the next.
  void skip(const QString &message);
  void fail(const QString &message);

  QNetworkAccessManager *network_;
  QPointer<QNetworkReply> reply_;
  QPointer<QProcess> expand_;
  QTimer deadline_;
  /// Waits before the current server is asked again.
  QTimer retry_;
  std::shared_ptr<QTemporaryFile> file_;
  std::shared_ptr<QTemporaryDir> expanded_;
  QJsonArray servers_;
  int server_ = -1;
  QString fileName_;
  QString destination_;
  QUrl url_;
  /// The last server's answer, and whether every server said it has none.
  QString error_;
  bool notFound_ = true;
  bool compressed_ = false;
  bool running_ = false;
  qint64 received_ = 0;
  /// How often the current server was asked again.
  int retries_ = 0;
};

} // namespace neverd::gui
