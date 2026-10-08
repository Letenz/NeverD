#include "Theme.h"

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QPalette>
#include <QRegularExpression>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <algorithm>

namespace neverd::gui {
namespace {
struct ColorPair {
  const char *name;
  const char *light;
  const char *dark;
};
constexpr ColorPair ViewColors[] = {
#define NEVERD_THEME_COLOR(Role, Light, Dark) {#Role, Light, Dark},
#include "ThemeColors.def"
};
static_assert(std::size(ViewColors) ==
              static_cast<std::size_t>(ColorRole::Count));
constexpr ColorPair ChromeColors[] = {
#define NEVERD_CHROME_COLOR(Name, Light, Dark) {#Name, Light, Dark},
#include "ChromeColors.def"
};

constexpr int ListingRoleCount = 0
#define NEVERD_LISTING_ROLE(Name, Code) +1
#include "ListingRoles.def"
    ;
constexpr int AddressClassCount = 0
#define NEVERD_ADDRESS_CLASS(Name, Code) +1
#include "AddressClasses.def"
    ;
// Worker protocol codes map to view colors by name; a role without a color is
// a compile error.
constexpr auto listingColors() {
  std::array<ColorRole, ListingRoleCount> map{};
#define NEVERD_LISTING_ROLE(Name, Code) map[Code] = ColorRole::Listing##Name;
#include "ListingRoles.def"
  return map;
}
constexpr auto prefixColors() {
  std::array<ColorRole, AddressClassCount> map{};
#define NEVERD_ADDRESS_CLASS(Name, Code) map[Code] = ColorRole::Prefix##Name;
#include "AddressClasses.def"
  return map;
}
constexpr auto navigationColors() {
  std::array<ColorRole, AddressClassCount> map{};
#define NEVERD_ADDRESS_CLASS(Name, Code) map[Code] = ColorRole::Nav##Name;
#include "AddressClasses.def"
  return map;
}

constexpr char ModeKey[] = "appearance/theme";
constexpr char FontKey[] = "appearance/codeFont";
constexpr char StyleSheetResource[] = ":/neverd/theme.qss";
constexpr int DefaultCodePointSize = 10;
constexpr int MinimumCodePointSize = 6;
constexpr int MaximumCodePointSize = 48;

struct PaletteRole {
  const char *name;
  QPalette::ColorRole role;
};
constexpr PaletteRole PaletteRoles[] = {
    {"Window", QPalette::Window},
    {"WindowText", QPalette::WindowText},
    {"Base", QPalette::Base},
    {"AlternateBase", QPalette::AlternateBase},
    {"Text", QPalette::Text},
    {"Button", QPalette::Button},
    {"ButtonText", QPalette::ButtonText},
    {"Highlight", QPalette::Highlight},
    {"HighlightedText", QPalette::HighlightedText},
    {"ToolTipBase", QPalette::ToolTipBase},
    {"ToolTipText", QPalette::ToolTipText},
    {"PlaceholderText", QPalette::PlaceholderText},
    {"Link", QPalette::Link},
    {"Light", QPalette::Light},
    {"Midlight", QPalette::Midlight},
    {"Mid", QPalette::Mid},
    {"Dark", QPalette::Dark},
    {"Shadow", QPalette::Shadow},
};
} // namespace

Theme &Theme::instance() {
  static Theme theme;
  return theme;
}

Theme::Theme() {
  QSettings settings;
  mode_ = modeFromName(settings.value(ModeKey).toString());
  codeFont_ = defaultCodeFont();
  if (const auto saved = settings.value(FontKey).toString(); !saved.isEmpty()) {
    QFont font;
    if (font.fromString(saved))
      codeFont_ = font;
  }
  loadColors();
}

QString Theme::modeName(Mode mode) {
  return mode == Mode::Light ? QStringLiteral("light") : QStringLiteral("dark");
}

Theme::Mode Theme::modeFromName(const QString &name) {
  return name == QLatin1String("light") ? Mode::Light : Mode::Dark;
}

void Theme::setMode(Mode mode) {
  if (mode == mode_)
    return;
  mode_ = mode;
  QSettings().setValue(ModeKey, modeName(mode));
  apply();
}

QFont Theme::defaultCodeFont() {
  QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  if (QFontDatabase::hasFamily(QStringLiteral("Consolas")))
    font.setFamily(QStringLiteral("Consolas"));
  font.setPointSize(DefaultCodePointSize);
  font.setStyleHint(QFont::Monospace, QFont::PreferAntialias);
  font.setFixedPitch(true);
  return font;
}

void Theme::setCodeFont(const QFont &font) {
  codeFont_ = font;
  QSettings().setValue(FontKey, font.toString());
  emit changed();
}

void Theme::zoomCodeFont(int steps) {
  QFont font = codeFont_;
  font.setPointSize(std::clamp(font.pointSize() + steps, MinimumCodePointSize,
                               MaximumCodePointSize));
  setCodeFont(font);
}

void Theme::resetCodeFontSize() {
  QFont font = codeFont_;
  font.setPointSize(DefaultCodePointSize);
  setCodeFont(font);
}

void Theme::loadColors() {
  const bool darkMode = mode_ == Mode::Dark;
  for (std::size_t i = 0; i < std::size(ViewColors); ++i)
    colors_[i] = QColor(darkMode ? ViewColors[i].dark : ViewColors[i].light);
  chrome_.clear();
  for (const auto &entry : ChromeColors)
    chrome_.insert(QString::fromLatin1(entry.name),
                   QColor(darkMode ? entry.dark : entry.light));
}

void Theme::apply() {
  loadColors();
  auto *application =
      qobject_cast<QApplication *>(QCoreApplication::instance());
  if (!application) {
    emit changed();
    return;
  }
  application->setFont(defaultCodeFont());
  // Fusion renders the palette faithfully on every platform.
  if (application->style()->name().compare("fusion", Qt::CaseInsensitive))
    application->setStyle(QStyleFactory::create("Fusion"));
  QPalette palette;
  for (const auto &entry : PaletteRoles)
    palette.setColor(entry.role,
                     chrome_.value(QString::fromLatin1(entry.name)));
  const QColor disabled = chrome_.value("DisabledText");
  for (const auto role : {QPalette::WindowText, QPalette::Text,
                          QPalette::ButtonText, QPalette::HighlightedText})
    palette.setColor(QPalette::Disabled, role, disabled);
  application->setPalette(palette);

  QFile file(StyleSheetResource);
  if (file.open(QIODevice::ReadOnly)) {
    QString sheet = QString::fromUtf8(file.readAll());
    static const QRegularExpression placeholder(R"(\$\{([A-Za-z]+)\})");
    QString resolved;
    resolved.reserve(sheet.size());
    qsizetype last = 0;
    for (auto it = placeholder.globalMatch(sheet); it.hasNext();) {
      const auto match = it.next();
      resolved += QStringView(sheet).mid(last, match.capturedStart() - last);
      resolved += chrome_.value(match.captured(1)).name();
      last = match.capturedEnd();
    }
    resolved += QStringView(sheet).mid(last);
    application->setStyleSheet(resolved);
  }
  emit changed();
}

QColor Theme::listingRole(int code) const {
  static constexpr auto map = listingColors();
  return color(map[code >= 0 && code < ListingRoleCount ? code : 0]);
}

QColor Theme::prefixColor(int addressClass) const {
  static constexpr auto map = prefixColors();
  return color(
      map[addressClass >= 0 && addressClass < AddressClassCount ? addressClass
                                                                : 0]);
}

QColor Theme::navigationColor(int addressClass) const {
  static constexpr auto map = navigationColors();
  return color(
      map[addressClass >= 0 && addressClass < AddressClassCount ? addressClass
                                                                : 0]);
}

} // namespace neverd::gui
