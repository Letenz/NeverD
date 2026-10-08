#pragma once

#include "Address.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QVector>
#include <functional>

class QWidget;

namespace neverd::gui {

class MainWindow;
class Session;

/// Opt-in measurement of interaction on the production workbench, over the
/// opened file: listing scrolling, jumps, pseudocode and graphs, each first
/// over content the worker has not served yet and then once more. A sample
/// ends at the backing-store flush after the view painted the content.
class InteractionMetrics final : public QObject {
public:
  InteractionMetrics(MainWindow &window, Session &session,
                     const QString &output, int timeoutMs, QObject *parent);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  using Done = std::function<void()>;
  using Step = std::function<void(Done)>;
  struct Series {
    QVector<double> ms;
    int missed = 0;
    /// Per-sample results of the jump, graph and pseudocode phases.
    QJsonArray detail;
  };

  void begin();
  void runSteps(QVector<Step> steps);
  Step scrollStep(const char *name, int direction);
  Step jumpStep(const char *name);
  Step pseudocodeStep(const char *name);
  void measurePseudocode(const char *name, Done done);
  Step graphStep(const char *name);
  /// Call \p then at the backing-store flush after \p view next paints.
  void afterPaint(QWidget *view, Done then);
  void record(const char *name, const Series &series);
  void finish(bool success, const QString &reason = {});

  MainWindow &window_;
  Session &session_;
  QString output_;
  int timeoutMs_;
  QElapsedTimer clock_;
  QVector<Address> samples_;
  QJsonObject phases_;
  QPointer<QWidget> paintView_;
  Done paintThen_;
  bool finished_ = false;
};

} // namespace neverd::gui
