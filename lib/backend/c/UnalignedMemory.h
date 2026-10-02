//===- UnalignedMemory.h - Scalar source memory spelling --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_UNALIGNEDMEMORY_H
#define NEVERD_BACKEND_C_UNALIGNEDMEMORY_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace neverd::c_memory {

struct Scalar {
  const char *Type;
  const char *Alias;
  unsigned Bytes;
};

inline constexpr Scalar Scalars[] = {
    {"uint8_t", "neverd_unaligned_u8", 1},
    {"int8_t", "neverd_unaligned_i8", 1},
    {"uint16_t", "neverd_unaligned_u16", 2},
    {"int16_t", "neverd_unaligned_i16", 2},
    {"uint32_t", "neverd_unaligned_u32", 4},
    {"int32_t", "neverd_unaligned_i32", 4},
    {"uint64_t", "neverd_unaligned_u64", 8},
    {"int64_t", "neverd_unaligned_i64", 8},
    {"unsigned __int128", "neverd_unaligned_u128", 16},
    {"__int128", "neverd_unaligned_i128", 16},
    {"float", "neverd_unaligned_f32", 4},
    {"double", "neverd_unaligned_f64", 8},
};

inline std::string alias(llvm::StringRef Type) {
  for (const auto &S : Scalars)
    if (Type == S.Type)
      return S.Alias;
  return {};
}

template <typename Stream> inline void writeTypes(Stream &OS) {
  OS << "#ifndef NEVERD_UNALIGNED_SCALARS\n"
        "#define NEVERD_UNALIGNED_SCALARS\n"
        "#if !defined(__clang__) && !defined(__GNUC__)\n"
        "#error \"unaligned pointer output requires Clang or GCC\"\n"
        "#endif\n";
  for (const auto &S : Scalars) {
    OS << "typedef " << S.Type << " " << S.Alias
       << " __attribute__((aligned(1), may_alias));\n"
       << "_Static_assert(sizeof(" << S.Alias << ") == " << S.Bytes
       << " && _Alignof(" << S.Alias
       << ") == 1, \"unaligned scalar layout\");\n";
  }
  OS << "#endif\n\n";
}

inline std::string access(llvm::StringRef Alias, llvm::StringRef Address,
                          bool ReadOnly) {
  return "(*(" + std::string(ReadOnly ? "const " : "") + Alias.str() +
         " *)(uintptr_t)(" + Address.str() + "))";
}

} // namespace neverd::c_memory

#endif
