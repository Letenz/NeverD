//===- DarwinSystem.cpp - Fixed Darwin system values and copy phases ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinSystem.h"

#include "DarwinUserMemory.h"

#include "llvm/Support/Endian.h"

#include <type_traits>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
std::optional<ServiceResult> returned(uint64_t Value, bool Error = false) {
  return ServiceResult{Value, Error};
}
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
enum class Observation {
#define NEVERD_DARWIN_SYSTEM_FIELD(Member, Field, Name, Root, Leaf) Member,
#include "DarwinSystemFields.def"
#undef NEVERD_DARWIN_SYSTEM_FIELD
  Page32,
  Page64
};
struct Binding {
  Observation Kind;
  llvm::StringLiteral Name;
  uint32_t Root, Leaf;
};
constexpr Binding Bindings[] = {
#define NEVERD_DARWIN_SYSTEM_FIELD(Member, Field, Name, Root, Leaf)            \
  {Observation::Member, Name, Root, Leaf},
#include "DarwinSystemFields.def"
#undef NEVERD_DARWIN_SYSTEM_FIELD
    {Observation::Page32, "hw.pagesize_compat", 6, 7},
    // This name has a dynamic native OID. Do not invent a numeric binding.
    {Observation::Page64, "hw.pagesize", 0, 0}};
struct Encoded {
  std::vector<uint8_t> Bytes;
  bool Quad = false;
};
template <typename T> Encoded encode(const T &Value) {
  Encoded Out;
  if constexpr (std::is_same_v<T, std::string>) {
    Out.Bytes.assign(Value.begin(), Value.end());
    Out.Bytes.push_back(0);
  } else {
    Out.Bytes.resize(sizeof(T));
    if constexpr (sizeof(T) == 8) {
      llvm::support::endian::write64le(Out.Bytes.data(), Value);
      Out.Quad = true;
    } else {
      llvm::support::endian::write32le(Out.Bytes.data(), Value);
    }
  }
  return Out;
}
std::optional<Encoded>
observation(Observation Kind, uint64_t PageSize,
            const std::optional<DarwinSystemOptions> &Options) {
  if (Kind == Observation::Page32)
    return encode(uint32_t(PageSize));
  if (Kind == Observation::Page64)
    return encode(PageSize);
  if (!Options)
    return std::nullopt;
  switch (Kind) {
#define NEVERD_DARWIN_SYSTEM_FIELD(Member, Field, Name, Root, Leaf)            \
  case Observation::Member:                                                    \
    return Options->Member ? std::optional(encode(*Options->Member))           \
                           : std::nullopt;
#include "DarwinSystemFields.def"
#undef NEVERD_DARWIN_SYSTEM_FIELD
  default:
    return std::nullopt;
  }
}
template <typename T> bool valid(const std::optional<T> &Value) {
  if constexpr (std::is_same_v<T, std::string>)
    return !Value || (Value->size() < SystemStringLimit &&
                      Value->find('\0') == std::string::npos);
  return true;
}
} // namespace

llvm::Error validateSystemOptions(const DarwinSystemOptions &Options) {
#define NEVERD_DARWIN_SYSTEM_FIELD(Member, Field, Name, Root, Leaf)            \
  if (!valid(Options.Member))                                                  \
    return failure(diagnostic::SystemString);
#include "DarwinSystemFields.def"
#undef NEVERD_DARWIN_SYSTEM_FIELD
  if (Options.CPUCount &&
      (!*Options.CPUCount || *Options.CPUCount > uint32_t(INT32_MAX)))
    return failure(diagnostic::SystemCPUCount);
  for (const auto &[Resource, Limit] : Options.ResourceLimits)
    if (Resource >= ResourceLimitCount || Limit.Current > Limit.Maximum ||
        Limit.Maximum > ResourceLimitInfinity)
      return failure(diagnostic::ResourceLimitOption);
  for (const auto *Usage :
       {&Options.ResourceUsageSelf, &Options.ResourceUsageChildren})
    if (*Usage && ((**Usage).UserMicroseconds >= 1000000 ||
                   (**Usage).SystemMicroseconds >= 1000000))
      return failure(diagnostic::ResourceUsageOption);
  return llvm::Error::success();
}

llvm::Expected<std::optional<ServiceResult>>
systemService(GuestMemory &Memory, uint64_t PageSize, ServiceKind Kind,
              const ProcessServiceEvent &Event,
              const std::optional<DarwinSystemOptions> &Options,
              ProcessResult &Result) {
  const auto &A = Event.Arguments;
  if (Kind == ServiceKind::GetRusage) {
    const uint32_t Who = uint32_t(A[0]);
    if (Who != 0 && Who != UINT32_MAX)
      return returned(InvalidArgument, true);
    if (!Options)
      return unsupported(Result, diagnostic::ResourceUsageObservation);
    const auto &Selected =
        Who == 0 ? Options->ResourceUsageSelf : Options->ResourceUsageChildren;
    if (!Selected)
      return unsupported(Result, diagnostic::ResourceUsageObservation);
    const auto &Usage = *Selected;
    std::array<uint8_t, 32 + 8 * DarwinResourceUsage::CounterCount> Bytes{};
    llvm::support::endian::write64le(Bytes.data(), uint64_t(Usage.UserSeconds));
    llvm::support::endian::write32le(Bytes.data() + 8, Usage.UserMicroseconds);
    llvm::support::endian::write64le(Bytes.data() + 16,
                                     uint64_t(Usage.SystemSeconds));
    llvm::support::endian::write32le(Bytes.data() + 24,
                                     Usage.SystemMicroseconds);
    for (size_t I = 0; I != Usage.Counters.size(); ++I)
      llvm::support::endian::write64le(Bytes.data() + 32 + 8 * I,
                                       uint64_t(Usage.Counters[I]));
    return copyUserMemory(Memory, A[1], Bytes,
                          diagnostic::ResourceUsagePartialOutput, Result);
  }
  if (Kind == ServiceKind::GetRlimit) {
    const uint32_t Resource =
        uint32_t(A[0]) & ~uint32_t(ResourceLimitPosixFlag);
    if (Resource >= ResourceLimitCount)
      return returned(InvalidArgument, true);
    if (!Options || !Options->ResourceLimits.contains(Resource))
      return unsupported(Result, diagnostic::ResourceLimitObservation);
    const auto &Limit = Options->ResourceLimits.at(Resource);
    std::array<uint8_t, 16> Bytes;
    llvm::support::endian::write64le(Bytes.data(), Limit.Current);
    llvm::support::endian::write64le(Bytes.data() + 8, Limit.Maximum);
    return copyUserMemory(Memory, A[1], Bytes,
                          diagnostic::ResourceLimitPartialOutput, Result);
  }
  const bool Named = Kind == ServiceKind::SysctlByName;
  const uint64_t Count = Named ? A[1] : uint32_t(A[1]);
  if (Named ? Count >= SystemStringLimit : Count < 2 || Count > SystemMIBLimit)
    return returned(Named ? NameTooLong : InvalidArgument, true);
  std::vector<uint8_t> Input(Named ? Count : Count * 4);
  if (!Input.empty()) {
    auto Prefix = userMemoryPrefix(Memory, A[0], Input.size(), Read);
    if (!Prefix)
      return Prefix.takeError();
    if (!*Prefix)
      return returned(BadAddress, true);
    if (*Prefix != Input.size())
      return unsupported(Result, diagnostic::SystemPartialInput);
    if (auto E = Memory.read(A[0], Input))
      return std::move(E);
  }
  uint64_t Capacity = 0;
  if (A[3]) {
    // Native probes with faulting length pointers did not return within their
    // deadline. Admit only a complete readable/writable size_t before effects.
    auto Prefix = userMemoryPrefix(Memory, A[3], 8, Read | Write);
    if (!Prefix)
      return Prefix.takeError();
    if (*Prefix != 8)
      return unsupported(Result, diagnostic::SystemLengthMemory);
    std::array<uint8_t, 8> Bytes;
    if (auto E = Memory.read(A[3], Bytes))
      return std::move(E);
    Capacity = llvm::support::endian::read64le(Bytes.data());
  }
  const Binding *Selected = nullptr;
  if (Named) {
    // XNU copies every supplied byte before interpreting the first NUL and
    // removing one final dot. Remaining noncanonical names stay unmodeled.
    Input.push_back(0);
    llvm::StringRef Name(reinterpret_cast<const char *>(Input.data()));
    if (Name.empty())
      return returned(NoEntry, true);
    if (Name.ends_with("."))
      Name = Name.drop_back();
    for (const auto &B : Bindings)
      if (Name == B.Name) {
        Selected = &B;
        break;
      }
  } else if (Count == 2) {
    const auto Root = llvm::support::endian::read32le(Input.data());
    const auto Leaf = llvm::support::endian::read32le(Input.data() + 4);
    for (const auto &B : Bindings)
      if (B.Root && B.Root == Root && B.Leaf == Leaf) {
        Selected = &B;
        break;
      }
  }
  if (!Selected)
    return unsupported(Result, diagnostic::SystemKey);
  // A pointer alone is not a write request. All selected nodes reject writes
  // for the fixed non-root identity, including privileged kern.osversion.
  if (A[4] && A[5])
    return returned(OperationNotPermitted, true);
  auto Value = observation(Selected->Kind, PageSize, Options);
  if (!Value)
    return unsupported(Result, diagnostic::SystemObservation);
  if (A[2] && Capacity == 4 && Value->Quad) {
    const auto Bits = llvm::support::endian::read64le(Value->Bytes.data());
    const uint64_t Low = uint32_t(Bits);
    const uint64_t Extended =
        Low & 0x80000000 ? Low | 0xffffffff00000000ULL : Low;
    if (Bits != Extended)
      return returned(ResultTooLarge, true);
    Value->Bytes.resize(4);
  }
  const bool Short = A[2] && Capacity < Value->Bytes.size();
  if (A[2] && !Short) {
    auto Copy = copyUserMemory(Memory, A[2], Value->Bytes,
                               diagnostic::SystemPartialOutput, Result);
    if (!Copy || !*Copy || (**Copy).Error)
      return Copy;
  }
  if (A[3]) {
    std::array<uint8_t, 8> Bytes;
    llvm::support::endian::write64le(Bytes.data(),
                                     Short ? 0 : Value->Bytes.size());
    auto Copy = copyUserMemory(Memory, A[3], Bytes,
                               diagnostic::SystemPartialOutput, Result);
    if (!Copy || !*Copy || (**Copy).Error)
      return Copy;
  }
  return returned(Short ? NoMemory : 0, Short);
}
} // namespace neverd::emulation::darwin_model
