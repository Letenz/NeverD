//===- ElidedLabel.h - Bounded titles with complete text --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared single-line headings retain full accessible text and tooltips while
/// fitting the available width across window and font changes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_ELIDEDLABEL_H
#define NEVERD_GUI_APP_ELIDEDLABEL_H

#include <QChar>
#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QString>
#include <initializer_list>

namespace neverd::gui {
class ElidedLabel final : public QLabel {
public:
  explicit ElidedLabel(QWidget *Parent) : QLabel(Parent) {
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
    // Width elision alone leaves explicit line/paragraph breaks intact.
    // Normalize only the displayed copy; metadata and accessible text stay
    // exact.
    auto Display = FullText;
    for (const auto Break :
         {QChar('\r'), QChar('\n'), QChar(QChar::LineSeparator),
          QChar(QChar::ParagraphSeparator)})
      Display.replace(Break, QLatin1Char(' '));
    setText(fontMetrics().elidedText(Display, Qt::ElideMiddle,
                                     contentsRect().width()));
  }
};
} // namespace neverd::gui

#endif // NEVERD_GUI_APP_ELIDEDLABEL_H
