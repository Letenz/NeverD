#ifndef NEVERD_LIBC_LIBCSEARCH_H
#define NEVERD_LIBC_LIBCSEARCH_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX search.h — hash/tree/linear search
inline constexpr std::string_view kSearchHeader = "search.h";

inline constexpr std::array kSearchFunctions = {
    "hcreate", "hcreate_r", "hdestroy", "hdestroy_r", "hsearch",  "hsearch_r",
    "insque",  "lfind",     "lsearch",  "remque",     "tdestroy", "tdelete",
    "tfind",   "tsearch",   "twalk",    "twalk_r",
};

/// Fixed arity of the search.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSearchArity = std::to_array<LibCArityEntry>({
    {"hcreate", {1, 0}},
    {"hcreate_r", {2, 0}},
    {"hdestroy", {0, 0}},
    {"hdestroy_r", {1, 0}},
    {"hsearch", {2, 0}},
    {"hsearch_r", {4, 0}},
    {"insque", {2, 0}},
    {"lfind", {5, 0}},
    {"lsearch", {5, 0}},
    {"remque", {1, 0}},
    {"tdestroy", {2, 0}},
    {"tdelete", {3, 0}},
    {"tfind", {3, 0}},
    {"tsearch", {3, 0}},
    {"twalk", {2, 0}},
    {"twalk_r", {3, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSEARCH_H
