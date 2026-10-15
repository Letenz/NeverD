//===- KernelModelRuntime.h - Checked guest runtime services --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded memory, string operations and debugger formatting for the Windows
/// model.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELMODELRUNTIME_H
#define NEVERD_EMULATION_KERNELMODELRUNTIME_H

#include "neverd/emulation/GuestMemory.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <string>

namespace neverd::emulation {
class KernelModel;
namespace runtime {
#define NEVERD_KERNEL_RUNTIME_LIMIT(Name, Value)                               \
  inline constexpr unsigned Name = Value;
#include "KernelRuntimeLimits.def"
#undef NEVERD_KERNEL_RUNTIME_LIMIT

llvm::Error checkBufferRange(uint64_t Address, uint64_t Size);
llvm::Error writeBytes(GuestMemory &Memory, uint64_t Address, uint64_t Size,
                       uint8_t Value = 0);

using ArgumentReader = llvm::function_ref<llvm::Expected<uint64_t>(unsigned)>;
enum class UnicodeOperation { Copy, Compare, Equal };

llvm::Expected<uint64_t> unicodeOperation(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          UnicodeOperation Operation,
                                          llvm::ArrayRef<uint64_t> Arguments);
/// RtlInitAnsiString and RtlInitString.
llvm::Expected<uint64_t> initializeAnsiString(const KernelModel &Model,
                                              GuestMemory &Memory,
                                              llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> initializeUnicodeStringEx(const KernelModel &Model,
                                                   GuestMemory &Memory,
                                                   llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> copyAnsiString(const KernelModel &Model,
                                        GuestMemory &Memory,
                                        llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> timeToTimeFields(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> timeFieldsToTime(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          llvm::ArrayRef<uint64_t> A);
/// RtlEqualString when \p EqualityOnly is true, otherwise RtlCompareString.
llvm::Expected<uint64_t> compareAnsiStrings(const KernelModel &Model,
                                            GuestMemory &Memory,
                                            llvm::ArrayRef<uint64_t> A,
                                            bool EqualityOnly);
/// RtlAppendUnicodeStringToString when \p CountedSource is true, otherwise
/// RtlAppendUnicodeToString with a null-terminated source.
llvm::Expected<uint64_t> appendUnicodeString(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             llvm::ArrayRef<uint64_t> A,
                                             bool CountedSource);
llvm::Expected<uint64_t> prefixUnicodeString(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> integerToUnicodeString(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> unicodeStringToInteger(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                llvm::ArrayRef<uint64_t> A);

/// C runtime string helpers. \p UnitWidth is 1 for the narrow routines and 2
/// for the wide ones. Each reads caller memory one unit at a time; a run with
/// no terminator stops at the 1 MiB model limit.
///
/// strlen and wcslen; strnlen and wcsnlen when \p A carries a count. Returns
/// the unit count (size_t).
llvm::Expected<uint64_t> cStringLength(const KernelModel &Model,
                                       GuestMemory &Memory,
                                       llvm::ArrayRef<uint64_t> A,
                                       unsigned UnitWidth);
/// strcpy/wcscpy, or strcat/wcscat when Append is true. A third argument
/// selects the counted form. Copy pads to the count; append always terminates.
/// Overlap and invalid storage are rejected before publishing any output.
llvm::Expected<uint64_t> cStringWrite(const KernelModel &Model,
                                      GuestMemory &Memory,
                                      llvm::ArrayRef<uint64_t> A,
                                      unsigned UnitWidth, bool Append);
/// How a comparison reports a mismatch: Normalized is strcmp/strncmp/wcscmp
/// (-1, 0 or 1); Raw is wcsncmp (the raw unit difference); FoldedRaw is the
/// case-insensitive family (ASCII A-Z folded to lowercase, then raw).
enum class CStringCompare { Normalized, Raw, FoldedRaw };
/// strcmp/wcscmp/_stricmp/_wcsicmp; the counted forms when \p A carries a
/// count. Returns an int zero-extended to 32 bits.
llvm::Expected<uint64_t> cStringCompare(const KernelModel &Model,
                                        GuestMemory &Memory,
                                        llvm::ArrayRef<uint64_t> A,
                                        unsigned UnitWidth,
                                        CStringCompare Kind);
/// strchr/wcschr when \p Reverse is false, strrchr/wcsrchr otherwise. The
/// target is truncated to the unit width; the terminator is searched too.
/// Returns the matching guest address or NULL.
llvm::Expected<uint64_t> cStringFindChar(const KernelModel &Model,
                                         GuestMemory &Memory,
                                         llvm::ArrayRef<uint64_t> A,
                                         unsigned UnitWidth, bool Reverse);
/// strstr and wcsstr. An empty needle returns the haystack. Returns the first
/// match's guest address or NULL.
llvm::Expected<uint64_t> cStringFind(const KernelModel &Model,
                                     GuestMemory &Memory,
                                     llvm::ArrayRef<uint64_t> A,
                                     unsigned UnitWidth);
/// strspn/wcsspn when \p Complement is false, wcscspn otherwise. Returns the
/// length (size_t) of the initial run.
llvm::Expected<uint64_t> cStringSpan(const KernelModel &Model,
                                     GuestMemory &Memory,
                                     llvm::ArrayRef<uint64_t> A,
                                     unsigned UnitWidth, bool Complement);
llvm::Expected<std::string> formatDebugMessage(const KernelModel &Model,
                                               GuestMemory &Memory,
                                               uint64_t Format,
                                               unsigned FirstArgument,
                                               ArgumentReader ReadArgument);
} // namespace runtime
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_KERNELMODELRUNTIME_H
