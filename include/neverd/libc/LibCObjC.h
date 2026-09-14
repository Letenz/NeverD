#ifndef NEVERD_LIBC_LIBCOBJC_H
#define NEVERD_LIBC_LIBCOBJC_H

#include <array>
#include <string_view>

namespace neverd::libc {

// Public declarations must come from the runtime header: id and association
// policy types cannot be redeclared using the decompiler's scalar carriers.
inline constexpr std::string_view kObjCHeader = "objc/runtime.h";
inline constexpr std::array kObjCFunctions = {"objc_getAssociatedObject",
                                              "objc_setAssociatedObject",
                                              "objc_removeAssociatedObjects",
                                              "objc_loadWeak",
                                              "objc_storeWeak",
                                              "objc_enumerationMutation"};

inline bool objcHasObjectStorageArgument(std::string_view Name) {
  return Name == "objc_loadWeak" || Name == "objc_storeWeak";
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCOBJC_H
