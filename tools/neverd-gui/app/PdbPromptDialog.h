//===- PdbPromptDialog.h - Load the PDB a PE names ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// "Load PDB file": the question IDA asks as it loads a PE that names a PDB,
/// whether to load the debug information the linker recorded for it.  The
/// dialog says where a matching copy is, or downloads one from the symbol
/// servers into the store a later load searches, and offers to browse for
/// the file when no server has it.
//===----------------------------------------------------------------------===//

#pragma once

#include <QDialog>
#include <QJsonObject>
#include <initializer_list>

class QDialogButtonBox;
class QLabel;
class QNetworkAccessManager;
class QProgressBar;
class QPushButton;

namespace neverd::gui {

class Session;
class SymbolDownload;

class PdbPromptDialog final : public QDialog {
  Q_OBJECT
public:
  /// \p reference is the "pdb" of the worker's pdb_reference for \p input.
  /// Downloads go through \p network, or a network of the dialog's own.
  PdbPromptDialog(Session &session, const QString &input,
                  const QJsonObject &reference,
                  QNetworkAccessManager *network = nullptr,
                  QWidget *parent = nullptr);

  /// The PDB to load, {"path","mode":"verified"}, or {"mode":"none"} once
  /// the user declined it.
  const QJsonObject &choice() const { return choice_; }

  void reject() override;

protected:
  void keyPressEvent(QKeyEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

private:
  /// Where the copy is, or that none is, and the question to answer.
  void ask();
  void download();
  /// Say which server the download asks now.
  void showServer();
  void progress(qint64 received, qint64 total);
  /// Whether the downloaded \p path is the PDB the input names.
  void verify(const QString &path);
  /// Say why no PDB arrived, offering to try again when \p retryable.
  void failed(const QString &message, bool retryable);
  void browse();
  void choose(const QString &path);
  /// Show \p text as the path line, its middle elided to fit.
  void showPath(const QString &text);
  /// Show \p shown, the first of them the default.
  void setButtons(std::initializer_list<QPushButton *> shown);
  /// Take the height the dialog's lines need.
  void fit();

  Session &session_;
  QString input_;
  QJsonObject reference_;
  QJsonObject choice_;
  SymbolDownload *download_;
  QLabel *recorded_, *where_, *path_, *question_, *detail_;
  QProgressBar *progress_;
  QDialogButtonBox *buttons_;
  QPushButton *yes_, *no_, *cancel_, *retry_, *browse_;
  /// Advances when a download stops, so a verification that answers after
  /// it is ignored.
  quint64 attempt_ = 0;
};

} // namespace neverd::gui
