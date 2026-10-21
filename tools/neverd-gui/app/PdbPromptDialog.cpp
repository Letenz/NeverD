//===- PdbPromptDialog.cpp - Load the PDB a PE names ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IDA's "Load PDB file" question, and the download and browse steps that
/// follow a "Yes" when no matching copy is on this computer.
//===----------------------------------------------------------------------===//

#include "PdbPromptDialog.h"

#include "../Config.h"
#include "Session.h"
#include "SymbolDownload.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace {

/// The host of Microsoft's public symbol server, which the question names
/// as IDA does.
constexpr char MicrosoftServerHost[] = "msdl.microsoft.com";

/// The whole of a line that is elided to fit.
constexpr char WholeText[] = "wholeText";

/// Elide the middle of \p label's whole text to the room it has.
void elideToFit(QLabel *label) {
  const QFontMetrics metrics(label->font());
  label->setText(
      metrics.elidedText(label->property(WholeText).toString(), Qt::ElideMiddle,
                         std::max(0, label->contentsRect().width())));
}

/// A line that shows the middle of a path elided and the whole of it in its
/// tooltip, and takes the width the dialog gives it.
void setElided(QLabel *label, const QString &text) {
  label->setProperty(WholeText, text);
  label->setToolTip(text);
  elideToFit(label);
}

QLabel *line(QWidget *parent, const char *name) {
  auto *label = new QLabel(parent);
  label->setObjectName(QLatin1String(name));
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  return label;
}

} // namespace

PdbPromptDialog::PdbPromptDialog(Session &session, const QString &input,
                                 const QJsonObject &reference,
                                 QNetworkAccessManager *network,
                                 QWidget *parent)
    : QDialog(parent), session_(session), input_(input), reference_(reference),
      download_(new SymbolDownload(network, this)) {
  setObjectName(QStringLiteral("pdbPrompt"));
  setWindowTitle(tr("Load PDB file"));
  // The question fits its text: each step sets the height it needs.
  setFixedWidth(::neverd::limits::gui_config::kPdbPromptDialogDialogWidth);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(
      ::neverd::limits::gui_config::kPdbPromptDialogMargin,
      ::neverd::limits::gui_config::kPdbPromptDialogMargin,
      ::neverd::limits::gui_config::kPdbPromptDialogMargin,
      ::neverd::limits::gui_config::kPdbPromptDialogMargin);
  layout->setSpacing(::neverd::limits::gui_config::kPdbPromptDialogLineSpacing);

  // What the input names, where a copy is, and what to do: each a group of
  // lines, apart from the next.
  auto *heading = line(this, "heading");
  heading->setText(
      tr("The input file was linked with debug information stored here:"));
  layout->addWidget(heading);
  recorded_ = line(this, "recorded");
  recorded_->setWordWrap(false);
  recorded_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  recorded_->setFont(Theme::instance().codeFont());
  setElided(recorded_, reference_.value("recorded").toString());
  layout->addWidget(recorded_);
  layout->addSpacing(
      ::neverd::limits::gui_config::kPdbPromptDialogGroupSpacing);

  where_ = line(this, "where");
  layout->addWidget(where_);
  path_ = line(this, "path");
  path_->setWordWrap(false);
  path_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  path_->setProperty("statusLine", true);
  layout->addWidget(path_);
  progress_ = new QProgressBar(this);
  progress_->setObjectName(QStringLiteral("progress"));
  progress_->setTextVisible(false);
  progress_->setFixedHeight(
      ::neverd::limits::gui_config::kPdbPromptDialogProgressHeight);
  layout->addWidget(progress_);
  detail_ = line(this, "detail");
  detail_->setProperty("statusLine", true);
  layout->addWidget(detail_);
  layout->addSpacing(
      ::neverd::limits::gui_config::kPdbPromptDialogGroupSpacing);
  question_ = line(this, "question");
  layout->addWidget(question_);
  layout->addSpacing(
      ::neverd::limits::gui_config::kPdbPromptDialogGroupSpacing);

  buttons_ = new QDialogButtonBox(this);
  buttons_->setCenterButtons(true);
  yes_ = buttons_->addButton(QDialogButtonBox::Yes);
  no_ = buttons_->addButton(QDialogButtonBox::No);
  cancel_ = buttons_->addButton(QDialogButtonBox::Cancel);
  retry_ = buttons_->addButton(QDialogButtonBox::Retry);
  browse_ = buttons_->addButton(tr("&Browse…"), QDialogButtonBox::AcceptRole);
  yes_->setObjectName(QStringLiteral("yes"));
  no_->setObjectName(QStringLiteral("no"));
  cancel_->setObjectName(QStringLiteral("cancel"));
  retry_->setObjectName(QStringLiteral("retry"));
  browse_->setObjectName(QStringLiteral("browse"));
  layout->addWidget(buttons_);

  connect(yes_, &QPushButton::clicked, this, [this] {
    const auto found = reference_.value("found").toObject();
    if (!found.isEmpty())
      choose(found.value("path").toString());
    else
      download();
  });
  connect(no_, &QPushButton::clicked, this, &PdbPromptDialog::reject);
  connect(cancel_, &QPushButton::clicked, this, [this] {
    ++attempt_;
    download_->cancel();
    failed(tr("The download was cancelled."), true);
  });
  connect(retry_, &QPushButton::clicked, this, &PdbPromptDialog::download);
  connect(browse_, &QPushButton::clicked, this, &PdbPromptDialog::browse);
  connect(download_, &SymbolDownload::progress, this,
          &PdbPromptDialog::progress);
  connect(download_, &SymbolDownload::downloaded, this,
          &PdbPromptDialog::verify);
  // A server that answered has nothing to retry for; a failed transfer does.
  connect(download_, &SymbolDownload::failed, this,
          [this](const QString &message, bool notFound) {
            failed(message, !notFound);
          });
  ask();
}

void PdbPromptDialog::ask() {
  const auto found = reference_.value("found").toObject();
  const auto servers = reference_.value("servers").toArray();
  progress_->hide();
  detail_->hide();
  path_->hide();
  if (!found.isEmpty()) {
    const auto location = found.value("location").toString();
    where_->setText(location == QLatin1String("recorded")
                        ? tr("A matching copy is at that path.")
                    : location == QLatin1String("beside")
                        ? tr("A matching copy is beside the input file:")
                    : location == QLatin1String("directory")
                        ? tr("A matching copy is in a symbol directory:")
                        : tr("A matching copy is in the symbol store:"));
    if (location != QLatin1String("recorded"))
      showPath(found.value("path").toString());
    question_->setText(tr("Do you want to load it?"));
    setButtons({yes_, no_});
    return;
  }
  // The copies that are here but describe another build.
  QStringList others;
  for (const auto &value : reference_.value("rejected").toArray()) {
    const auto copy = value.toObject();
    const auto shown = QDir::toNativeSeparators(copy.value("path").toString());
    others.append(copy.value("reason").toString() == QLatin1String("mismatch")
                      ? tr("%1 belongs to another build.").arg(shown)
                      : tr("%1 cannot be read.").arg(shown));
  }
  if (!others.isEmpty()) {
    detail_->setText(others.join(QLatin1Char('\n')));
    detail_->show();
  }
  if (servers.isEmpty()) {
    where_->setText(tr("No matching copy is on this computer, and no symbol "
                       "server is configured."));
    question_->setText(tr("Do you want to browse for the PDB file on disk?"));
    setButtons({browse_, no_});
    return;
  }
  const QUrl server(servers.first().toObject().value("url").toString());
  where_->setText(tr("No matching copy is on this computer. Only the PDB's "
                     "name and build identity are sent to look for it."));
  question_->setText(
      server.host().compare(QLatin1String(MicrosoftServerHost),
                            Qt::CaseInsensitive) == 0
          ? tr("Do you want to look for this file on the Microsoft Symbol "
               "Server?")
          : tr("Do you want to look for this file on the symbol server "
               "%1?")
                .arg(server.host()));
  setButtons({yes_, no_});
}

void PdbPromptDialog::download() {
  path_->hide();
  detail_->hide();
  question_->hide();
  progress_->setRange(0, 0);
  progress_->show();
  ++attempt_;
  // A server that fails at once answers before start() returns, and the
  // dialog then shows the failure instead.
  download_->start(reference_.value("servers").toArray(),
                   reference_.value("file").toString());
  if (!download_->running())
    return;
  showServer();
  setButtons({cancel_});
}

void PdbPromptDialog::showServer() {
  const auto text =
      tr("Downloading %1 from %2…")
          .arg(reference_.value("file").toString(), download_->host());
  if (where_->text() == text)
    return;
  where_->setText(text);
  fit();
}

void PdbPromptDialog::progress(qint64 received, qint64 total) {
  // A server that has none hands the request to the next.
  showServer();
  // Nothing to count while the connection is made.
  if (received <= 0 && total <= 0)
    return;
  const QLocale locale;
  if (total > 0) {
    progress_->setRange(0, 1000);
    progress_->setValue(int(1000 * received / total));
    detail_->setText(tr("%1 of %2")
                         .arg(locale.formattedDataSize(received),
                              locale.formattedDataSize(total)));
  } else {
    detail_->setText(locale.formattedDataSize(received));
  }
  if (detail_->isHidden()) {
    detail_->show();
    fit();
  }
}

void PdbPromptDialog::verify(const QString &path) {
  const auto attempt = attempt_;
  where_->setText(tr("Verifying %1…").arg(reference_.value("file").toString()));
  progress_->setRange(0, 0);
  detail_->hide();
  // The engine names the copy that matches the input; a file that is not it
  // leaves the store again.
  session_.read(
      QStringLiteral("pdb_reference"), {{"path", input_}}, this,
      [this, path, attempt](const QJsonObject &payload) {
        if (attempt != attempt_)
          return;
        const auto found =
            payload.value("pdb").toObject().value("found").toObject();
        if (!found.isEmpty() &&
            QFileInfo(found.value("path").toString()) == QFileInfo(path)) {
          choose(path);
          return;
        }
        QFile::remove(path);
        failed(tr("The downloaded file is not the PDB of this build."), false);
      },
      [this, attempt](const QString &, const QString &message) {
        if (attempt == attempt_)
          failed(message, true);
      });
}

void PdbPromptDialog::failed(const QString &message, bool retryable) {
  progress_->hide();
  path_->hide();
  where_->setText(tr("Could not find PDB file '%1'.")
                      .arg(reference_.value("file").toString()));
  detail_->setText(message);
  detail_->show();
  question_->setText(
      retryable ? tr("Do you want to try again, or to browse for the PDB file "
                     "on disk?")
                : tr("Do you want to browse for the PDB file on disk?"));
  question_->show();
  if (retryable)
    setButtons({retry_, browse_, no_});
  else
    setButtons({browse_, no_});
}

void PdbPromptDialog::browse() {
  const auto path = QFileDialog::getOpenFileName(
      this, tr("Load PDB file"), QFileInfo(input_).absolutePath(),
      tr("PDB files (*.pdb);;All files (*)"));
  if (!path.isEmpty())
    choose(path);
}

void PdbPromptDialog::choose(const QString &path) {
  choice_ = {{"path", path}, {"mode", "verified"}};
  QDialog::accept();
}

void PdbPromptDialog::reject() {
  // Closing the question answers it: the input loads without its PDB.
  ++attempt_;
  download_->cancel();
  if (choice_.isEmpty())
    choice_ = {{"mode", "none"}};
  QDialog::reject();
}

void PdbPromptDialog::showPath(const QString &text) {
  setElided(path_, QDir::toNativeSeparators(text));
  path_->show();
}

void PdbPromptDialog::resizeEvent(QResizeEvent *event) {
  QDialog::resizeEvent(event);
  elideToFit(recorded_);
  elideToFit(path_);
}

void PdbPromptDialog::keyPressEvent(QKeyEvent *event) {
  // Escape stops a download, as its Cancel does, rather than the question.
  if (event->key() == Qt::Key_Escape && download_->running()) {
    cancel_->click();
    return;
  }
  QDialog::keyPressEvent(event);
}

void PdbPromptDialog::setButtons(std::initializer_list<QPushButton *> shown) {
  for (auto *button : {yes_, no_, cancel_, retry_, browse_}) {
    button->setVisible(std::find(shown.begin(), shown.end(), button) !=
                       shown.end());
    button->setDefault(button == *shown.begin());
  }
  (*shown.begin())->setFocus();
  fit();
}

void PdbPromptDialog::fit() {
  // Lines and buttons shown or hidden reach the layouts' sizes once their
  // layout requests are handled, the button box's among them.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
  layout()->activate();
  setFixedHeight(heightForWidth(width()));
}

} // namespace neverd::gui
