#include "SecondaryTextDelegate.h"

#include <QApplication>
#include <QPainter>
#include <algorithm>
#include <utility>

namespace neverd::gui {
namespace {
// Room between an item's text and its secondary text, and after the latter.
constexpr int SecondaryGap = 16;
constexpr int SecondaryMargin = 8;
// The secondary text reads as a description of its item.
constexpr qreal SecondaryOpacity = 0.6;
} // namespace

SecondaryTextDelegate::SecondaryTextDelegate(std::optional<QFont> font,
                                             QObject *parent)
    : QStyledItemDelegate(parent), font_(std::move(font)) {}

void SecondaryTextDelegate::paint(QPainter *painter,
                                  const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const {
  const QString secondary = index.data(SecondaryTextRole).toString();
  if (secondary.isEmpty()) {
    QStyledItemDelegate::paint(painter, option, index);
    return;
  }
  QStyleOptionViewItem item = option;
  initStyleOption(&item, index);
  const QWidget *widget = item.widget;
  const QStyle *style = widget ? widget->style() : QApplication::style();
  const QFont font = font_.value_or(item.font);
  const int width = QFontMetrics(font).horizontalAdvance(secondary);
  const int right = item.rect.right() - SecondaryMargin;
  // The item's own text elides before it reaches the secondary text.
  const QRect text =
      style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget);
  const int room = std::max(0, right - width - SecondaryGap - text.left());
  item.text =
      QFontMetrics(item.font).elidedText(item.text, item.textElideMode, room);
  style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

  QColor color = item.palette.color(item.state & QStyle::State_Selected
                                        ? QPalette::HighlightedText
                                        : QPalette::Text);
  color.setAlphaF(SecondaryOpacity);
  painter->save();
  painter->setFont(font);
  painter->setPen(color);
  painter->drawText(
      QRect(right - width + 1, item.rect.top(), width, item.rect.height()),
      Qt::AlignRight | Qt::AlignVCenter, secondary);
  painter->restore();
}

} // namespace neverd::gui
