//===- DwarfPromptDialog.cpp - Load the DWARF of an image -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// IDA asks "Load DWARF debug information?" under the title "DWARF info
/// found", and names the file in parentheses when the DWARF is not the
/// input's own.  IDA's import options are left out: the engine reads DWARF
/// whole or not at all.
//===----------------------------------------------------------------------===//

#include "DwarfPromptDialog.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontMetrics>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace {

// The width of the PDB question, which this one sits beside.
constexpr int DialogWidth = 480;
/// The dialog's margins, the space between its lines and before its
/// buttons, on the 4-pixel grid.
constexpr int Margin = 16;
constexpr int LineSpacing = 4;
constexpr int GroupSpacing = 8;

/// The location of DWARF read from the input's own sections.
constexpr char InputLocation[] = "input";
/// The whole of the path line, which is elided to fit.
constexpr char WholeText[] = "wholeText";

QLabel *line(QWidget *parent, const char *name) {
  auto *label = new QLabel(parent);
  label->setObjectName(QLatin1String(name));
  label->setTextFormat(Qt::PlainText);
  return label;
}

} // namespace

DwarfPromptDialog::DwarfPromptDialog(const QJsonObject &reference,
                                     QWidget *parent)
    : QDialog(parent) {
  setObjectName(QStringLiteral("dwarfPrompt"));
  setWindowTitle(tr("DWARF info found"));
  setFixedWidth(DialogWidth);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(Margin, Margin, Margin, Margin);
  layout->setSpacing(LineSpacing);

  auto *question = line(this, "question");
  question->setWordWrap(true);
  question->setText(tr("Load DWARF debug information?"));
  layout->addWidget(question);
  if (reference.value("location").toString() != QLatin1String(InputLocation)) {
    // A .dSYM bundle or a .debug file holds it: say which, its middle elided
    // to fit and the whole of it in the tooltip.
    path_ = line(this, "path");
    path_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    path_->setProperty("statusLine", true);
    const QString shown =
        QDir::toNativeSeparators(reference.value("path").toString());
    path_->setProperty(WholeText, tr("(%1)").arg(shown));
    path_->setToolTip(shown);
    layout->addWidget(path_);
  }
  layout->addSpacing(GroupSpacing);

  auto *buttons = new QDialogButtonBox(this);
  buttons->setCenterButtons(true);
  auto *yes = buttons->addButton(QDialogButtonBox::Yes);
  auto *no = buttons->addButton(QDialogButtonBox::No);
  yes->setObjectName(QStringLiteral("yes"));
  no->setObjectName(QStringLiteral("no"));
  yes->setDefault(true);
  yes->setFocus();
  connect(yes, &QPushButton::clicked, this, &QDialog::accept);
  connect(no, &QPushButton::clicked, this, &QDialog::reject);
  layout->addWidget(buttons);

  // The lines reach the layout's size once its requests are handled.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
  layout->activate();
  setFixedHeight(heightForWidth(width()));
  // The path line shows from the first paint, before any resize.
  elidePath();
}

void DwarfPromptDialog::resizeEvent(QResizeEvent *event) {
  QDialog::resizeEvent(event);
  elidePath();
}

void DwarfPromptDialog::elidePath() {
  if (!path_)
    return;
  const QFontMetrics metrics(path_->font());
  path_->setText(
      metrics.elidedText(path_->property(WholeText).toString(), Qt::ElideMiddle,
                         std::max(0, path_->contentsRect().width())));
}

} // namespace neverd::gui
