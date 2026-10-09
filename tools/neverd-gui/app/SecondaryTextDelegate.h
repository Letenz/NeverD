#pragma once

#include <QFont>
#include <QStyledItemDelegate>
#include <optional>

namespace neverd::gui {

/// Draws an item's text with a dimmed secondary text, such as an address or
/// a shortcut, at the item's right edge, as quick-pick lists describe their
/// entries.  The secondary text is the item's SecondaryTextRole data.
class SecondaryTextDelegate final : public QStyledItemDelegate {
public:
  static constexpr int SecondaryTextRole = Qt::UserRole + 100;

  /// \p font, when given, is the font the secondary text prints in.
  explicit SecondaryTextDelegate(std::optional<QFont> font = std::nullopt,
                                 QObject *parent = nullptr);

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override;

private:
  std::optional<QFont> font_;
};

} // namespace neverd::gui
