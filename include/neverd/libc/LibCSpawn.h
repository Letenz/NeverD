#ifndef NEVERD_LIBC_LIBCSPAWN_H
#define NEVERD_LIBC_LIBCSPAWN_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX spawn.h — process creation
inline constexpr std::string_view kSpawnHeader = "spawn.h";

inline constexpr std::array kSpawnFunctions = {
    "posix_spawn",
    "posix_spawn_file_actions_addclose",
    "posix_spawn_file_actions_adddup2",
    "posix_spawn_file_actions_addopen",
    "posix_spawn_file_actions_destroy",
    "posix_spawn_file_actions_init",
    "posix_spawnattr_destroy",
    "posix_spawnattr_getflags",
    "posix_spawnattr_getpgroup",
    "posix_spawnattr_getsigdefault",
    "posix_spawnattr_getsigmask",
    "posix_spawnattr_init",
    "posix_spawnattr_setflags",
    "posix_spawnattr_setpgroup",
    "posix_spawnattr_setsigdefault",
    "posix_spawnattr_setsigmask",
    "posix_spawnp",
};

/// Fixed arity of the spawn.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kSpawnArity = std::to_array<LibCArityEntry>({
    {"posix_spawn", {6, 0}},
    {"posix_spawn_file_actions_addclose", {2, 0}},
    {"posix_spawn_file_actions_adddup2", {3, 0}},
    {"posix_spawn_file_actions_addopen", {5, 0}},
    {"posix_spawn_file_actions_destroy", {1, 0}},
    {"posix_spawn_file_actions_init", {1, 0}},
    {"posix_spawnattr_destroy", {1, 0}},
    {"posix_spawnattr_getflags", {2, 0}},
    {"posix_spawnattr_getpgroup", {2, 0}},
    {"posix_spawnattr_getsigdefault", {2, 0}},
    {"posix_spawnattr_getsigmask", {2, 0}},
    {"posix_spawnattr_init", {1, 0}},
    {"posix_spawnattr_setflags", {2, 0}},
    {"posix_spawnattr_setpgroup", {2, 0}},
    {"posix_spawnattr_setsigdefault", {2, 0}},
    {"posix_spawnattr_setsigmask", {2, 0}},
    {"posix_spawnp", {6, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSPAWN_H
