//===- ComparisonSourceView.cpp - Paired source inspection ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Displays ordinary source projections without inventing source-line matches.
//===----------------------------------------------------------------------===//

#include "ComparisonSourceView.h"

#include "../ResourceLimits.h"
#include "ComparisonSession.h"
#include "Theme.h"
#include "TypeSourceHighlighter.h"

#include "neverd/diff/Limits.h"

#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace neverd::gui {
namespace {
constexpr int PageLines = limits::gui::kCodeViewPageLines;
// A committed source line must not activate the containing dialog's default
// button (which can open an input file chooser).
class SourceLineSelector final : public QSpinBox {
public:
  using QSpinBox::QSpinBox;

protected:
  void keyPressEvent(QKeyEvent *Event) override {
    QSpinBox::keyPressEvent(Event);
    if (Event->key() == Qt::Key_Return || Event->key() == Qt::Key_Enter)
      Event->accept();
  }
};

class SourceTitle final : public QLabel {
public:
  explicit SourceTitle(QWidget *Parent) : QLabel(Parent) {
    setTextFormat(Qt::PlainText);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setMinimumWidth(0);
  }
  void setFullText(const QString &Text) {
    FullText = Text;
    setToolTip(Text);
    setAccessibleName(Text);
    updateText();
  }

protected:
  void resizeEvent(QResizeEvent *Event) override {
    QLabel::resizeEvent(Event);
    updateText();
  }
  void changeEvent(QEvent *Event) override {
    QLabel::changeEvent(Event);
    if (Event->type() == QEvent::FontChange)
      updateText();
  }

private:
  QString FullText;
  void updateText() {
    setText(fontMetrics().elidedText(FullText, Qt::ElideMiddle,
                                     contentsRect().width()));
  }
};
QLabel *label(QWidget *Parent, const QString &Text = {}) {
  auto *L = new QLabel(Text, Parent);
  L->setTextFormat(Qt::PlainText);
  L->setWordWrap(true);
  L->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return L;
}
} // namespace

ComparisonSourceView::ComparisonSourceView(ComparisonSession &S,
                                           QWidget *Parent)
    : QWidget(Parent), Session(S) {
  setObjectName("comparisonSource");
  auto *Root = new QVBoxLayout(this);
  Root->setContentsMargins(0, 0, 0, 0);
  auto *Toolbar = new QHBoxLayout;
  auto *ViewLabel = new QLabel(tr("View:"), this);
  Representation = new QComboBox(this);
  Representation->setObjectName("comparisonSourceRepresentation");
  Representation->addItem("C", "c");
  Representation->addItem("LLVM C", "llvmc");
  Representation->addItem(tr("Input language"), "source");
  for (const auto &[Name, Key] :
       {std::pair{"C++", "cpp"}, {"Rust", "rust"}, {"Go", "go"}})
    Representation->addItem(Name, Key);
  ViewLabel->setBuddy(Representation);
  auto *Reload = new QPushButton(tr("Reload source"), this);
  Reload->setObjectName("comparisonSourceReload");
  auto *Options = new QToolButton(this);
  Options->setText(tr("Options"));
  Options->setPopupMode(QToolButton::InstantPopup);
  auto *Menu = new QMenu(Options);
  Unlimited = Menu->addAction(tr("Remove instruction allowance"));
  Unlimited->setCheckable(true);
  Options->setMenu(Menu);
  Toolbar->addWidget(ViewLabel);
  Toolbar->addWidget(Representation);
  Toolbar->addStretch();
  Toolbar->addWidget(Reload);
  Toolbar->addWidget(Options);
  Root->addLayout(Toolbar);
  Root->addWidget(
      label(this, tr("Each pane shows its own source lines. Select a "
                     "line to inspect its original instructions.")));
  auto *Split = new QSplitter(this);
  Split->setChildrenCollapsible(false);
  Root->addWidget(Split, 1);
  for (int Side = 0; Side < 2; ++Side) {
    auto &P = Panes[Side];
    auto *Container = new QWidget(Split);
    auto *Layout = new QVBoxLayout(Container);
    Layout->setContentsMargins(0, 0, 0, 0);
    P.Title = new SourceTitle(Container);
    P.Title->setObjectName(Side ? "comparisonSecondarySourceTitle"
                                : "comparisonPrimarySourceTitle");
    Layout->addWidget(P.Title);
    P.Text = new QPlainTextEdit(Container);
    P.Text->setObjectName(Side ? "comparisonSecondarySource"
                               : "comparisonPrimarySource");
    P.Text->setReadOnly(true);
    P.Text->setLayoutDirection(Qt::LeftToRight);
    P.Text->setLineWrapMode(QPlainTextEdit::NoWrap);
    P.Text->setFont(Theme::instance().codeFont());
    P.Text->setAccessibleName(Side ? tr("Secondary source")
                                   : tr("Primary source"));
    P.Highlighter = new TypeSourceHighlighter(P.Text->document());
    P.Highlighter->setLanguage("c");
    Layout->addWidget(P.Text, 1);
    P.Status = label(Container);
    P.Status->setObjectName(Side ? "comparisonSecondarySourceStatus"
                                 : "comparisonPrimarySourceStatus");
    Layout->addWidget(P.Status);
    auto *Paging = new QHBoxLayout;
    P.Previous = new QToolButton(Container);
    P.Previous->setArrowType(isRightToLeft() ? Qt::RightArrow : Qt::LeftArrow);
    P.Previous->setToolTip(tr("Previous source page"));
    P.Previous->setAccessibleName(P.Previous->toolTip());
    P.Next = new QToolButton(Container);
    P.Next->setArrowType(isRightToLeft() ? Qt::LeftArrow : Qt::RightArrow);
    P.Next->setToolTip(tr("Next source page"));
    P.Next->setAccessibleName(P.Next->toolTip());
    P.Line = new SourceLineSelector(Container);
    P.Line->setObjectName(Side ? "comparisonSecondarySourceLine"
                               : "comparisonPrimarySourceLine");
    P.Line->setMinimum(1);
    P.Line->setKeyboardTracking(false);
    P.Line->setToolTip(tr("Go to source line"));
    P.Line->setAccessibleName(P.Line->toolTip());
    Paging->addWidget(P.Previous);
    Paging->addWidget(P.Line, 1);
    Paging->addWidget(P.Next);
    Layout->addLayout(Paging);
    auto *Navigation = new QHBoxLayout;
    P.Anchors = new QComboBox(Container);
    P.Anchors->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    P.Anchors->setMinimumContentsLength(8);
    P.Anchors->setAccessibleName(tr("Original instruction anchors"));
    P.Instruction = new QPushButton(tr("Show instruction"), Container);
    P.Instruction->setObjectName(Side ? "comparisonSecondarySourceInstruction"
                                      : "comparisonPrimarySourceInstruction");
    Navigation->addWidget(P.Anchors, 1);
    Navigation->addWidget(P.Instruction);
    Layout->addLayout(Navigation);
    connect(P.Text, &QPlainTextEdit::cursorPositionChanged, this,
            [this, Side] { cursor(Side); });
    connect(P.Previous, &QToolButton::clicked, this, [this, Side] {
      load(Side, std::max(0, Panes[Side].Offset - PageLines));
    });
    connect(P.Next, &QToolButton::clicked, this, [this, Side] {
      load(Side, Panes[Side].Document.value("next_offset").toInt());
    });
    connect(P.Line, &QSpinBox::editingFinished, this, [this, Side] {
      const int Offset = Panes[Side].Line->value() - 1;
      if (Offset != Panes[Side].Offset)
        load(Side, Offset);
    });
    connect(P.Anchors, &QComboBox::currentIndexChanged, this, [this, Side] {
      auto &P = Panes[Side];
      P.Instruction->setEnabled(!P.Pending &&
                                P.Anchors->currentData().isValid());
    });
    connect(P.Instruction, &QPushButton::clicked, this, [this, Side] {
      emit instructionSelected(Side ? "secondary" : "primary",
                               Panes[Side].Anchors->currentData().toInt());
    });
    connect(&Theme::instance(), &Theme::changed, this, [this, Side] {
      Panes[Side].Text->setFont(Theme::instance().codeFont());
      Panes[Side].Highlighter->rehighlight();
    });
  }
  connect(Representation, &QComboBox::currentIndexChanged, this,
          [this] { reset(); });
  connect(Unlimited, &QAction::toggled, this, [this] { reset(); });
  connect(Reload, &QPushButton::clicked, this, [this] { reset(true); });
  connect(&Session, &ComparisonSession::mappingChanged, this,
          [this] { setFunctions({}, {}); });
  connect(&Session, &ComparisonSession::changed, this, [this] {
    for (int Side = 0; Side < 2; ++Side) {
      updateControls(Side);
      if (Panes[Side].NeedsLoad)
        QTimer::singleShot(0, this, [this, Side] {
          if (Panes[Side].NeedsLoad)
            load(Side);
        });
    }
  });
  reset();
}
void ComparisonSourceView::setFunctions(const QJsonObject &A,
                                        const QJsonObject &B) {
  if (Panes[0].Function == A && Panes[1].Function == B &&
      Revision == Session.revision())
    return;
  Panes[0].Function = A;
  Panes[1].Function = B;
  Revision = Session.revision();
  reset();
}
void ComparisonSourceView::reset(bool Refresh) {
  ++Generation;
  for (int Side = 0; Side < 2; ++Side) {
    auto &P = Panes[Side];
    P.Document = {};
    P.Offset = 0;
    P.Initial = true;
    P.NeedsLoad = true;
    P.Refresh = Refresh;
    P.Text->clear();
    static_cast<SourceTitle *>(P.Title)->setFullText(
        (Side ? tr("Secondary: %1 %2") : tr("Primary: %1 %2"))
            .arg(P.Function.value("name").toString(),
                 P.Function.value("address").toString()));
    P.Status->setText(P.Function.isEmpty()
                          ? tr("No function selected on this side.")
                          : tr("Source is ready to load."));
    cursor(Side);
    updateControls(Side);
    if (isVisible())
      load(Side);
  }
}
void ComparisonSourceView::showEvent(QShowEvent *Event) {
  QWidget::showEvent(Event);
  for (int Side = 0; Side < 2; ++Side)
    if (Panes[Side].NeedsLoad)
      load(Side);
}
void ComparisonSourceView::load(int Side, int Offset) {
  auto &P = Panes[Side];
  if (!isVisible() || P.Pending || P.Function.isEmpty() || !Session.ready() ||
      Session.busy())
    return;
  P.NeedsLoad = false;
  if (!Session.supports("comparison_source_page")) {
    P.Status->setText(tr("This worker does not support source inspection."));
    return;
  }
  P.Pending = true;
  const bool Refresh = std::exchange(P.Refresh, false);
  const auto Serial = Generation;
  const auto ExpectedRevision = Revision;
  P.Status->setText(tr(
      "Preparing source… Cancellation waits for the current analysis step."));
  updateControls(Side);
  Session.request(
      "comparison_source_page",
      {{"side", Side ? "secondary" : "primary"},
       {"address", P.Function.value("address")},
       {"representation", Representation->currentData().toString()},
       {"offset", Offset},
       {"limit", PageLines},
       {"refresh", Refresh},
       {"max_native_instructions",
        Unlimited->isChecked()
            ? 0
            : qint64(limits::diff::kDefaultSourceNativeInstructions)}},
      this, [this, Side, Offset, Serial, ExpectedRevision](const auto &Reply) {
        auto &P = Panes[Side];
        P.Pending = false;
        if (Serial != Generation || ExpectedRevision != Session.revision()) {
          updateControls(Side);
          QTimer::singleShot(0, this, [this, Side] { load(Side); });
          return;
        }
        if (Reply["status"] != QJsonValue("ok")) {
          P.Status->setText(Reply["error"].toObject()["message"].toString());
          updateControls(Side);
          return;
        }
        const auto Result = Reply["payload"].toObject();
        if (Result["availability"] != QJsonValue("available")) {
          const auto State = Result["state"].toString();
          P.Status->setText(
              State == "resource_limited"
                  ? tr("Source exceeds the instruction allowance. Adjust "
                       "Options to inspect this function.")
              : State == "cancelled" || State == "timed_out"
                  ? tr("Source inspection stopped. Reload source to try again.")
                  : tr("Source unavailable: %1")
                        .arg(Result["reason"].toString()));
          P.Document = {};
          P.Text->clear();
          updateControls(Side);
          return;
        }
        P.Document = Result["document"].toObject();
        const int Prelude =
            P.Document.value("prelude").toObject()["lines"].toInt();
        if (P.Initial && Offset == 0 && Prelude > 0 &&
            Prelude < P.Document.value("total_lines").toInt()) {
          P.Initial = false;
          load(Side, Prelude);
          return;
        }
        P.Initial = false;
        P.Offset = Offset;
        P.Highlighter->setLanguage(P.Document.value("dialect").toString("c"));
        P.Text->setPlainText(P.Document.value("text").toString());
        const auto Rows = P.Document.value("rows").toArray();
        P.Status->setText(tr("Lines %1–%2 of %3")
                              .arg(Rows.isEmpty() ? Offset : Offset + 1)
                              .arg(Offset + Rows.size())
                              .arg(P.Document.value("total_lines").toInt()));
        if (!Result["native_analysis_complete"].toBool())
          P.Status->setText(P.Status->text() +
                            tr(" · Native coverage is incomplete"));
        {
          const QSignalBlocker Block(P.Line);
          P.Line->setMaximum(
              std::max(1, P.Document.value("total_lines").toInt()));
          P.Line->setValue(Offset + 1);
        }
        cursor(Side);
        updateControls(Side);
      });
}
void ComparisonSourceView::cursor(int Side) {
  auto &P = Panes[Side];
  P.Anchors->clear();
  const auto Rows = P.Document.value("rows").toArray();
  const int Row = P.Text->textCursor().blockNumber();
  if (Row >= 0 && Row < Rows.size())
    for (const auto &Value :
         Rows[Row].toObject()["comparison_anchors"].toArray()) {
      const auto A = Value.toObject();
      const auto Change = A["native_change"].toString();
      P.Anchors->addItem(A["address"].toString() +
                             (Change.isEmpty() ? QString() : " · " + Change),
                         A["listing_row"].isDouble()
                             ? QVariant(A["listing_row"].toInt())
                             : QVariant());
    }
  if (!P.Anchors->count())
    P.Anchors->addItem(tr("Unmapped line"));
  P.Instruction->setEnabled(!P.Pending && P.Anchors->currentData().isValid());
}
void ComparisonSourceView::updateControls(int Side) {
  auto &P = Panes[Side];
  const bool Ready = !P.Pending && Session.ready() && !Session.busy();
  P.Previous->setEnabled(Ready && P.Offset > 0 && !P.Document.isEmpty());
  P.Next->setEnabled(Ready && P.Document.value("next_offset").isDouble());
  P.Line->setEnabled(Ready && !P.Document.isEmpty());
  P.Anchors->setEnabled(Ready);
  P.Instruction->setEnabled(Ready && P.Anchors->currentData().isValid());
}
} // namespace neverd::gui
