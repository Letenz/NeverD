#ifndef NEVERD_LIBC_LIBCDLFCN_H
#define NEVERD_LIBC_LIBCDLFCN_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX dlfcn.h — dynamic linking
inline constexpr std::string_view kDlfcnHeader = "dlfcn.h";

inline constexpr std::array kDlfcnFunctions = {
    "dladdr", "dlclose", "dlerror", "dlinfo", "dlopen", "dlsym",
};

/// Fixed arity of the dlfcn.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kDlfcnArity = std::to_array<LibCArityEntry>({
    {"dladdr", {2, 0}},
    {"dlclose", {1, 0}},
    {"dlerror", {0, 0}},
    {"dlinfo", {3, 0}},
    {"dlopen", {2, 0}},
    {"dlsym", {2, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCDLFCN_H
