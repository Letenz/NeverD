//===- MicrosoftSymbols.cpp - Optional Microsoft PDB acquisition ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Acquire a missing PDB after consent. HTTP failures leave analysis intact;
/// no downloaded file is applied without the engine's strict identity check.
//===----------------------------------------------------------------------===//

#include "MicrosoftSymbols.h"

#include "Session.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtEndian>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace neverd::gui {
namespace {
constexpr qint64 MaxBytes = 2LL * 1024 * 1024 * 1024;
constexpr int IdleTimeoutMs = 30000;
constexpr int DownloadTimeoutMs = 10 * 60 * 1000;
} // namespace

MicrosoftSymbols::MicrosoftSymbols(Session &session, QWidget *window,
                                   QNetworkAccessManager *network,
                                   const QString &cacheRoot)
    : QObject(window), session_(session), window_(window),
      network_(network ? network : new QNetworkAccessManager(this)),
      cacheRoot_(cacheRoot.isEmpty() ? QStandardPaths::writableLocation(
                                           QStandardPaths::CacheLocation)
                                     : cacheRoot) {
  connect(&session_, &Session::opened, this, &MicrosoftSymbols::opened);
  connect(&session_, &Session::unloaded, this, &MicrosoftSymbols::cancel);
  connect(&session_, &Session::stateChanged, this, [this] {
    if (!request_.isEmpty() && !current(serial_))
      cancel();
  });
  deadline_.setSingleShot(true);
  connect(&deadline_, &QTimer::timeout, this,
          [this] { fail(tr("The symbol download timed out.")); });
}

MicrosoftSymbols::~MicrosoftSymbols() { cancel(); }

bool MicrosoftSymbols::validRequest(const QJsonObject &request) {
  const auto file = request.value("file").toString();
  const auto key = request.value("key").toString();
  if (file.isEmpty() || file.size() > 240 || file.startsWith('.') ||
      !file.endsWith(QLatin1String(".pdb"), Qt::CaseInsensitive))
    return false;
  for (const auto c : file)
    if (c.unicode() < 0x20 || c.unicode() >= 0x7f ||
        QStringLiteral("<>:\"/\\|?*").contains(c))
      return false;
  static const QRegularExpression Key(
      QStringLiteral("\\A(?:[0-9A-Fa-f]{9,16}|[0-9A-Fa-f]{33,40})\\z"));
  return Key.match(key).hasMatch();
}

QUrl MicrosoftSymbols::downloadUrl(const QJsonObject &request,
                                   bool compressed) {
  if (!validRequest(request))
    return {};
  const auto file = request.value("file").toString();
  QUrl url(QStringLiteral("https://msdl.microsoft.com"));
  url.setPath(QStringLiteral("/download/symbols/%1/%2/%3")
                  .arg(file, request.value("key").toString(),
                       compressed ? file.chopped(1) + '_' : file));
  return url;
}

bool MicrosoftSymbols::current(quint64 serial) const {
  return serial == serial_ && !request_.isEmpty() && session_.loaded() &&
         !session_.opening() && session_.epoch() == epoch_ &&
         session_.metadata().value("pdb_download").toObject() == request_ &&
         !session_.metadata().value("pdb").isObject();
}

void MicrosoftSymbols::cancel() {
  ++serial_;
  request_ = {};
  deadline_.stop();
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
  if (prompt_) {
    prompt_->close();
    prompt_ = nullptr;
  }
  if (progress_) {
    progress_->close();
    progress_->deleteLater();
    progress_ = nullptr;
  }
  file_.reset();
  expanded_.reset();
}

void MicrosoftSymbols::opened() {
  cancel();
  const auto request = session_.metadata().value("pdb_download").toObject();
  if (!session_.loadsPdbFiles() || !validRequest(request))
    return;
  request_ = request;
  epoch_ = session_.epoch();
  const auto &root = cacheRoot_;
  if (root.isEmpty()) {
    cancel();
    return;
  }
  cachePath_ = QDir(root).filePath(QStringLiteral("symbols/microsoft/%1/%2/%1")
                                       .arg(request.value("file").toString(),
                                            request.value("key").toString()));
  const auto serial = serial_;
  // The opened signal runs inside query delivery. Show dialogs only after
  // that barrier is released, and discard the callback on a new file.
  QTimer::singleShot(0, this, [this, serial] {
    if (!current(serial))
      return;
    if (QFileInfo(cachePath_).isFile())
      inspect(cachePath_, serial, true);
    else
      ask(serial);
  });
}

void MicrosoftSymbols::ask(quint64 serial) {
  if (!current(serial))
    return;
  if (QApplication::activeModalWidget()) {
    QTimer::singleShot(150, this, [this, serial] { ask(serial); });
    return;
  }
  auto *prompt = new QMessageBox(
      QMessageBox::Question, tr("Download debug symbols"),
      tr("No matching local PDB was loaded for %1.\n\n"
         "Try downloading %2 from the Microsoft Symbol Server? "
         "This only succeeds if Microsoft publishes symbols for this build.\n\n"
         "The PDB file name and build identifier will be sent; the binary "
         "is not uploaded. Matching symbols will be cached and loaded "
         "automatically.")
          .arg(session_.fileName(), request_.value("file").toString()),
      QMessageBox::NoButton, window_);
  prompt_ = prompt;
  prompt->setObjectName(QStringLiteral("downloadPdbPrompt"));
  prompt->setTextFormat(Qt::PlainText);
  prompt->setAttribute(Qt::WA_DeleteOnClose);
  auto *downloadButton =
      prompt->addButton(tr("Download"), QMessageBox::AcceptRole);
  auto *skipButton = prompt->addButton(tr("Not now"), QMessageBox::RejectRole);
  prompt->setDefaultButton(downloadButton);
  prompt->setEscapeButton(skipButton);
  connect(prompt, &QDialog::finished, this,
          [this, serial, prompt, downloadButton] {
            if (!current(serial))
              return;
            prompt_ = nullptr;
            if (prompt->clickedButton() == downloadButton) {
              deadline_.start(DownloadTimeoutMs);
              download(serial);
            } else {
              cancel();
            }
          });
  prompt->open();
}

void MicrosoftSymbols::download(quint64 serial, bool compressed) {
  if (!current(serial))
    return;
  const auto directory = QFileInfo(cachePath_).absolutePath();
  if (!QDir().mkpath(directory)) {
    fail(tr("Cannot create the symbol cache directory: %1").arg(directory));
    return;
  }
  file_ = std::make_shared<QTemporaryFile>(directory + "/download-XXXXXX");
  if (!file_->open()) {
    fail(tr("Cannot open a temporary symbol file in %1: %2")
             .arg(directory, file_->errorString()));
    return;
  }
  received_ = 0;
  if (!progress_) {
    progress_ = new QProgressDialog(window_);
    progress_->setObjectName(QStringLiteral("downloadPdbProgress"));
    progress_->setWindowTitle(tr("Download debug symbols"));
    progress_->setCancelButtonText(tr("Cancel"));
    progress_->setWindowModality(Qt::NonModal);
    progress_->setAutoClose(false);
    progress_->setAutoReset(false);
    connect(progress_, &QProgressDialog::canceled, this, [this, serial] {
      if (current(serial)) {
        cancel();
        emit session_.message(tr("Symbol download cancelled."), 0);
      }
    });
  }
  progress_->setLabelText(
      tr("Downloading %1…").arg(request_.value("file").toString()));
  progress_->setRange(0, 0);
  progress_->show();
  QNetworkRequest request(downloadUrl(request_, compressed));
  request.setRawHeader("User-Agent", "NeverD-Symbols/1.0");
  request.setRawHeader("Accept-Encoding", "identity");
  request.setTransferTimeout(IdleTimeoutMs);
  request.setMaximumRedirectsAllowed(5);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  reply_ = network_->get(request);
  reply_->setReadBufferSize(256 * 1024);
  connect(reply_, &QNetworkReply::readyRead, this,
          [this, serial] { consume(serial); });
  connect(reply_, &QNetworkReply::downloadProgress, this,
          [this, serial](qint64 received, qint64 total) {
            if (!current(serial))
              return;
            if (received > MaxBytes || total > MaxBytes) {
              fail(tr("The symbol file exceeds the 2 GiB download limit."));
              return;
            }
            if (progress_ && total > 0) {
              progress_->setRange(0, 100);
              progress_->setValue(int(100 * received / total));
            }
          });
  connect(reply_, &QNetworkReply::finished, this, [this, serial, compressed] {
    if (!current(serial) || !reply_)
      return;
    consume(serial);
    if (!current(serial) || !reply_)
      return;
    const int status =
        reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto error = reply_->error();
    const auto errorText = reply_->errorString();
    reply_->deleteLater();
    reply_ = nullptr;
    if (status == 404) {
#ifdef Q_OS_WIN
      if (!compressed) {
        download(serial, true);
        return;
      }
#endif
      fail(tr("Microsoft has no matching symbols for this build."));
      return;
    }
    if (error != QNetworkReply::NoError || status != 200) {
      fail(tr("Symbol download failed: %1").arg(errorText));
      return;
    }
    if (!received_) {
      fail(tr("The downloaded symbol file is empty."));
      return;
    }
    if (!file_->flush()) {
      fail(tr("Cannot write the symbol file %1: %2")
               .arg(file_->fileName(), file_->errorString()));
      return;
    }
    file_->close();
    if (compressed)
      expand(serial);
    else
      inspect(file_->fileName(), serial, false);
  });
}

void MicrosoftSymbols::consume(quint64 serial) {
  if (!current(serial) || !reply_ || !file_)
    return;
  if (reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
      200) {
    reply_->readAll();
    return;
  }
  while (reply_->bytesAvailable()) {
    const auto data = reply_->read(64 * 1024);
    if (data.isEmpty())
      break;
    received_ += data.size();
    if (received_ > MaxBytes) {
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

void MicrosoftSymbols::expand(quint64 serial) {
#ifdef Q_OS_WIN
  QFile cabinet(file_->fileName());
  if (!cabinet.open(QIODevice::ReadOnly)) {
    fail(tr("The compressed symbol file is invalid."));
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
    fail(tr("The compressed symbol file is invalid."));
    return;
  }
  const auto entry = cabinet.read(16 + 241);
  const int terminator = entry.indexOf('\0', 16);
  const auto file = request_.value("file").toString();
  if (entry.size() < 17 || terminator < 0 ||
      qFromLittleEndian<quint32>(entry.constData()) > MaxBytes ||
      QString::fromLatin1(entry.mid(16, terminator - 16))
          .compare(file, Qt::CaseInsensitive)) {
    fail(tr("The compressed symbol file is invalid."));
    return;
  }
  expanded_ = std::make_shared<QTemporaryDir>(
      QFileInfo(cachePath_).absolutePath() + "/expanded-XXXXXX");
  if (!expanded_->isValid()) {
    fail(tr("Cannot create a temporary symbol directory in %1: %2")
             .arg(QFileInfo(cachePath_).absolutePath(),
                  expanded_->errorString()));
    return;
  }
  progress_->setLabelText(tr("Expanding compressed symbols…"));
  progress_->setRange(0, 0);
  auto *process = new QProcess(this);
  expand_ = process;
  process->setCreateProcessArgumentsModifier(
      [](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
      });
  connect(process, &QProcess::errorOccurred, this,
          [this, serial](QProcess::ProcessError) {
            if (current(serial))
              fail(tr("Could not expand the compressed symbol file."));
          });
  connect(process, &QProcess::finished, this,
          [this, serial, file, process](int code, QProcess::ExitStatus status) {
            if (!current(serial))
              return;
            expand_ = nullptr;
            process->deleteLater();
            const auto path = expanded_->filePath(file);
            const QFileInfo info(path);
            if (code || status != QProcess::NormalExit || !info.isFile() ||
                info.size() <= 0 || info.size() > MaxBytes) {
              fail(tr("Could not expand the compressed symbol file."));
              return;
            }
            inspect(path, serial, false);
          });
  process->start(QDir(qEnvironmentVariable("SystemRoot", "C:/Windows"))
                     .filePath(QStringLiteral("System32/expand.exe")),
                 {file_->fileName(), "-F:" + file, expanded_->path()});
#else
  Q_UNUSED(serial);
  fail(tr("Compressed symbols require Windows."));
#endif
}

void MicrosoftSymbols::inspect(const QString &path, quint64 serial,
                               bool cached) {
  if (!current(serial))
    return;
  if (progress_) {
    progress_->setLabelText(tr("Verifying downloaded symbols…"));
    progress_->setRange(0, 0);
  }
  session_.read(
      "pdb_inspect", {{"path", path}}, this,
      [this, serial, path, cached, file = file_,
       expanded = expanded_](const QJsonObject &result) {
        Q_UNUSED(expanded);
        if (!current(serial))
          return;
        if (!result.value("verified").toBool()) {
          fail(tr("The PDB does not match the loaded file."));
          return;
        }
        if (!cached) {
          QFile expandedFile(path);
          // QTemporaryFile retains its native handle after close(). Rename
          // through its owner so Windows can release that handle first.
          const bool temporary = file && path == file->fileName();
          const bool saved = temporary ? file->rename(cachePath_)
                                       : expandedFile.rename(cachePath_);
          if (!saved) {
            // Another window may have published the same cache entry.
            // Validate its file too, rather than overwriting it.
            if (QFileInfo(cachePath_).isFile()) {
              inspect(cachePath_, serial, true);
              return;
            }
            fail(tr("Cannot save the verified PDB to %1: %2")
                     .arg(cachePath_, temporary ? file->errorString()
                                                : expandedFile.errorString()));
            return;
          }
          if (temporary)
            file->setAutoRemove(false);
        }
        const auto cache = cachePath_;
        QTimer::singleShot(0, this,
                           [this, cache, serial] { apply(cache, serial); });
      },
      [this, serial, path, cached, file = file_,
       expanded = expanded_](const QString &, const QString &message) {
        Q_UNUSED(file);
        Q_UNUSED(expanded);
        if (!current(serial))
          return;
        if (cached && !file_) {
          // Only this application's invalid cache entry is removed;
          // locally discovered and manually selected PDBs are untouched.
          QFile::remove(path);
          QTimer::singleShot(0, this, [this, serial] { ask(serial); });
        } else {
          fail(tr("Symbol verification failed: %1").arg(message));
        }
      });
}

void MicrosoftSymbols::apply(const QString &path, quint64 serial) {
  if (!current(serial))
    return;
  // pdb_load repeats the strict identity check transactionally and returns
  // the final path's stamp. No unverified-analysis fallback is used here.
  const QJsonObject selection{
      {"path", path}, {"mode", "verified"}, {"download", request_}};
  cancel();
  session_.loadPdb(selection);
}

void MicrosoftSymbols::fail(const QString &message) {
  cancel();
  emit session_.message(message, 1);
}
} // namespace neverd::gui
