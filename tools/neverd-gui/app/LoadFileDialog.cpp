#include "LoadFileDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
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
  bool binary;
};
constexpr Processor Processors[] = {
#define NEVERD_PROCESSOR(Family, ShortName, Name, Binary)                      \
  {#Family, ShortName, Name, Binary},
#include "Processors.def"
};

/// The list shows this many rows before it scrolls.
constexpr int ListedRows = 5;
/// Whether a processor list item can read a binary file.
constexpr int BinaryRole = Qt::UserRole + 1;

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
    } else if (row.value("by_name").toBool()) {
      item->setToolTip(tr("Listed for the file's name alone; its contents do "
                          "not show this format"));
    }
  }
  const int shown = std::clamp(loaders_->count(), 1, ListedRows);
  loaders_->setFixedHeight(loaders_->sizeHintForRow(0) * shown +
                           2 * loaders_->frameWidth());
  layout->addWidget(heading);
  layout->addWidget(loaders_);

  // The processor the chosen loader reads the file as.
  processorHeading_ = new QLabel(this);
  processors_ = new QTreeWidget(this);
  processors_->setObjectName(QStringLiteral("processors"));
  processors_->setColumnCount(2);
  processors_->setHeaderHidden(true);
  processorHeading_->setBuddy(processors_);
  for (const auto &family : Families) {
    auto *folder = new QTreeWidgetItem(
        processors_, {QCoreApplication::translate("Processors", family.name)});
    folder->setFlags(folder->flags() & ~Qt::ItemIsSelectable);
    for (const auto &processor : Processors)
      if (QLatin1String(processor.family) == QLatin1String(family.id)) {
        auto *item = new QTreeWidgetItem(
            folder, {QCoreApplication::translate("Processors", processor.name),
                     QString::fromLatin1(processor.shortName)});
        item->setData(0, Qt::UserRole,
                      QString::fromLatin1(processor.shortName));
        item->setData(0, BinaryRole, processor.binary);
      }
  }
  processors_->expandAll();
  processors_->resizeColumnToContents(0);
  layout->addWidget(processorHeading_);
  layout->addWidget(processors_);

  // Where a binary file's bytes map, as IDA's image base and memory
  // organization ask.
  auto *placement = new QFormLayout;
  const auto field = [this](const char *name, const QString &text,
                            const QString &placeholder) {
    auto *edit = new QLineEdit(text, this);
    edit->setObjectName(QLatin1String(name));
    edit->setPlaceholderText(placeholder);
    connect(edit, &QLineEdit::textChanged, this, &LoadFileDialog::update);
    return edit;
  };
  base_ = field("base", QStringLiteral("0x0"), {});
  offset_ = field("offset", QStringLiteral("0x0"), {});
  size_ = field("size", {}, tr("To the end of the file"));
  entry_ = field("entry", {}, tr("The image base"));
  base_->setToolTip(tr("Base address for loading the file"));
  offset_->setToolTip(tr("Where in the file the loaded bytes start"));
  size_->setToolTip(tr("How many bytes to load"));
  entry_->setToolTip(tr("Where execution starts"));
  placement->addRow(tr("Image &base"), base_);
  placement->addRow(tr("File &offset"), offset_);
  placement->addRow(tr("Loading si&ze"), size_);
  placement->addRow(tr("E&ntry point"), entry_);
  // The conventions the code follows, read from the code unless chosen.
  platform_ = new QComboBox(this);
  platform_->setObjectName(QStringLiteral("platform"));
  platform_->setToolTip(
      tr("The platform the code was built for, whose conventions it "
         "follows: how calls pass arguments, which registers they keep, and "
         "the sizes of C types"));
#define NEVERD_PLATFORM(ShortName, Name)                                       \
  platform_->addItem(QCoreApplication::translate("Platforms", Name),           \
                     QStringLiteral(ShortName));
#include "Processors.def"
  placement->addRow(tr("&Platform"), platform_);
  layout->addLayout(placement);

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
            if ((item->flags() & Qt::ItemIsEnabled) && ok_->isEnabled())
              accept();
          });
  connect(processors_, &QTreeWidget::currentItemChanged, this,
          &LoadFileDialog::update);
  // The first row loading can read for the file's contents is the default,
  // as in IDA; a row a loader took for the file's name alone is never chosen
  // for the user.
  for (int index = 0; index < loaders_->count(); ++index)
    if ((loaders_->item(index)->flags() & Qt::ItemIsEnabled) &&
        !rows_[index].toObject().value("by_name").toBool()) {
      loaders_->setCurrentRow(index);
      break;
    }
  select(loaders_->currentRow());
}

int LoadFileDialog::row() const {
  const auto *item = loaders_->currentItem();
  return item && (item->flags() & Qt::ItemIsEnabled) ? loaders_->currentRow()
                                                     : -1;
}

bool LoadFileDialog::binary() const {
  const int chosen = row();
  return chosen >= 0 && rows_[chosen].toObject().value("loader").toString() ==
                            QLatin1String("binary");
}

QString LoadFileDialog::chosenProcessor() const {
  const auto *item = processors_->currentItem();
  return item && item->data(0, BinaryRole).toBool()
             ? item->data(0, Qt::UserRole).toString()
             : QString();
}

std::optional<quint64> LoadFileDialog::number(const QLineEdit *field,
                                              std::optional<quint64> empty) {
  QString text = field->text().trimmed();
  if (text.isEmpty())
    return empty;
  if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
    text = text.mid(2);
  bool ok = false;
  const quint64 value = text.toULongLong(&ok, 16);
  return ok ? std::optional<quint64>(value) : std::nullopt;
}

LoadOptions LoadFileDialog::options() const {
  LoadOptions options;
  options.debugInfo = debugInfo_->isChecked();
  options.analysis = analysis_->isChecked();
  // A row the file's name alone suggests reads the file only when chosen,
  // as a binary file does.
  if (const int chosen = row(); chosen >= 0) {
    const auto picked = rows_[chosen].toObject();
    if (binary() || picked.value("by_name").toBool())
      options.loader = picked.value("loader").toString();
  }
  if (binary()) {
    options.processor = chosenProcessor();
    options.platform = platform_->currentData().toString();
    options.base = number(base_, 0).value_or(0);
    options.offset = number(offset_, 0).value_or(0);
    options.size = number(size_, 0).value_or(0);
    options.entry = number(entry_);
  }
  return options;
}

bool LoadFileDialog::indicator() const { return indicator_->isChecked(); }

void LoadFileDialog::setIndicator(bool shown) { indicator_->setChecked(shown); }

void LoadFileDialog::setBinaryProcessor(const QString &processor) {
  binaryProcessor_ = processor;
  select(loaders_->currentRow());
}

void LoadFileDialog::select(int row) {
  const bool raw = binary();
  // A header names the processor; a binary file's is the user's to pick.
  processorHeading_->setText(raw ? tr("Processor t&ype (double-click to set)")
                                 : tr("Processor t&ype"));
  processors_->setEnabled(raw);
  processors_->setToolTip(raw ? tr("Choose the processor the file's code "
                                   "runs on")
                              : tr("The file's header states the processor"));
  for (auto *field : {base_, offset_, size_, entry_})
    field->setEnabled(raw);
  platform_->setEnabled(raw);
  const QString wanted =
      raw ? binaryProcessor_
          : (row >= 0 && row < rows_.size()
                 ? rows_[row].toObject().value("processor").toString()
                 : QString());
  // Rewriting the tree moves its current item; the OK state follows once,
  // from the finished tree.
  const QSignalBlocker quiet(processors_);
  processors_->clearSelection();
  processors_->setCurrentItem(nullptr);
  for (int family = 0; family < processors_->topLevelItemCount(); ++family) {
    auto *folder = processors_->topLevelItem(family);
    for (int child = 0; child < folder->childCount(); ++child) {
      auto *item = folder->child(child);
      const bool usable = !raw || item->data(0, BinaryRole).toBool();
      item->setFlags(usable ? item->flags() | Qt::ItemIsEnabled
                            : item->flags() & ~Qt::ItemIsEnabled);
      if (!wanted.isEmpty() &&
          item->data(0, Qt::UserRole).toString() == wanted) {
        processors_->setCurrentItem(item);
        item->setSelected(true);
      }
    }
  }
  update();
}

void LoadFileDialog::update() {
  QString note;
  bool acceptable = false;
  const int chosen = row();
  if (chosen < 0) {
    bool loadable = false;
    for (int index = 0; index < loaders_->count(); ++index)
      loadable |= bool(loaders_->item(index)->flags() & Qt::ItemIsEnabled);
    const auto first = rows_.isEmpty() ? QJsonObject() : rows_[0].toObject();
    note = loadable ? tr("Only the file's name suggests a format; choose a row "
                         "to load the file that way")
                    : tr("NeverD cannot load this file: %1")
                          .arg(first.value("reason").toString());
  } else if (binary()) {
    if (chosenProcessor().isEmpty())
      note = tr("Choose the processor the file's code runs on");
    else if (!number(base_, 0))
      note = tr("%1 is not a hexadecimal number").arg(base_->text());
    else if (!number(offset_, 0))
      note = tr("%1 is not a hexadecimal number").arg(offset_->text());
    else if (!number(size_, 0))
      note = tr("%1 is not a hexadecimal number").arg(size_->text());
    else if (!entry_->text().trimmed().isEmpty() && !number(entry_))
      note = tr("%1 is not a hexadecimal number").arg(entry_->text());
    else
      acceptable = true;
  } else {
    acceptable = rows_[chosen].toObject().value("loadable").toBool();
    // A processor the list does not name still shows.
    const auto processor =
        rows_[chosen].toObject().value("processor").toString();
    if (!processor.isEmpty() && !processors_->currentItem())
      note = tr("Processor: %1").arg(processor);
  }
  ok_->setEnabled(acceptable);
  note_->setText(note);
  note_->setVisible(!note.isEmpty());
}

} // namespace neverd::gui
