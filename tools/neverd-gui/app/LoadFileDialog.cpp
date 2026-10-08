#include "LoadFileDialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace {

/// The path as the dialog shows it: its end when it is long.
QString shownPath(const QString &path) {
  constexpr int Shown = 80;
  return path.size() > Shown ? QStringLiteral("...") + path.right(Shown) : path;
}

struct ProcessorFamily {
  const char *id, *name;
};
constexpr ProcessorFamily Families[] = {
#define NEVERD_PROCESSOR_FAMILY(Id, Name) {#Id, Name},
#include "Processors.def"
};

struct Processor {
  const char *family, *shortName, *name;
};
constexpr Processor Processors[] = {
#define NEVERD_PROCESSOR(Family, ShortName, Name) {#Family, ShortName, Name},
#include "Processors.def"
};

/// The list shows this many rows before it scrolls.
constexpr int ListedRows = 5;

} // namespace

LoadFileDialog::LoadFileDialog(const QString &path, const QJsonArray &rows,
                               QWidget *parent)
    : QDialog(parent), rows_(rows) {
  setWindowTitle(tr("Load a new file"));
  auto *layout = new QVBoxLayout(this);

  // The loaders that read the file, as the engine lists them.
  auto *heading = new QLabel(tr("Load file %1 &as").arg(shownPath(path)), this);
  heading->setTextFormat(Qt::PlainText);
  loaders_ = new QListWidget(this);
  loaders_->setObjectName(QStringLiteral("loaders"));
  loaders_->setToolTip(tr("The input file possibly has the listed formats"));
  heading->setBuddy(loaders_);
  for (const auto &value : rows_) {
    const auto row = value.toObject();
    const auto loader = row.value("loader").toString();
    QString text = row.value("text").toString();
    if (loader != QLatin1String("binary"))
      text += QStringLiteral(" [%1]").arg(loader);
    auto *item = new QListWidgetItem(text, loaders_);
    if (!row.value("loadable").toBool()) {
      item->setFlags(item->flags() &
                     ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
      item->setToolTip(row.value("reason").toString());
    }
  }
  const int shown = std::clamp(loaders_->count(), 1, ListedRows);
  loaders_->setFixedHeight(loaders_->sizeHintForRow(0) * shown +
                           2 * loaders_->frameWidth());
  layout->addWidget(heading);
  layout->addWidget(loaders_);

  // The processor the chosen loader reads the file as.
  auto *processorHeading = new QLabel(tr("Processor t&ype"), this);
  processors_ = new QTreeWidget(this);
  processors_->setObjectName(QStringLiteral("processors"));
  processors_->setColumnCount(2);
  processors_->setHeaderHidden(true);
  processors_->setToolTip(tr("The file's header states the processor"));
  processorHeading->setBuddy(processors_);
  for (const auto &family : Families) {
    auto *folder = new QTreeWidgetItem(
        processors_, {QCoreApplication::translate("Processors", family.name)});
    for (const auto &processor : Processors)
      if (QLatin1String(processor.family) == QLatin1String(family.id)) {
        auto *item = new QTreeWidgetItem(
            folder, {QCoreApplication::translate("Processors", processor.name),
                     QString::fromLatin1(processor.shortName)});
        item->setData(0, Qt::UserRole,
                      QString::fromLatin1(processor.shortName));
      }
  }
  processors_->expandAll();
  processors_->resizeColumnToContents(0);
  // Every loader NeverD has takes the processor from the header.
  processors_->setEnabled(false);
  layout->addWidget(processorHeading);
  layout->addWidget(processors_);

  // What loading does besides reading the image.
  auto *groups = new QHBoxLayout;
  auto *analysisGroup = new QGroupBox(tr("Analysis"), this);
  auto *analysisLayout = new QVBoxLayout(analysisGroup);
  analysis_ = new QCheckBox(tr("&Enabled"), analysisGroup);
  analysis_->setObjectName(QStringLiteral("analysis"));
  analysis_->setChecked(true);
  analysis_->setToolTip(tr("If turned off, NeverD will not analyze the program "
                           "in idle time"));
  indicator_ = new QCheckBox(tr("In&dicator enabled"), analysisGroup);
  indicator_->setObjectName(QStringLiteral("indicator"));
  indicator_->setChecked(true);
  indicator_->setToolTip(
      tr("Display the analysis progress in the status line"));
  analysisLayout->addWidget(analysis_);
  analysisLayout->addWidget(indicator_);
  auto *optionsGroup = new QGroupBox(tr("Options"), this);
  auto *optionsLayout = new QVBoxLayout(optionsGroup);
  debugInfo_ = new QCheckBox(tr("Load debu&g information"), optionsGroup);
  debugInfo_->setObjectName(QStringLiteral("debugInfo"));
  debugInfo_->setChecked(true);
  debugInfo_->setToolTip(tr("Read the PDB, DWARF or linker map that belongs to "
                            "the input"));
  optionsLayout->addWidget(debugInfo_);
  optionsLayout->addStretch();
  groups->addWidget(analysisGroup);
  groups->addWidget(optionsGroup);
  layout->addLayout(groups);

  note_ = new QLabel(this);
  note_->setObjectName(QStringLiteral("note"));
  note_->setWordWrap(true);
  note_->setTextFormat(Qt::PlainText);
  note_->hide();
  layout->addWidget(note_);

  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  buttons->setCenterButtons(true);
  ok_ = buttons->button(QDialogButtonBox::Ok);
  ok_->setObjectName(QStringLiteral("ok"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  connect(loaders_, &QListWidget::currentRowChanged, this,
          &LoadFileDialog::select);
  connect(loaders_, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *item) {
            if (item->flags() & Qt::ItemIsEnabled)
              accept();
          });
  // The first row loading can read is the default, as in IDA.
  for (int index = 0; index < loaders_->count(); ++index)
    if (loaders_->item(index)->flags() & Qt::ItemIsEnabled) {
      loaders_->setCurrentRow(index);
      break;
    }
  if (row() < 0) {
    ok_->setEnabled(false);
    const auto first = rows_.isEmpty() ? QJsonObject() : rows_[0].toObject();
    note_->setText(tr("NeverD cannot load this file: %1")
                       .arg(first.value("reason").toString()));
    note_->show();
  }
}

int LoadFileDialog::row() const {
  const auto *item = loaders_->currentItem();
  return item && (item->flags() & Qt::ItemIsEnabled) ? loaders_->currentRow()
                                                     : -1;
}

LoadOptions LoadFileDialog::options() const {
  LoadOptions options;
  options.debugInfo = debugInfo_->isChecked();
  options.analysis = analysis_->isChecked();
  return options;
}

bool LoadFileDialog::indicator() const { return indicator_->isChecked(); }

void LoadFileDialog::setIndicator(bool shown) { indicator_->setChecked(shown); }

void LoadFileDialog::select(int row) {
  if (row < 0 || row >= rows_.size())
    return;
  const auto chosen = rows_[row].toObject();
  const auto processor = chosen.value("processor").toString();
  processors_->clearSelection();
  for (int family = 0; family < processors_->topLevelItemCount(); ++family) {
    auto *folder = processors_->topLevelItem(family);
    for (int child = 0; child < folder->childCount(); ++child)
      if (folder->child(child)->data(0, Qt::UserRole).toString() == processor) {
        processors_->setCurrentItem(folder->child(child));
        folder->child(child)->setSelected(true);
      }
  }
  ok_->setEnabled(chosen.value("loadable").toBool());
}

} // namespace neverd::gui
