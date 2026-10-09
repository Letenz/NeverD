#include "ExtensionsView.h"

#include "Session.h"
#include "ShrinkableRow.h"
#include "Theme.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {
constexpr int IdRole = Qt::UserRole;
constexpr int NamespaceRole = Qt::UserRole + 1;
} // namespace

ExtensionsView::ExtensionsView(Session &session, QWidget *parent)
    : QWidget(parent), session_(session) {
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  auto *splitter = new QSplitter(this);
  auto *left = new QWidget(splitter);
  auto *leftLayout = new QVBoxLayout(left);
  leftLayout->setContentsMargins(4, 4, 4, 4);
  auto *buttonRow = new QWidget(left);
  auto *buttons = new QHBoxLayout(buttonRow);
  buttons->setContentsMargins(0, 0, 0, 0);
  import_ = new QPushButton(tr("Import Manifest…"), buttonRow);
  run_ = new QPushButton(tr("Run"), buttonRow);
  unload_ = new QPushButton(tr("Unload"), buttonRow);
  buttons->addWidget(import_);
  buttons->addWidget(run_);
  buttons->addWidget(unload_);
  buttons->addStretch(1);
  makeRowShrinkable(*buttonRow);
  leftLayout->addWidget(buttonRow);
  list_ = new QListWidget(left);
  list_->setToolTip(
      tr("Import a declarative manifest to add analysis commands and views."));
  leftLayout->addWidget(list_, 1);
  result_ = new QPlainTextEdit(splitter);
  result_->setReadOnly(true);
  result_->setFont(Theme::instance().codeFont());
  result_->setPlaceholderText(
      tr("Select an extension command to inspect its result."));
  splitter->setStretchFactor(1, 1);
  layout->addWidget(splitter);
  connect(import_, &QPushButton::clicked, this,
          &ExtensionsView::importManifest);
  connect(run_, &QPushButton::clicked, this, &ExtensionsView::run);
  connect(list_, &QListWidget::itemActivated, this, &ExtensionsView::run);
  connect(unload_, &QPushButton::clicked, this, [this] {
    if (auto *item = list_->currentItem())
      session_.unregisterContributions(item->data(NamespaceRole).toString());
  });
  connect(list_, &QListWidget::currentItemChanged, this,
          &ExtensionsView::updateButtons);
  connect(&session_, &Session::contributionsChanged, this,
          &ExtensionsView::refresh);
  connect(&session_, &Session::stateChanged, this,
          &ExtensionsView::updateButtons);
  connect(&session_, &Session::contributionResult, this,
          [this](const QJsonObject &result) {
            result_->setPlainText(QString::fromUtf8(
                QJsonDocument(result).toJson(QJsonDocument::Indented)));
          });
  refresh();
}

void ExtensionsView::importManifest() {
  const auto path = QFileDialog::getOpenFileName(
      this, tr("Import extension manifest"), {}, tr("Manifests (*.json)"));
  if (!path.isEmpty())
    session_.registerContributions(path);
}

void ExtensionsView::refresh() {
  const QString current = list_->currentItem()
                              ? list_->currentItem()->data(IdRole).toString()
                              : QString();
  list_->clear();
  for (const auto &value : session_.contributions()) {
    const auto entry = value.toObject();
    auto *item =
        new QListWidgetItem(QStringLiteral("%1\n%2 · %3")
                                .arg(entry.value("title").toString(),
                                     entry.value("namespace").toString(),
                                     entry.value("kind").toString()),
                            list_);
    item->setData(IdRole, entry.value("id").toString());
    item->setData(NamespaceRole, entry.value("namespace").toString());
    if (entry.value("id").toString() == current)
      list_->setCurrentItem(item);
  }
  updateButtons();
}

void ExtensionsView::updateButtons() {
  const bool selected = list_->currentItem() != nullptr;
  import_->setEnabled(session_.loaded());
  run_->setEnabled(selected && session_.loaded());
  unload_->setEnabled(selected);
}

void ExtensionsView::run() {
  if (auto *item = list_->currentItem())
    session_.executeContribution(item->data(IdRole).toString(),
                                 location_ ? location_() : std::nullopt);
}

} // namespace neverd::gui
