//===- MicrosoftSymbols.h - Load the PDB an open PE lacks ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The PDB an open PE names but loaded without: a project whose PDB was not
/// where it was, or a file opened before anyone was asked.  A copy that has
/// reached a symbol store loads at once; otherwise the user is asked, as the
/// load of a new file asks, and a download goes where a later load finds it.
/// Inspection and application stay in the worker/C API.
//===----------------------------------------------------------------------===//

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QWidget;

namespace neverd::gui {
class PdbPromptDialog;
class Session;

class MicrosoftSymbols final : public QObject {
  Q_OBJECT
public:
  /// Downloads go through \p network, or a network of its own when null.
  MicrosoftSymbols(Session &session, QWidget *window,
                   QNetworkAccessManager *network = nullptr);
  ~MicrosoftSymbols() override;
  /// Also called before the user starts a manual PDB selection.
  void cancel();
  /// Whether an open file that lacks its PDB is offered one; a run nobody
  /// answers is not.
  void setOffers(bool offers);

private:
  void opened();
  bool current(quint64 serial) const;
  /// Ask about \p reference, the worker's pdb_reference for the open file.
  void offer(quint64 serial, const QJsonObject &reference);
  void apply(const QString &path, quint64 serial);

  Session &session_;
  QWidget *window_;
  QNetworkAccessManager *network_;
  QPointer<PdbPromptDialog> prompt_;
  /// The open file's pdb_download, {file,key}, while it is offered.
  QJsonObject request_;
  quint64 serial_ = 0, epoch_ = 0;
  bool offers_ = true;
};
} // namespace neverd::gui
