//===- InstanceServer.cpp - Other programs' way into a workbench -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "InstanceServer.h"

#include "DeepLink.h"
#include "SettingsKeys.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace neverd::gui {
namespace {

QJsonObject succeeded() { return {{QStringLiteral("status"), "ok"}}; }

QJsonObject failed(const QString &message) {
  return {{QStringLiteral("status"), "error"},
          {QStringLiteral("message"), message}};
}

/// A server's name: its process, and its serial among the process's.
QString instanceName(qint64 pid, int serial) {
  return QStringLiteral("%1-%2").arg(pid).arg(serial);
}

int nextSerial() {
  static int serial = 0;
  return ++serial;
}

#ifdef Q_OS_WIN
QString pipeName(const QString &name) {
  return QStringLiteral("neverd-instance-") + name;
}
#endif

bool processAlive(qint64 pid) {
  if (pid <= 0)
    return false;
#ifdef Q_OS_WIN
  HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
  if (!process)
    return GetLastError() == ERROR_ACCESS_DENIED;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
  CloseHandle(process);
  return alive;
#else
  return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

#ifndef Q_OS_WIN
/// Whether the peer of the connected socket \p descriptor is this user.
bool sameUser(qintptr descriptor) {
#ifdef Q_OS_LINUX
  ucred credentials{};
  socklen_t size = sizeof credentials;
  return getsockopt(int(descriptor), SOL_SOCKET, SO_PEERCRED, &credentials,
                    &size) == 0 &&
         credentials.uid == getuid();
#else
  uid_t user = 0;
  gid_t group = 0;
  return getpeereid(int(descriptor), &user, &group) == 0 && user == getuid();
#endif
}
#endif

} // namespace

InstanceServer::InstanceServer(QObject *parent) : QObject(parent) {
  connect(&server_, &QLocalServer::newConnection, this,
          &InstanceServer::accept);
}

InstanceServer::~InstanceServer() { stop(); }

QString InstanceServer::directory() {
#ifdef Q_OS_WIN
  // Only the files naming the pipes live here.
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#else
  // $XDG_RUNTIME_DIR, or the directory Qt makes in its place, both the
  // user's own.
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
#endif
  if (base.isEmpty())
    return {};
  const QString path = base + QStringLiteral("/neverd-instances");
  if (!QDir().mkpath(path) ||
      !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner |
                                       QFile::ExeOwner))
    return {};
  return path;
}

bool InstanceServer::start() {
  stop();
  error_.clear();
  if (QSettings().value(settings::InstancesDisabled, false).toBool()) {
    error_ = QStringLiteral("turned off by the instances/disabled setting");
    return false;
  }
  const QString directory = InstanceServer::directory();
  if (directory.isEmpty()) {
    error_ = QStringLiteral("no private directory for the server");
    return false;
  }
  const QString name =
      instanceName(QCoreApplication::applicationPid(), nextSerial());
  server_.setSocketOptions(QLocalServer::UserAccessOption);
#ifdef Q_OS_WIN
  const QString endpoint = pipeName(name);
#else
  const QString endpoint = directory + QLatin1Char('/') + name;
  QLocalServer::removeServer(endpoint);
#endif
  if (!server_.listen(endpoint)) {
    error_ = server_.errorString();
    return false;
  }
#ifdef Q_OS_WIN
  marker_ = directory + QLatin1Char('/') + name;
  QFile marker(marker_);
  if (!marker.open(QIODevice::WriteOnly)) {
    error_ = marker.errorString();
    stop();
    return false;
  }
#endif
  return true;
}

void InstanceServer::stop() {
  server_.close();
  if (!marker_.isEmpty())
    QFile::remove(std::exchange(marker_, {}));
}

void InstanceServer::accept() {
  while (QLocalSocket *socket = server_.nextPendingConnection()) {
#ifndef Q_OS_WIN
    // The directory is the user's own; this refuses whatever got in anyway.
    if (!sameUser(socket->socketDescriptor())) {
      socket->abort();
      socket->deleteLater();
      continue;
    }
#endif
    connect(socket, &QLocalSocket::readyRead, this,
            [this, socket] { read(socket); });
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
  }
}

void InstanceServer::read(QLocalSocket *socket) {
  QByteArray buffer =
      socket->property("neverdRequest").toByteArray() + socket->readAll();
  const auto rejectOversized = [socket] {
    reply(socket,
          failed(
              QStringLiteral("Request exceeds %1 bytes")
                  .arg(::neverd::limits::gui::kInstanceServerMaxRequestBytes)));
    socket->disconnectFromServer();
    socket->setProperty("neverdRequest", QByteArray());
  };
  for (qsizetype newline; (newline = buffer.indexOf('\n')) >= 0;) {
    if (newline > ::neverd::limits::gui::kInstanceServerMaxRequestBytes)
      return rejectOversized();
    const QByteArray line = buffer.left(newline);
    buffer.remove(0, newline + 1);
    if (!line.trimmed().isEmpty())
      answer(socket, line);
  }
  // Bound the request before parsing or dispatching it, including the
  // complete objects clients send without a newline.
  if (buffer.size() > ::neverd::limits::gui::kInstanceServerMaxRequestBytes)
    return rejectOversized();
  // IDA's hcli writes one object without a newline and waits.
  if (!buffer.trimmed().isEmpty()) {
    QJsonParseError error;
    QJsonDocument::fromJson(buffer, &error);
    if (error.error == QJsonParseError::NoError) {
      answer(socket, buffer);
      buffer.clear();
    }
  }
  socket->setProperty("neverdRequest", buffer);
}

void InstanceServer::answer(QLocalSocket *socket, const QByteArray &text) {
  QJsonParseError parse;
  const QJsonDocument document = QJsonDocument::fromJson(text, &parse);
  if (parse.error != QJsonParseError::NoError)
    return reply(
        socket,
        failed(
            QStringLiteral("JSON parse error: %1").arg(parse.errorString())));
  if (!document.isObject())
    return reply(socket, failed(QStringLiteral("Expected a JSON object")));
  const QJsonObject request = document.object();
  const QString command = request.value(QStringLiteral("cmd")).toString();
  if (command == QLatin1String("ping"))
    return reply(socket, succeeded());
  if (command == QLatin1String("get_info") ||
      command == QLatin1String("is_analysis_complete")) {
    QJsonObject info = info_ ? info_() : QJsonObject();
    if (command == QLatin1String("is_analysis_complete"))
      return reply(
          socket, {{QStringLiteral("status"), "ok"},
                   {QStringLiteral("analysis_complete"),
                    info.value(QStringLiteral("analysis_complete")).toBool()}});
    info.insert(QStringLiteral("status"), "ok");
    info.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
    info.insert(QStringLiteral("server"), serverName());
    return reply(socket, info);
  }
  if (command == QLatin1String("open_link")) {
    const ParsedLink parsed =
        parseDeepLink(request.value(QStringLiteral("uri")).toString());
    if (!parsed.link)
      return reply(socket, failed(parsed.error));
    if (!handler_)
      return reply(socket,
                   failed(QStringLiteral("This workbench opens no links")));
    // The link resolves through the worker; its reply follows.
    handler_(*parsed.link,
             [socket = QPointer<QLocalSocket>(socket)](const QString &error) {
               if (socket)
                 reply(socket, error.isEmpty() ? succeeded() : failed(error));
             });
    return;
  }
  reply(socket, failed(QStringLiteral("Unknown command: %1").arg(command)));
}

void InstanceServer::reply(QLocalSocket *socket, const QJsonObject &reply) {
  if (socket->state() != QLocalSocket::ConnectedState)
    return;
  socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
  socket->flush();
}

QList<InstanceServer::Instance> InstanceServer::discover() {
  QList<Instance> instances;
  const QString directory = InstanceServer::directory();
  if (directory.isEmpty())
    return instances;
  static const QRegularExpression Name(QStringLiteral("^([0-9]+)-[0-9]+$"));
  // A socket is a system entry to QDir.
  const auto entries = QDir(directory).entryInfoList(
      QDir::Files | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot);
  for (const QFileInfo &entry : entries) {
    const auto match = Name.match(entry.fileName());
    if (!match.hasMatch())
      continue;
    const qint64 pid = match.captured(1).toLongLong();
    if (!processAlive(pid)) {
      QFile::remove(entry.filePath());
      continue;
    }
#ifdef Q_OS_WIN
    instances.push_back({pid, pipeName(entry.fileName())});
#else
    instances.push_back({pid, entry.filePath()});
#endif
  }
  std::sort(instances.begin(), instances.end(),
            [](const Instance &left, const Instance &right) {
              return left.pid != right.pid ? left.pid < right.pid
                                           : left.server < right.server;
            });
  return instances;
}

std::optional<QJsonObject> InstanceServer::request(const QString &server,
                                                   const QJsonObject &request,
                                                   int timeoutMs) {
  QLocalSocket socket;
  socket.connectToServer(server);
  QDeadlineTimer deadline(timeoutMs);
  if (!socket.waitForConnected(int(deadline.remainingTime())))
    return std::nullopt;
  socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
  if (!socket.waitForBytesWritten(int(deadline.remainingTime())))
    return std::nullopt;
  QByteArray buffer;
  while (!buffer.contains('\n')) {
    if (deadline.hasExpired() ||
        !socket.waitForReadyRead(int(deadline.remainingTime())))
      return std::nullopt;
    buffer += socket.readAll();
    if (buffer.size() > ::neverd::limits::gui::kInstanceServerMaxReplyBytes)
      return std::nullopt;
  }
  const QJsonDocument document =
      QJsonDocument::fromJson(buffer.left(buffer.indexOf('\n')));
  if (!document.isObject())
    return std::nullopt;
  return document.object();
}

} // namespace neverd::gui
