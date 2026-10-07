#pragma once

#include <QIcon>
#include <QString>

namespace neverd::gui {

/// Original workbench icon \p name (resources/icons/<name>.svg), rendered
/// from SVG at every requested size and device pixel ratio without relying
/// on an icon-engine plugin.  Unknown names yield a null icon.
QIcon icon(const QString &name);

} // namespace neverd::gui
