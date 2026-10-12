#include "ItemViewActivation.h"

#include <QAbstractItemView>
#include <QKeyEvent>

namespace neverd::gui {
namespace {

/// Turns Return and Enter on the view it filters into its activation.
class ReturnActivates : public QObject {
public:
  explicit ReturnActivates(QAbstractItemView &view)
      : QObject(&view), view_(view) {}

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (object != &view_ || event->type() != QEvent::KeyPress)
      return false;
    const auto *key = static_cast<QKeyEvent *>(event);
    // An open editor takes the keys itself; the view sees none of them.
    if ((key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter) ||
        (key->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier)
      return false;
    const QModelIndex current = view_.currentIndex();
    if (!current.isValid())
      return false;
    emit view_.activated(current);
    return true;
  }

private:
  QAbstractItemView &view_;
};

} // namespace

void activateOnReturn(QAbstractItemView &view) {
  view.installEventFilter(new ReturnActivates(view));
}

} // namespace neverd::gui
