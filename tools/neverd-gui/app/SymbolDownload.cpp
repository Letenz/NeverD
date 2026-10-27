//===- SymbolDownload.cpp - Download a PDB from symbol servers ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One HTTPS transfer at a time, bounded in size and time, into a temporary
/// file beside the store path; Windows also tries a server's compressed .pd_.
//===----------------------------------------------------------------------===//

#include "SymbolDownload.h"

#include "../ResourceLimits.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtEndian>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace neverd::gui {
namespace {
constexpr int HttpOk = 200;
constexpr int HttpNotFound = 404;
/// The first status a server sends for its own failure.
constexpr int HttpServerError = 500;
/// Whether a transfer that ended with \p error and \p status broke on the
/// network or in the server, which the next try may not meet, rather than
/// with an answer that will not change.
bool transient(QNetworkReply::NetworkError error, int status) {
  if (status >= HttpServerError)
    return true;
  switch (error) {
  case QNetworkReply::ConnectionRefusedError:
  case QNetworkReply::RemoteHostClosedError:
  case QNetworkReply::HostNotFoundError:
  case QNetworkReply::TimeoutError:
  // The transfer timeout stops a stalled transfer this way.
  case QNetworkReply::OperationCanceledError:
  case QNetworkReply::TemporaryNetworkFailureError:
  case QNetworkReply::NetworkSessionFailedError:
  case QNetworkReply::UnknownNetworkError:
  case QNetworkReply::ProxyConnectionClosedError:
  case QNetworkReply::ProxyTimeoutError:
  case QNetworkReply::InternalServerError:
  case QNetworkReply::ServiceUnavailableError:
  case QNetworkReply::UnknownServerError:
    return true;
  default:
    return false;
  }
}
} // namespace

SymbolDownload::SymbolDownload(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent),
      network_(network ? network : new QNetworkAccessManager(this)) {
  deadline_.setSingleShot(true);
  connect(&deadline_, &QTimer::timeout, this,
          [this] { fail(tr("The symbol download timed out.")); });
  retry_.setSingleShot(true);
  connect(&retry_, &QTimer::timeout, this, [this] { request(compressed_); });
}

SymbolDownload::~SymbolDownload() { cancel(); }

void SymbolDownload::start(const QJsonArray &servers, const QString &file) {
  cancel();
  servers_ = servers;
  fileName_ = file;
  server_ = -1;
  error_.clear();
  notFound_ = true;
  running_ = true;
  deadline_.start(::neverd::limits::gui::kSymbolDownloadDownloadTimeoutMs);
  next();
}

void SymbolDownload::cancel() {
  running_ = false;
  deadline_.stop();
  retry_.stop();
  if (reply_) {
    auto *reply = reply_.data();
    reply_ = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  if (expand_) {
    disconnect(expand_, nullptr, this, nullptr);
    // Keep the cabinet and output directory alive until the child has
    // stopped, without blocking the GUI during cancellation.
    const std::shared_ptr<QTemporaryFile> file(std::move(file_));
    const std::shared_ptr<QTemporaryDir> directory(std::move(expanded_));
    connect(expand_, &QObject::destroyed, [file, directory] {});
    expand_->kill();
    expand_->deleteLater();
    expand_ = nullptr;
  }
  file_.reset();
  expanded_.reset();
}

void SymbolDownload::next() {
  if (!running_)
    return;
  if (++server_ >= servers_.size()) {
    fail(error_.isEmpty() ? tr("No symbol server is configured.") : error_);
    return;
  }
  const auto server = servers_.at(server_).toObject();
  url_ = QUrl(server.value("url").toString());
  destination_ = server.value("path").toString();
  retries_ = 0;
  if (!url_.isValid() || destination_.isEmpty() ||
      (url_.scheme() != QLatin1String("https") &&
       url_.scheme() != QLatin1String("http"))) {
    notFound_ = false;
    skip(tr("%1 is not an HTTP symbol server.").arg(url_.toDisplayString()));
    return;
  }
  request(false);
}

void SymbolDownload::request(bool compressed) {
  compressed_ = compressed;
  const auto directory = QFileInfo(destination_).absolutePath();
  if (!QDir().mkpath(directory)) {
    fail(tr("Cannot create the symbol store directory %1.").arg(directory));
    return;
  }
  file_ = std::make_shared<QTemporaryFile>(directory + "/download-XXXXXX");
  if (!file_->open()) {
    fail(tr("Cannot open a temporary symbol file in %1: %2")
             .arg(directory, file_->errorString()));
    return;
  }
  received_ = 0;
  QUrl url = url_;
  if (compressed)
    url.setPath(url.path().chopped(1) + '_');
  QNetworkRequest request(url);
  request.setRawHeader("User-Agent", "NeverD-Symbols/1.0");
  request.setRawHeader("Accept-Encoding", "identity");
  request.setTransferTimeout(
      ::neverd::limits::gui::kSymbolDownloadIdleTimeoutMs);
  request.setMaximumRedirectsAllowed(
      ::neverd::limits::gui::kSymbolDownloadMaxRedirects);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  reply_ = network_->get(request);
  reply_->setReadBufferSize(
      ::neverd::limits::gui::kSymbolDownloadReadBufferBytes);
  connect(reply_, &QNetworkReply::readyRead, this, &SymbolDownload::consume);
  connect(reply_, &QNetworkReply::downloadProgress, this,
          [this](qint64 received, qint64 total) {
            if (received > ::neverd::limits::gui::kSymbolDownloadMaxBytes ||
                total > ::neverd::limits::gui::kSymbolDownloadMaxBytes) {
              fail(tr("The symbol file exceeds the 2 GiB download limit."));
              return;
            }
            emit progress(received, total > 0 ? total : -1);
          });
  connect(reply_, &QNetworkReply::finished, this, &SymbolDownload::finish);
}

void SymbolDownload::consume() {
  if (!reply_ || !file_)
    return;
  if (reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
      HttpOk) {
    reply_->readAll();
    return;
  }
  while (reply_->bytesAvailable()) {
    const auto data =
        reply_->read(::neverd::limits::gui::kSymbolDownloadReadChunkBytes);
    if (data.isEmpty())
      break;
    received_ += data.size();
    if (received_ > ::neverd::limits::gui::kSymbolDownloadMaxBytes) {
      fail(tr("The symbol file exceeds the 2 GiB download limit."));
      return;
    }
    if (file_->write(data) != data.size()) {
      fail(tr("Cannot write the symbol file %1: %2")
               .arg(file_->fileName(), file_->errorString()));
      return;
    }
  }
}

void SymbolDownload::finish() {
  if (!reply_)
    return;
  consume();
  if (!reply_)
    return;
  const int status =
      reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const auto error = reply_->error();
  const auto errorText = reply_->errorString();
  reply_->deleteLater();
  reply_ = nullptr;
  if (status == HttpNotFound) {
#ifdef Q_OS_WIN
    if (!compressed_) {
      request(true);
      return;
    }
#endif
    skip(tr("%1 has no matching symbols for this build.").arg(url_.host()));
    return;
  }
  notFound_ = false;
  if (error != QNetworkReply::NoError && transient(error, status) &&
      retries_ < ::neverd::limits::gui::kSymbolDownloadTransferRetries) {
    file_.reset();
    retry_.start(::neverd::limits::gui::kSymbolDownloadRetryPauseMs *
                 ++retries_);
    return;
  }
  if (error != QNetworkReply::NoError || status != HttpOk) {
    skip(tr("The symbol download from %1 failed: %2")
             .arg(url_.host(), errorText));
    return;
  }
  if (!received_) {
    skip(tr("%1 sent an empty symbol file.").arg(url_.host()));
    return;
  }
  if (!file_->flush()) {
    fail(tr("Cannot write the symbol file %1: %2")
             .arg(file_->fileName(), file_->errorString()));
    return;
  }
  file_->close();
  if (compressed_)
    expand();
  else
    complete(file_->fileName());
}

void SymbolDownload::expand() {
#ifdef Q_OS_WIN
  QFile cabinet(file_->fileName());
  if (!cabinet.open(QIODevice::ReadOnly)) {
    skip(tr("The compressed symbol file is invalid."));
    return;
  }
  const auto header = cabinet.read(36);
  const auto filesOffset =
      header.size() == 36 ? qFromLittleEndian<quint32>(header.constData() + 16)
                          : 0;
  if (!header.startsWith("MSCF") || header.size() != 36 || filesOffset < 36 ||
      filesOffset > cabinet.size() - 16 ||
      qFromLittleEndian<quint16>(header.constData() + 28) != 1 ||
      !cabinet.seek(filesOffset)) {
    skip(tr("The compressed symbol file is invalid."));
    return;
  }
  const auto entry = cabinet.read(16 + 241);
  const int terminator = entry.indexOf('\0', 16);
  if (entry.size() < 17 || terminator < 0 ||
      qFromLittleEndian<quint32>(entry.constData()) >
          ::neverd::limits::gui::kSymbolDownloadMaxBytes ||
      QString::fromLatin1(entry.mid(16, terminator - 16))
          .compare(fileName_, Qt::CaseInsensitive)) {
    skip(tr("The compressed symbol file is invalid."));
    return;
  }
  const auto directory = QFileInfo(destination_).absolutePath();
  expanded_ = std::make_shared<QTemporaryDir>(directory + "/expanded-XXXXXX");
  if (!expanded_->isValid()) {
    fail(tr("Cannot create a temporary symbol directory in %1: %2")
             .arg(directory, expanded_->errorString()));
    return;
  }
  auto *process = new QProcess(this);
  expand_ = process;
  process->setCreateProcessArgumentsModifier(
      [](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
      });
  connect(process, &QProcess::errorOccurred, this,
          [this] { skip(tr("Could not expand the compressed symbol file.")); });
  connect(process, &QProcess::finished, this,
          [this, process](int code, QProcess::ExitStatus status) {
            expand_ = nullptr;
            process->deleteLater();
            const auto path = expanded_->filePath(fileName_);
            const QFileInfo info(path);
            if (code || status != QProcess::NormalExit || !info.isFile() ||
                info.size() <= 0 ||
                info.size() > ::neverd::limits::gui::kSymbolDownloadMaxBytes) {
              skip(tr("Could not expand the compressed symbol file."));
              return;
            }
            complete(path);
          });
  process->start(QDir(qEnvironmentVariable("SystemRoot", "C:/Windows"))
                     .filePath(QStringLiteral("System32/expand.exe")),
                 {file_->fileName(), "-F:" + fileName_, expanded_->path()});
#else
  skip(tr("Compressed symbols require Windows."));
#endif
}

void SymbolDownload::complete(const QString &path) {
  // QTemporaryFile retains its native handle after close(). Rename through
  // its owner so Windows can release that handle first.
  const bool temporary = file_ && path == file_->fileName();
  QFile expandedFile(path);
  const bool saved = temporary ? file_->rename(destination_)
                               : expandedFile.rename(destination_);
  if (!saved && !QFileInfo(destination_).isFile()) {
    fail(tr("Cannot save the symbol file to %1: %2")
             .arg(destination_, temporary ? file_->errorString()
                                          : expandedFile.errorString()));
    return;
  }
  // Another download may have stored the same file first; the engine judges
  // whichever copy is there.  A store may serve other users, as a symbol
  // server's downstream store does.
  if (saved) {
    if (temporary)
      file_->setAutoRemove(false);
    QFile::setPermissions(destination_,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ReadGroup | QFileDevice::ReadOther);
  }
  const auto destination = destination_;
  running_ = false;
  deadline_.stop();
  file_.reset();
  expanded_.reset();
  emit downloaded(destination);
}

void SymbolDownload::skip(const QString &message) {
  error_ = message;
  if (reply_) {
    disconnect(reply_, nullptr, this, nullptr);
    reply_->abort();
    reply_->deleteLater();
    reply_ = nullptr;
  }
  file_.reset();
  expanded_.reset();
  next();
}

void SymbolDownload::fail(const QString &message) {
  const bool notFound = notFound_;
  cancel();
  emit failed(message, notFound);
}

} // namespace neverd::gui
