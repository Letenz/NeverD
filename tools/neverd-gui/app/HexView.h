#pragma once

#include "Address.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <list>
#include <optional>

namespace neverd::gui {

class AddressSpace;
class Session;

/// The hex dump: sixteen bytes per row across every mapped region, fetched
/// from the worker in cached chunks.  The current item is highlighted and
/// follows the synchronized disassembly view.  The text column reads the
/// bytes as ASCII or, decoded by the engine, in a chosen text encoding.
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
  /// What the text column shows at \p address once it is loaded: a
  /// character, nothing for a character's later bytes, or a dot.
  std::optional<QString> textAt(Address address) const;
  /// The engine encoding the text column reads, empty for plain ASCII.
  QString textEncoding() const { return textEncoding_; }
  /// Read the text column in \p name, or as ASCII when it is empty; the
  /// choice is kept for later sessions.
  void setTextEncoding(const QString &name);

signals:
  void locationChanged(neverd::gui::Address address);

protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void scrollContentsBy(int dx, int dy) override;
  void contextMenuEvent(QContextMenuEvent *event) override;

private:
  struct Chunk {
    QByteArray data;
    int mapped = 0;
    /// With a text encoding, the text shown at each byte: a character at its
    /// first byte, nothing at the rest, and a dot where none is shown.
    QStringList cells;
  };
  struct TextEncoding {
    QString name, label;
    bool legacy = false;
  };
  qint64 totalRows() const;
  /// Row index of \p address (rows restart at each region).
  qint64 rowOf(Address address) const;
  std::optional<Address> addressOfRow(qint64 row) const;
  /// Where the chunk holding \p address starts: its 4 KiB block, clipped to
  /// the start of the address's region, so that a chunk reads one region.
  Address chunkKey(Address address) const;
  const Chunk *chunk(Address key) const;
  void request(Address key) const;
  void clearChunks();
  void loadEncodings();
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
  QString textEncoding_;
  QVector<TextEncoding> encodings_;
  qreal charWidth_ = 8;
  int lineHeight_ = 16, ascent_ = 12;
  quint64 serial_ = 0;
};

} // namespace neverd::gui
