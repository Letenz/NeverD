//===- MicrosoftSymbolsTests.cpp - Symbol download lifecycle tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise consent, cache verification and stale completion with an in-memory
/// network reply and the fixture worker. No test contacts a symbol server.
//===----------------------------------------------------------------------===//

#include "MicrosoftSymbols.h"
#include "Session.h"

#include <QFile>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cstring>

using namespace neverd::gui;

namespace {
class Reply final : public QNetworkReply {
public:
  Reply(const QNetworkRequest &request, int status, QByteArray bytes,
        bool delayed, int &aborted, QObject *parent)
      : QNetworkReply(parent), bytes_(std::move(bytes)), aborted_(aborted) {
    setRequest(request);
    setUrl(request.url());
    setOperation(QNetworkAccessManager::GetOperation);
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    if (status != 200)
      setError(ContentNotFoundError, QStringLiteral("fixture not found"));
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    if (!delayed)
      QTimer::singleShot(0, this, [this] {
        if (isFinished())
          return;
        emit readyRead();
        emit downloadProgress(bytes_.size(), bytes_.size());
        setFinished(true);
        emit finished();
      });
  }
  void abort() override {
    if (isFinished())
      return;
    ++aborted_;
    setError(OperationCanceledError, QStringLiteral("cancelled"));
    setFinished(true);
    emit finished();
  }
  qint64 bytesAvailable() const override {
    return bytes_.size() - offset_ + QNetworkReply::bytesAvailable();
  }

protected:
  qint64 readData(char *data, qint64 length) override {
    const auto count = std::min(length, qint64(bytes_.size()) - offset_);
    if (!count)
      return -1;
    std::memcpy(data, bytes_.constData() + offset_, size_t(count));
    offset_ += count;
    return count;
  }

private:
  QByteArray bytes_;
  qint64 offset_ = 0;
  int &aborted_;
};

class Network final : public QNetworkAccessManager {
public:
  QList<QUrl> requests;
  QByteArray bytes = "matching fixture PDB";
  int status = 200, aborted = 0;
  bool delayed = false;

protected:
  QNetworkReply *createRequest(Operation, const QNetworkRequest &request,
                               QIODevice *) override {
    requests.append(request.url());
    return new Reply(request, status, bytes, delayed, aborted, this);
  }
};

QString fixture(const QTemporaryDir &directory, const QString &name) {
  const auto path = directory.filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write("fixture") != 7)
    return {};
  return path;
}

void choose(QMessageBox *prompt, QMessageBox::ButtonRole role) {
  for (auto *button : prompt->buttons())
    if (prompt->buttonRole(button) == role) {
      button->click();
      return;
    }
}
} // namespace

class MicrosoftSymbolsTests final : public QObject {
  Q_OBJECT
private slots:
  void requestCannotRedirectOrEscapeCache() {
    const QJsonObject valid{{"file", "kernel name.pdb"},
                            {"key", "123456789ABCDEF0123456789ABCDEF02a"}};
    const auto url = MicrosoftSymbols::downloadUrl(valid);
    QCOMPARE(url.scheme(), QStringLiteral("https"));
    QCOMPARE(url.host(), QStringLiteral("msdl.microsoft.com"));
    QVERIFY(url.toEncoded().contains("kernel%20name.pdb"));
    QVERIFY(MicrosoftSymbols::downloadUrl(valid, true)
                .path()
                .endsWith("kernel name.pd_"));
    for (const auto &file : {"../other.pdb", "C:/other.pdb", "bad?query.pdb",
                             "bad:stream.pdb", "bad\nname.pdb"}) {
      auto invalid = valid;
      invalid["file"] = file;
      QVERIFY(!MicrosoftSymbols::validRequest(invalid));
      QVERIFY(MicrosoftSymbols::downloadUrl(invalid).isEmpty());
    }
    auto invalid = valid;
    invalid["key"] = "../../other";
    QVERIFY(!MicrosoftSymbols::validRequest(invalid));
  }

  void onlyMissingEligibleSymbolsPrompt_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("debug");
    QTest::newRow("local-pdb") << QStringLiteral("symbols-local.bin") << true;
    QTest::newRow("no-identity")
        << QStringLiteral("symbols-absent.bin") << true;
    QTest::newRow("disabled") << QStringLiteral("symbols-missing.bin") << false;
  }
  void onlyMissingEligibleSymbolsPrompt() {
    QFETCH(QString, name);
    QFETCH(bool, debug);
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QWidget window;
    Network network;
    MicrosoftSymbols symbols(session, &window, &network, directory.path());
    LoadOptions options;
    options.debugInfo = debug;
    session.open(fixture(directory, name), options);
    QTRY_VERIFY(session.loaded());
    QCoreApplication::processEvents();
    QVERIFY(!session.metadata().value("pdb_download").isObject());
    QVERIFY(!window.findChild<QMessageBox *>("downloadPdbPrompt"));
    QVERIFY(network.requests.isEmpty());
  }

  void declineDoesNotContactServer() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QWidget window;
    Network network;
    MicrosoftSymbols symbols(session, &window, &network, directory.path());
    session.open(fixture(directory, "symbols-missing.bin"));
    QTRY_VERIFY(window.findChild<QMessageBox *>("downloadPdbPrompt"));
    QVERIFY(network.requests.isEmpty());
    choose(window.findChild<QMessageBox *>("downloadPdbPrompt"),
           QMessageBox::RejectRole);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(network.requests.isEmpty());
    QVERIFY(session.loaded());
    QVERIFY(!session.metadata().value("pdb").isObject());
  }

  void consentDownloadsVerifiesAndReusesCache() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QWidget window;
    Network network;
    MicrosoftSymbols symbols(session, &window, &network, directory.path());
    const auto input = fixture(directory, "symbols-missing.bin");
    session.open(input);
    QTRY_VERIFY(window.findChild<QMessageBox *>("downloadPdbPrompt"));
    QVERIFY(network.requests.isEmpty());
    choose(window.findChild<QMessageBox *>("downloadPdbPrompt"),
           QMessageBox::AcceptRole);
    QTRY_VERIFY(session.metadata().value("pdb").isObject());
    QCOMPARE(network.requests.size(), 1);
    const auto selected = session.metadata().value("pdb").toObject();
    QCOMPARE(selected.value("mode").toString(), QStringLiteral("verified"));
    QVERIFY(selected.value("stamp").isObject());
    QFile cached(selected.value("path").toString());
    QVERIFY(cached.open(QIODevice::ReadOnly));
    QCOMPARE(cached.readAll(), network.bytes);
    cached.close();
    session.closeFile();
    session.open(input);
    QTRY_VERIFY(session.loaded() && session.metadata().value("pdb").isObject());
    QCOMPARE(network.requests.size(), 1);
  }

  void failedDownloadsDoNotReplaceAnalysis_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("not-found") << 404 << QByteArray("not found");
    QTest::newRow("wrong-pdb") << 200 << QByteArray("wrong PDB identity");
  }
  void failedDownloadsDoNotReplaceAnalysis() {
    QFETCH(int, status);
    QFETCH(QByteArray, bytes);
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QWidget window;
    Network network;
    network.status = status;
    network.bytes = bytes;
    MicrosoftSymbols symbols(session, &window, &network, directory.path());
    session.open(fixture(directory, "symbols-missing.bin"));
    QTRY_VERIFY(window.findChild<QMessageBox *>("downloadPdbPrompt"));
    QSignalSpy messages(&session, &Session::message);
    choose(window.findChild<QMessageBox *>("downloadPdbPrompt"),
           QMessageBox::AcceptRole);
    QTRY_VERIFY(
        std::any_of(messages.cbegin(), messages.cend(),
                    [](const auto &args) { return args[1].toInt() == 1; }));
    QVERIFY(!network.requests.isEmpty());
    QVERIFY(session.loaded());
    QVERIFY(!session.metadata().value("pdb").isObject());
    QTRY_VERIFY(!window.findChild<QProgressDialog *>("downloadPdbProgress"));
  }

  void changingFileCancelsPendingDownload() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QWidget window;
    Network network;
    network.delayed = true;
    MicrosoftSymbols symbols(session, &window, &network, directory.path());
    session.open(fixture(directory, "symbols-missing.bin"));
    QTRY_VERIFY(window.findChild<QMessageBox *>("downloadPdbPrompt"));
    choose(window.findChild<QMessageBox *>("downloadPdbPrompt"),
           QMessageBox::AcceptRole);
    QTRY_COMPARE(network.requests.size(), 1);
    const auto next = fixture(directory, "symbols-absent.bin");
    session.open(next);
    QTRY_VERIFY(session.loaded() && session.filePath() == next);
    QTRY_COMPARE(network.aborted, 1);
    QVERIFY(!session.metadata().value("pdb").isObject());
    QTRY_VERIFY(!window.findChild<QProgressDialog *>("downloadPdbProgress"));
  }
};

QTEST_MAIN(MicrosoftSymbolsTests)
#include "MicrosoftSymbolsTests.moc"
