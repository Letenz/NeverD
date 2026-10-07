#pragma once

namespace neverd::gui {

// Kept apart from ChooserView.h: lupdate loses class scopes that follow an
// #include inside a declaration in the same header.
enum class ChooserKind : int {
#define NEVERD_CHOOSER(Id, Operation, Title, Icon, Filterable) Id,
#include "Choosers.def"
  Count
};

} // namespace neverd::gui
