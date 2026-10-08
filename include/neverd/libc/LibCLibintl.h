#ifndef NEVERD_LIBC_LIBCLIBINTL_H
#define NEVERD_LIBC_LIBCLIBINTL_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// GNU libintl.h — message translation (glibc, GNU gettext).  The `_()`
/// macro of a translated program calls dcgettext(NULL, msgid, LC_MESSAGES).
inline constexpr std::string_view kLibintlHeader = "libintl.h";

inline constexpr std::array kLibintlFunctions = {
    "bind_textdomain_codeset",
    "bindtextdomain",
    "dcgettext",
    "dcngettext",
    "dgettext",
    "dngettext",
    "gettext",
    "ngettext",
    "textdomain",
};

inline constexpr auto kLibintlArity = std::to_array<LibCArityEntry>({
    {"gettext", {1, 0}},
    {"dgettext", {2, 0}},
    {"dcgettext", {3, 0}},
    {"ngettext", {3, 0}},
    {"dngettext", {4, 0}},
    {"dcngettext", {5, 0}},
    {"textdomain", {1, 0}},
    {"bindtextdomain", {2, 0}},
    {"bind_textdomain_codeset", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCLIBINTL_H
