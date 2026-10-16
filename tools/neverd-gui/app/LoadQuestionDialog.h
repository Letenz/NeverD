//===- LoadQuestionDialog.h - A question asked before a load ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The frame of the yes-or-no questions asked before a file loads, as IDA
/// asks "Load DWARF debug information?": the question, an optional line of
/// detail elided to the dialog's width, and Yes and No.
//===----------------------------------------------------------------------===//

#pragma once

#include <QDialog>

class QLabel;

namespace neverd::gui {

class LoadQuestionDialog : public QDialog {
  Q_OBJECT
public:
  /// What a question shows.  \p detail, when not empty, sits below the
  /// question in a label named \p detailName, elided with \p elide and whole
  /// in its tooltip, or \p detailToolTip when that is given.  \p note, when
  /// not empty, follows it word-wrapped in a label named \p noteName, with
  /// \p noteToolTip.  A \p cautious question makes No the default answer.
  struct Text {
    QString title;
    QString question;
    QString detail;
    QString detailName;
    Qt::TextElideMode elide = Qt::ElideMiddle;
    QString detailToolTip;
    QString note;
    QString noteName;
    QString noteToolTip;
    bool cautious = false;
  };

  /// Yes accepts the dialog; No, Escape and closing it reject it.
  LoadQuestionDialog(const QString &name, const Text &text,
                     QWidget *parent = nullptr);

protected:
  void resizeEvent(QResizeEvent *event) override;
  void showEvent(QShowEvent *event) override;

private:
  /// Fix the height to what the lines need at the dialog's width.
  void fitHeight();
  /// Show the detail line elided to the room it has.
  void elideDetail();

  QLabel *detail_ = nullptr;
  QString wholeDetail_;
  Qt::TextElideMode elide_ = Qt::ElideMiddle;
};

} // namespace neverd::gui
