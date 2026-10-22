//===- ViewSearch.cpp - Literal view search ------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared literal matching for the find dialog and both text renderers.
//===----------------------------------------------------------------------===//

#include "ViewSearch.h"

namespace neverd::gui {

QVector<int> ViewSearchQuery::matches(QStringView text) const {
  QVector<int> result;
  if (Text.isEmpty())
    return result;
  const auto word = [](QChar c) {
    return c.isLetterOrNumber() || c.isMark() || c == u'_';
  };
  for (qsizetype at = text.indexOf(Text, 0, Case); at >= 0;
       at = text.indexOf(Text, at + Text.size(), Case)) {
    const auto end = at + Text.size();
    if (!WholeWords || ((at == 0 || !word(text[at - 1])) &&
                        (end == text.size() || !word(text[end]))))
      result.append(int(at));
  }
  return result;
}

} // namespace neverd::gui
