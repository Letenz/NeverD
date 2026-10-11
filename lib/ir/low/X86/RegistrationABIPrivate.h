//===- RegistrationABIPrivate.h - PE32 call frame checks ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H
#define NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H

#include "neverd/ir/low/RegistrationABI.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <utility>

namespace neverd::registration_abi {

struct ImageFrameEffects {
  std::set<std::pair<va_t, va_t>> Reads;
  std::set<std::pair<va_t, va_t>> Writes;
  std::set<std::pair<va_t, va_t>> CallerPCWrites;
  std::set<std::pair<int32_t, int32_t>> ECXReads;
  std::set<std::pair<int32_t, int32_t>> ECXWrites;
  std::set<std::pair<int32_t, int32_t>> CopySourceReads;
};

template <typename Set, typename Vector>
void copyExtents(const Set &From, Vector &To) {
  for (const auto &[Begin, End] : From) {
    if (!To.empty() && Begin <= To.back().End)
      To.back().End = std::max(To.back().End, End);
    else
      To.push_back({Begin, End});
  }
}

std::optional<std::vector<RegistrationRuntimeThrowInfo>>
collectRegistrationRuntimeThrowInfos(const LowFunc &Function,
                                     const BinaryImage &Image, size_t &Work,
                                     ImageFrameEffects &Effects,
                                     std::set<std::pair<va_t, va_t>> &Code);

bool chargeDecodedCallee(size_t &Work, const LowFunc &Function);
bool collectCalleeCodeRanges(const LowFunc &Function, const BinaryImage &Image,
                             size_t &Work,
                             std::vector<ExceptionAddressRange> &Ranges);

bool chargeCalleeWork(size_t &Work, size_t Amount);

struct CleanupFramePrefix {
  unsigned Size = 0;
  uint8_t BaseRegister = 5;
  int32_t BaseOffset = 0;
  bool CallsLeaf = false;
  std::optional<RegistrationCleanupParentFrame> RealignedParent;
};

std::optional<CleanupFramePrefix>
getCleanupFramePrefix(const BinaryImage &Image, va_t Target, size_t &Work);

std::optional<uint16_t> parentPopBytes(const LowFunc &Function,
                                       const BinaryImage &Image);

std::optional<uint32_t>
checkedRegistrationImportStackPop(const BinaryImage &Image, va_t Target);
bool callerPCIsNotReadBack(const ImageFrameEffects &Effects);

struct InitializerFrame {
  uint32_t StackPopBytes = 0;
  std::optional<std::set<int32_t>> Initialized;
};

using InitializerCalls =
    std::map<std::pair<va_t, int>, RegistrationInitializerCalleeABI>;

constexpr unsigned kMaxInitializerGraphDepth = 32;
constexpr size_t kMaxInitializerGraphNodes = 256;

/// A fresh invocation owns this bounded, cycle-rejecting constructor graph.
/// The cache includes failures and never crosses an image or proof budget.
struct InitializerContext {
  std::map<va_t, std::optional<RegistrationInitializerCalleeABI>> Callees;
  unsigned Depth = 0;
};

std::optional<RegistrationInitializerCalleeABI>
checkedInitializerCalleeABI(const BinaryImage &Image, va_t Target, size_t &Work,
                            InitializerContext &Context);

struct PrivateFrameOptions {
  bool BorrowECX = false;
  RegistrationThrowCalleeABI *ThrowProof = nullptr;
  bool *IndependentScalarReturn = nullptr;
  bool DiscardReturnValue = false;
  uint32_t CopyObjectSize = 0;
  InitializerFrame *Initializer = nullptr;
  const InitializerCalls *Initializers = nullptr;
};

bool hasPrivateCallerFrame(const LowFunc &Function, const BinaryImage &Image,
                           size_t &Work, ImageFrameEffects &Effects,
                           const PrivateFrameOptions &Options = {});

std::optional<RegistrationLeafCalleeABI> checkedLeafCalleeABI(
    const BinaryImage &Image, va_t Target, size_t &Work,
    RegistrationLeafResult ResultKind, uint32_t CopyObjectSize = 0,
    std::vector<RegistrationObjectExtent> *CopySource = nullptr,
    InitializerFrame *Initializer = nullptr,
    InitializerContext *Context = nullptr);

} // namespace neverd::registration_abi

#endif // NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H
