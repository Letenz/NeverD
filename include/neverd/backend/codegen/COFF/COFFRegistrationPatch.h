//===- COFFRegistrationPatch.h - PE32 registration contract install -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFREGISTRATIONPATCH_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFREGISTRATIONPATCH_H

#include "neverd/Common.h"
#include "neverd/loader/ExceptionCommon.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <map>
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

/// Resolve only the checked PE32 load-config cookie or the CRT wrapper's
/// exact fastcall checker. Conflicting checker identities cannot share a
/// symbol.
std::optional<va_t> findCOFFRegistrationRuntimeVA(const BinaryImage &Image,
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

/// Replay the C++ source execution graph and authenticate edited catch,
/// cleanup, invoke and resumption edges. Frame effects and native installation
/// require their separate validators.
llvm::Error
validateCOFFRegistrationCxxControlIR(const llvm::Function &Function,
                                     const ExceptionFunction &Source,
                                     const BinaryImage &Image);

/// Authenticate source control, actual LLVM frame/object borrows, definite
/// initialization, runtime reference identities and original image effects.
/// Generated tables, the compiler handler and final PE still need validation.
llvm::Error validateCOFFRegistrationCxxIR(const llvm::Function &Function,
                                          const ExceptionFunction &Source,
                                          const BinaryImage &Image);

/// Checked PE32 C++ compiler table closure. This receipt authenticates raw
/// language-table bytes and absolute pointer fields, not edited IR effects or
/// permission to install the generated function.
struct COFFRegistrationCxxTableReceipt {
  va_t OwnerVA = 0;
  va_t FuncInfoVA = 0;
  std::array<std::pair<va_t, va_t>, 3> Tables{};
  std::map<int32_t, int32_t> SourceToGeneratedStates;
  std::vector<va_t> AbsolutePointerFields;
};
llvm::Expected<COFFRegistrationCxxTableReceipt>
getCheckedCOFFRegistrationCxxTableReceipt(const llvm::Function &Function,
                                          const ExceptionFunction &Source,
                                          const BinaryImage &Image,
                                          const CompiledImage &Compiled);

/// Checked compiler-created registration handler and its raw FuncInfo load,
/// exact runtime branch, parent pointer fixups and SafeSEH symbol-index row.
/// This receipt does not replace the independent source IR proof.
struct COFFRegistrationCxxHandlerReceipt {
  COFFRegistrationCxxTableReceipt Tables;
  ExceptionAddressRange CodeRange;
  std::vector<va_t> AbsolutePointerFields;
};
llvm::Expected<COFFRegistrationCxxHandlerReceipt>
getCheckedCOFFRegistrationCxxHandlerReceipt(const llvm::Function &Function,
                                            const ExceptionFunction &Source,
                                            const BinaryImage &Image,
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
