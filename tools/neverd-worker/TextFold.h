#pragma once

#include <string>
#include <string_view>

namespace neverd::worker {

/// \p text case-folded, for comparisons that ignore case: ASCII directly,
/// other text by the engine's Unicode simple case folding (neverd_fold_case)
/// so that "ПРИВЕТ" finds "привет".  With an engine that lacks it, only the
/// ASCII letters fold.
std::string foldText(std::string_view text);

} // namespace neverd::worker
