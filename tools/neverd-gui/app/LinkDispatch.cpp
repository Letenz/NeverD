//===- LinkDispatch.cpp - A link clicked anywhere --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LinkDispatch.h"

#include "../ResourceLimits.h"
#include "DeepLink.h"
#include "InstanceServer.h"
#include "SettingsKeys.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

namespace neverd::gui {
namespace {

QString fileSha256(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file))
    return {};
  return QString::fromLatin1(hash.result().toHex());
}

} // namespace

InstanceChoice chooseInstance(const QList<QJsonObject> &instances,
                              const DeepLink &link) {
  QList<int> matches;
  for (int index = 0; index < instances.size(); ++index) {
    const QJsonObject &info = instances[index];
    if (!info.value(QStringLiteral("loaded")).toBool())
      continue;
    if (!link.sha256.isEmpty()
            ? link.namesHash(info.value(QStringLiteral("sha256")).toString())
        : !link.input.isEmpty()
            ? linkNamesInput(
                  link.input,
                  info.value(QStringLiteral("input_name")).toString())
            : true)
      matches << index;
  }
  if (matches.isEmpty())
    return {};
  if (matches.size() > 1 && link.input.isEmpty() && link.sha256.isEmpty())
    return {-1, true};
  return {matches.front(), false};
}

Dispatched dispatchLink(const QString &text, const DeepLink &link) {
  QList<QJsonObject> infos;
  QStringList servers;
  for (const auto &instance : InstanceServer::discover())
    if (const auto info = InstanceServer::request(
            instance.server, {{QStringLiteral("cmd"), "get_info"}})) {
      infos << *info;
      servers << instance.server;
    }
  const InstanceChoice choice = chooseInstance(infos, link);
  Dispatched result;
  if (choice.ambiguous) {
    result.sent = true;
    result.message = QCoreApplication::translate(
        "LinkDispatch",
        "%n running NeverDs have programs open, and the link names none of "
        "them; give it the input's name.",
        nullptr, int(infos.size()));
    return result;
  }
  if (choice.index < 0)
    return result;
  const QJsonObject &info = infos[choice.index];
  const QString input = info.value(QStringLiteral("input")).toString();
  const auto reply = InstanceServer::request(
      servers[choice.index],
      {{QStringLiteral("cmd"), "open_link"}, {QStringLiteral("uri"), text}},
      ::neverd::limits::gui::kLinkDispatchOpenTimeoutMs);
  result.sent = true;
  if (!reply) {
    result.message =
        QCoreApplication::translate(
            "LinkDispatch", "The NeverD that has %1 open did not answer.")
            .arg(input);
  } else if (reply->value(QStringLiteral("status")).toString() ==
             QLatin1String("ok")) {
    result.opened = true;
    result.message =
        QCoreApplication::translate(
            "LinkDispatch", "Opened in the NeverD that has %1 open (process "
                            "%2).")
            .arg(input)
            .arg(info.value(QStringLiteral("pid")).toInteger());
  } else {
    result.message = reply->value(QStringLiteral("message")).toString();
  }
  return result;
}

QString describeInstances() {
  QStringList lines;
  for (const auto &instance : InstanceServer::discover()) {
    const auto info = InstanceServer::request(
        instance.server, {{QStringLiteral("cmd"), "get_info"}});
    if (!info) {
      lines << QCoreApplication::translate("LinkDispatch",
                                           "%1\t(does not answer)")
                   .arg(instance.pid);
      continue;
    }
    if (!info->value(QStringLiteral("loaded")).toBool()) {
      lines << QCoreApplication::translate("LinkDispatch",
                                           "%1\t(no program open)")
                   .arg(instance.pid);
      continue;
    }
    lines << QStringLiteral("%1\t%2\t%3")
                 .arg(instance.pid)
                 .arg(info->value(QStringLiteral("input")).toString(),
                      info->value(QStringLiteral("analysis_complete")).toBool()
                          ? QCoreApplication::translate("LinkDispatch",
                                                        "analysis complete")
                          : QCoreApplication::translate("LinkDispatch",
                                                        "analyzing"));
  }
  if (lines.isEmpty())
    return QCoreApplication::translate("LinkDispatch",
                                       "NeverD is not running.");
  return lines.join(QLatin1Char('\n'));
}

std::optional<QString> recentFileFor(const DeepLink &link) {
  if (link.input.isEmpty() && link.sha256.isEmpty())
    return std::nullopt;
  for (const QString &path :
       QSettings().value(settings::RecentFiles).toStringList()) {
    const QFileInfo file(path);
    if (!file.isFile() ||
        (!link.input.isEmpty() && !linkNamesInput(link.input, file.fileName())))
      continue;
    if (!link.sha256.isEmpty() && !link.namesHash(fileSha256(path)))
      continue;
    return file.absoluteFilePath();
  }
  return std::nullopt;
}

bool registerLinkHandler(bool install, QString &message) {
  const QString executable =
      QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
#if defined(Q_OS_WIN)
  // HKEY_CURRENT_USER\Software\Classes\neverd, the user's own, as Windows
  // registers a URL scheme.
  QSettings scheme(
      QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\neverd"),
      QSettings::NativeFormat);
  if (!install) {
    scheme.remove(QString());
    scheme.sync();
    message = QCoreApplication::translate(
        "LinkDispatch", "neverd:// links no longer open in NeverD.");
    return scheme.status() == QSettings::NoError;
  }
  scheme.setValue(QStringLiteral("Default"), QStringLiteral("URL:NeverD link"));
  scheme.setValue(QStringLiteral("URL Protocol"), QString());
  scheme.setValue(QStringLiteral("shell/open/command/Default"),
                  QStringLiteral("\"%1\" --link \"%2\"")
                      .arg(executable, QStringLiteral("%1")));
  scheme.sync();
  if (scheme.status() != QSettings::NoError) {
    message = QCoreApplication::translate(
        "LinkDispatch", "The registry did not take the neverd:// handler.");
    return false;
  }
  message = QCoreApplication::translate("LinkDispatch",
                                        "neverd:// links now open in %1.")
                .arg(executable);
  return true;
#elif defined(Q_OS_MACOS)
  // The application bundle declares the scheme (CFBundleURLTypes), and
  // LaunchServices hands its links to the running workbench.
  Q_UNUSED(executable);
  message = install ? QCoreApplication::translate(
                          "LinkDispatch",
                          "On macOS the NeverD application declares neverd:// "
                          "links; they open in it once it is in Applications.")
                    : QCoreApplication::translate(
                          "LinkDispatch",
                          "On macOS the NeverD application declares neverd:// "
                          "links; remove the application to stop that.");
  return true;
#else
  // A desktop entry the user's own, which xdg-mime makes the scheme's
  // handler.
  const QString applications =
      QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
  const QString entry = applications + QStringLiteral("/neverd-links.desktop");
  if (!install) {
    if (QFile::exists(entry) && !QFile::remove(entry)) {
      message =
          QCoreApplication::translate("LinkDispatch", "Could not remove %1.")
              .arg(entry);
      return false;
    }
    message = QCoreApplication::translate(
        "LinkDispatch", "neverd:// links no longer open in NeverD.");
    return true;
  }
  QFile file(entry);
  if (!QDir().mkpath(applications) ||
      !file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    message = QCoreApplication::translate("LinkDispatch", "Could not write %1.")
                  .arg(entry);
    return false;
  }
  // The desktop entry format quotes an argument with spaces and doubles a
  // percent sign.
  QString quoted = executable;
  quoted.replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
      .replace(QLatin1Char('"'), QStringLiteral("\\\""))
      .replace(QLatin1Char('%'), QStringLiteral("%%"));
  QTextStream(&file) << "[Desktop Entry]\n"
                     << "Type=Application\n"
                     << "Name=NeverD\n"
                     << "Comment=Open neverd:// links in NeverD\n"
                     << "Exec=\"" << quoted << "\" --link %u\n"
                     << "NoDisplay=true\n"
                     << "Terminal=false\n"
                     << "MimeType=x-scheme-handler/neverd;\n";
  file.close();
  const int status = QProcess::execute(
      QStringLiteral("xdg-mime"),
      {QStringLiteral("default"), QStringLiteral("neverd-links.desktop"),
       QStringLiteral("x-scheme-handler/neverd")});
  if (status != 0) {
    message = QCoreApplication::translate(
                  "LinkDispatch",
                  "Wrote %1, but xdg-mime did not make it the handler of "
                  "neverd:// links.")
                  .arg(entry);
    return false;
  }
  message = QCoreApplication::translate("LinkDispatch",
                                        "neverd:// links now open in %1.")
                .arg(executable);
  return true;
#endif
}

} // namespace neverd::gui
