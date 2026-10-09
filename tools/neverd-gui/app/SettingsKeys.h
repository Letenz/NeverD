#pragma once

namespace neverd::gui::settings {

#define NEVERD_SETTINGS_KEY(Name, Key) inline constexpr char Name[] = Key;
#include "SettingsKeys.def"

} // namespace neverd::gui::settings
