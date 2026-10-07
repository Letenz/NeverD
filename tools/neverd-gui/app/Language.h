#pragma once

#include <QString>
#include <QVector>

namespace neverd::gui {

struct Language {
  QString code;     ///< Settings and resource spelling, such as "zh-CN".
  QString selfName; ///< The language's own name for menus.
};

/// Bundled interface languages, English first.
const QVector<Language> &languages();
/// The language saved in settings; English until one is chosen.
QString currentLanguage();
/// Install \p code's translations, mirror the layout for right-to-left
/// scripts and remember the choice.  Widgets retranslate on LanguageChange.
void applyLanguage(const QString &code);

} // namespace neverd::gui
