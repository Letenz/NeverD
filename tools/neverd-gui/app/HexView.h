#pragma once

#include "Address.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QSet>
#include <list>
#include <optional>

namespace neverd::gui {

class AddressSpace;
class Session;

/// The hex dump: sixteen bytes per row across every mapped region, fetched
/// from the worker in cached chunks.  The current item is highlighted and
/// follows the synchronized disassembly view.
class HexView final : public QAbstractScrollArea {
  Q_OBJECT
public:
  HexView(Session &session, const AddressSpace &space,
          QWidget *parent = nullptr);

  /// Show \p address, highlighting \p size bytes.
  void setCurrent(Address address, int size = 1);
  std::optional<Address> currentAddress() const { return current_; }
  /// Map rows again after the regions of the address space changed.
  void addressSpaceChanged();
  /// The byte at \p address once it is loaded and mapped.
  std::optional<quint8> byteAt(Address address) const;

signals:
  void locationChanged(neverd::gui::Address address);

protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void scrollContentsBy(int dx, int dy) override;

private:
  struct Chunk {
    QByteArray data;
    int mapped = 0;
  };
  qint64 totalRows() const;
  /// Row index of \p address (rows restart at each region).
  qint64 rowOf(Address address) const;
  std::optional<Address> addressOfRow(qint64 row) const;
  const Chunk *chunk(Address base) const;
  void request(Address base) const;
  void updateRange();
  void updateMetrics();
  int visibleRows() const;

  Session &session_;
  const AddressSpace &space_;
  mutable QHash<Address, Chunk> chunks_;
  mutable std::list<Address> order_;
  mutable QSet<Address> pending_;
  std::optional<Address> current_;
  int currentSize_ = 1;
  qreal charWidth_ = 8;
  int lineHeight_ = 16, ascent_ = 12;
  quint64 serial_ = 0;
};

} // namespace neverd::gui
