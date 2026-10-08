//===- COFFRegistrationPatch.h - PE32 registration contract install -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFREGISTRATIONPATCH_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFREGISTRATIONPATCH_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace llvm {
class Function;
class Module;
} // namespace llvm
namespace neverd {
struct BinaryImage;
struct CompiledImage;
struct ExceptionFunction;
struct COFFGuardTableUpdate;

/// Resolve the compiler cookie only to the checked PE32 load-config storage.
std::optional<va_t>
findCOFFRegistrationSecurityCookieVA(const BinaryImage &Image,
                                     llvm::StringRef Symbol);

struct COFFRegistrationPatchUpdate {
  bool Apply = false;
  uint32_t ImageBase = 0;
  uint32_t SectionRVA = 0;
  uint32_t SectionSize = 0;
  std::array<uint8_t, 32> SectionSHA256{};
  uint32_t RelocationRVA = 0;
  uint32_t RelocationSize = 0;
  uint32_t LoadConfigRVA = 0;
  uint32_t LoadConfigSize = 0;
  uint32_t LoadConfigDeclaredSize = 0;
  /// Exact SafeSEH pointer/count fields; other guard fields have their own
  /// shared transaction and must not be overwritten from a stale snapshot.
  std::vector<uint8_t> LoadConfigBytes;
  uint16_t DllCharacteristics = 0;
  uint16_t FileCharacteristics = 0;
  std::vector<uint32_t> SafeSEHHandlers;
};

llvm::Error validateCOFFRegistrationIR(const llvm::Function &Function,
                                       const ExceptionFunction &Source,
                                       const BinaryImage &Image);
llvm::Error
validateCOFFRegistrationSemanticRows(const llvm::Function &Function,
                                     const ExceptionFunction &Source,
                                     const CompiledImage &Compiled);
llvm::Expected<COFFRegistrationPatchUpdate> prepareCOFFRegistrationPatch(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, const llvm::Module &RewriteModule,
    const COFFGuardTableUpdate *GuardUpdate = nullptr);
llvm::Error
applyCOFFRegistrationPatch(std::vector<uint8_t> &Binary,
                           const COFFRegistrationPatchUpdate &Update);
llvm::Error
validateCOFFRegistrationPatch(llvm::ArrayRef<uint8_t> Binary,
                              const COFFRegistrationPatchUpdate &Update);
} // namespace neverd
#endif
