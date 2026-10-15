//===- LoadQuestionDialog.cpp - A question asked before a load ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LoadQuestionDialog.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace {

// The width of the PDB question, which these sit beside.
constexpr int DialogWidth = 480;
/// The dialog's margins, the space between its lines and before its
/// buttons, on the 4-pixel grid.
constexpr int Margin = 16;
constexpr int LineSpacing = 4;
constexpr int GroupSpacing = 8;

QLabel *line(QWidget *parent, const QString &name) {
  auto *label = new QLabel(parent);
  label->setObjectName(name);
  label->setTextFormat(Qt::PlainText);
  return label;
}

} // namespace

LoadQuestionDialog::LoadQuestionDialog(const QString &name, const Text &text,
                                       QWidget *parent)
    : QDialog(parent), wholeDetail_(text.detail), elide_(text.elide) {
  setObjectName(name);
  setWindowTitle(text.title);
  setFixedWidth(DialogWidth);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(Margin, Margin, Margin, Margin);
  layout->setSpacing(LineSpacing);

  auto *question = line(this, QStringLiteral("question"));
  question->setWordWrap(true);
  question->setText(text.question);
  layout->addWidget(question);
  if (!text.detail.isEmpty()) {
    // The detail's ends that fit, and the whole of it in the tooltip.
    detail_ = line(this, text.detailName);
    detail_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    detail_->setProperty("statusLine", true);
    detail_->setToolTip(text.detailToolTip.isEmpty() ? text.detail
                                                     : text.detailToolTip);
    layout->addWidget(detail_);
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

  fitHeight();
  // The detail line shows from the first paint, before any resize.
  elideDetail();
}

void LoadQuestionDialog::fitHeight() {
  // The lines reach the layout's size once its requests are handled.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
  layout()->activate();
  setFixedHeight(heightForWidth(width()));
}

void LoadQuestionDialog::showEvent(QShowEvent *event) {
  // The style sheet gives the detail line its font as the dialog polishes,
  // after the constructor measured it: a Chinese or Japanese detail then
  // needed a pixel more than the dialog had.
  fitHeight();
  QDialog::showEvent(event);
}

void LoadQuestionDialog::resizeEvent(QResizeEvent *event) {
  QDialog::resizeEvent(event);
  elideDetail();
}

void LoadQuestionDialog::elideDetail() {
  if (!detail_)
    return;
  const QFontMetrics metrics(detail_->font());
  detail_->setText(metrics.elidedText(
      wholeDetail_, elide_, std::max(0, detail_->contentsRect().width())));
}

} // namespace neverd::gui
