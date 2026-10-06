#pragma once

#include "Address.h"

#include <QTimer>
#include <QWidget>
#include <optional>

namespace neverd::gui {

class AddressSpace;
class Session;

/// The navigation band: the whole address space as colored address classes
/// (functions, library code, data, unexplored bytes, externals), the current
/// position, and a legend.  Clicking or dragging navigates.
class NavigationBand final : public QWidget {
  Q_OBJECT
public:
  NavigationBand(Session &session, AddressSpace &space,
                 QWidget *parent = nullptr);

  void setCurrent(std::optional<Address> address);
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

signals:
  void navigateRequested(neverd::gui::Address address);
  /// The address-space map was refreshed.
  void spaceChanged();

protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  bool event(QEvent *event) override;

private:
  void requestOverview();
  QRect bandRect() const;
  std::optional<Address> addressAtX(int x) const;
  Session &session_;
  AddressSpace &space_;
  QByteArray classes_;
  std::optional<Address> current_;
  QTimer refresh_;
  quint64 serial_ = 0;
};

} // namespace neverd::gui
