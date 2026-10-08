//===- ProcessImports.cpp - Export addresses returned by guest helpers ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessImports.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <tuple>

namespace neverd::unpack {
using namespace emulation;
namespace {
constexpr CPURegister Registers[] = {
#define NEVERD_SCALAR_REGISTER(ISA, Name, Width, Backend) REGISTER_##ISA(Name)
#define NEVERD_EXTENDED_REGISTER NEVERD_SCALAR_REGISTER
#define NEVERD_VECTOR_REGISTER(ISA, Index, Backend) REGISTER_##ISA(V##Index)
#define REGISTER_X64(Name) CPURegister::X64##Name,
#define REGISTER_AArch64(Name)
#include "neverd/emulation/Registers.def"
#undef REGISTER_AArch64
#undef REGISTER_X64
#undef NEVERD_VECTOR_REGISTER
#undef NEVERD_EXTENDED_REGISTER
#undef NEVERD_SCALAR_REGISTER
};
constexpr CPURegister Destinations[] = {
    CPURegister::X64AX,  CPURegister::X64CX,  CPURegister::X64DX,
    CPURegister::X64BX,  CPURegister::X64SP,  CPURegister::X64BP,
    CPURegister::X64SI,  CPURegister::X64DI,  CPURegister::X64R8,
    CPURegister::X64R9,  CPURegister::X64R10, CPURegister::X64R11,
    CPURegister::X64R12, CPURegister::X64R13, CPURegister::X64R14,
    CPURegister::X64R15};
size_t registerIndex(CPURegister R) {
  return llvm::find(Registers, R) - std::begin(Registers);
}
} // namespace

llvm::Expected<std::vector<ExecutionWatch>>
ExportObserver::started(ProcessView &Process) {
  Watches.clear();
  const auto Input = Process.inputModule();
  auto Mappings = Process.mappings();
  if (!Mappings)
    return Mappings.takeError();
  const auto Modules = Process.modules();
  for (const auto &Export : Process.exports()) {
    const uint64_t PC = Export.Address;
    if (Input && PC >= Input->Base && PC - Input->Base < Input->Size)
      continue;
    // Modeled providers already publish exporting() before service dispatch.
    // Guest dependencies need an execution stop at their actual code export.
    if (!llvm::any_of(Modules,
                      [&](const auto &M) {
                        return !M.Modeled && PC >= M.Base &&
                               PC - M.Base < M.Size;
                      }) ||
        !llvm::any_of(*Mappings, [&](const auto &M) {
          return !M.Device && (M.Permissions & Execute) && PC >= M.Address &&
                 PC - M.Address < M.Size;
        }))
      continue;
    Watches.push_back({PC, 1});
  }
  llvm::sort(Watches, [](const auto &A, const auto &B) {
    return A.Address < B.Address;
  });
  Watches.erase(std::unique(Watches.begin(), Watches.end(),
                            [](const auto &A, const auto &B) {
                              return A.Address == B.Address;
                            }),
                Watches.end());
  return Watches;
}

llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
ExportObserver::invoking(ProcessView &Process) {
  auto Next = started(Process);
  if (!Next)
    return Next.takeError();
  return std::move(*Next);
}

llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
ExportObserver::watched(ProcessView &Process, uint64_t PC) {
  auto Frame = Process.callFrame();
  if (!Frame)
    return Frame.takeError();
  if (*Frame)
    for (const auto &Export : Process.exports()) {
      if (Export.Address != PC)
        continue;
      if (auto E = exporting(Process, Export, (**Frame).ReturnAddress))
        return std::move(E);
      break;
    }
  return Watches;
}

llvm::Error ExportObserver::exporting(ProcessView &,
                                      const ProcessExportView &Export,
                                      std::optional<uint64_t> ReturnAddress) {
  if (!ReturnAddress)
    return llvm::Error::success();
  if (Calls.size() == defaults::Imports)
    return failure(text::ImportLimit);
  Calls.push_back({*ReturnAddress, Export.Address});
  return llvm::Error::success();
}

void ImportObserver::reject(size_t Index) {
  auto &C = Candidates[Index];
  C.Rejected = true;
  llvm::erase_if(Imports, [&](const auto &I) {
    return I.InstructionAddress == Base + C.RVA;
  });
}
void ImportObserver::rejectActive() {
  if (Active)
    reject(Active->Candidate);
  Active.reset();
}
std::vector<TailImport> ImportObserver::takeImports() {
  // A final invocation that never reached its continuation invalidates any
  // earlier proof for that site as well.
  rejectActive();
  return std::move(Imports);
}

llvm::Expected<std::vector<RegisterValue>>
ImportObserver::registers(ProcessView &Process) {
  std::vector<RegisterValue> Result;
  for (auto R : Registers) {
    auto V = Process.readRegister(R);
    if (!V)
      return V.takeError();
    Result.push_back(*V);
  }
  return Result;
}

llvm::Expected<bool> ImportObserver::unchanged(ProcessView &Process,
                                               const Pending &Before) {
  auto Layout = Process.mappings();
  if (!Layout)
    return Layout.takeError();
  if (Layout->size() != Before.Layout.size() ||
      !std::equal(Layout->begin(), Layout->end(), Before.Layout.begin(),
                  [](const auto &A, const auto &B) {
                    return std::tie(A.Address, A.Size, A.Permissions,
                                    A.Device) ==
                           std::tie(B.Address, B.Size, B.Permissions, B.Device);
                  }))
    return false;
  for (const auto &Region : Before.Memory) {
    std::vector<uint8_t> Bytes(Region.Bytes.size());
    if (auto E = Process.read(Region.Address, Bytes))
      return std::move(E);
    if (Bytes != Region.Bytes)
      return false;
  }
  return true;
}

llvm::Expected<std::vector<ExecutionWatch>>
ImportObserver::started(ProcessView &Process) {
  if (Process.architecture() != GuestArchitecture::X64)
    return Watches;
  const auto Main = Process.inputModule();
  if (!Main)
    return Watches;
  Base = Main->Base;
  Initialized = true;
  Code.assign(Main->Size, 0);
  auto &Memory = Code;
  auto Layout = Process.mappings();
  if (!Layout)
    return Layout.takeError();
  for (const auto &M : *Layout) {
    if (M.Device || !(M.Permissions & Execute) ||
        M.Address >= Base + Main->Size || M.Address + M.Size <= Base)
      continue;
    const uint64_t Begin = std::max(Base, M.Address) - Base;
    const uint64_t End = std::min(Base + Main->Size, M.Address + M.Size) - Base;
    if (auto E = Process.read(Base + Begin, llvm::MutableArrayRef(Memory).slice(
                                                Begin, End - Begin)))
      return std::move(E);
  }
  // Start from actual export-call continuations rather than scanning the
  // protector's entire executable image. Overlapping windows are merged.
  std::vector<std::pair<uint64_t, uint64_t>> Windows;
  for (uint64_t PC : Continuations) {
    if (PC < Base || PC - Base > Main->Size)
      continue;
    const uint64_t End = PC - Base;
    Windows.emplace_back(
        End > value::ImportLookbackBytes ? End - value::ImportLookbackBytes : 0,
        End);
  }
  llvm::sort(Windows);
  uint64_t Scanned = 0;
  for (auto [Begin, End] : Windows) {
    Begin = std::max(Begin, Scanned);
    for (uint64_t RVA = Begin; RVA + 6 <= End; ++RVA) {
      auto Call = x64::importHelper(Memory, RVA);
      if (!Call)
        continue;
      if (Candidates.size() == value::MaxImportHelperSites)
        return failure(text::ImportHelperLimit);
      const unsigned Capacity =
          std::min<uint64_t>(x64::MaxAddressLoadSize, Memory.size() - RVA);
      Candidate C{RVA, Capacity, *Call};
      std::copy_n(Memory.data() + RVA, Capacity, C.Bytes.begin());
      Candidates.push_back(C);
      Watches.push_back({Base + RVA, 1});
      for (unsigned Size = x64::MinAddressLoadSize; Size <= Capacity; ++Size)
        Watches.push_back({Base + RVA + Size, 1});
    }
    // A later overlapping window may supply the continuation for a start
    // within the last five bytes of this window.
    if (End >= 5)
      Scanned = std::max(Scanned, End - 5);
  }
  llvm::sort(Gates);
  Gates.erase(std::unique(Gates.begin(), Gates.end()), Gates.end());
  // A discovered entry may only become bound after program invocation. The
  // address seeds a watch; only its live identity can authorize a repair.
  if (!Candidates.empty())
    for (uint64_t Gate : Gates)
      Watches.push_back({Gate, 1});

  return Watches;
}

llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
ImportObserver::invoking(ProcessView &Process) {
  rejectActive();
  if (Initialized)
    return std::nullopt;
  auto Initial = started(Process);
  if (!Initial)
    return Initial.takeError();
  return std::move(*Initial);
}

llvm::Error ImportObserver::complete(ProcessView &Process, uint64_t PC) {
  auto &C = Candidates[Active->Candidate];
  auto After = registers(Process);
  if (!After)
    return After.takeError();
  const bool Call = llvm::is_contained(Gates, PC);
  uint64_t ReturnPC = PC, Gate = 0;
  std::optional<unsigned> Destination;
  bool Pure = Process.nativeCallCount() == Active->NativeCalls;
  const auto SPIndex = registerIndex(CPURegister::X64SP);
  if (Call) {
    const uint64_t BeforeSP = Active->Registers[SPIndex][0];
    Pure &= BeforeSP >= 8 && (*After)[SPIndex][0] == BeforeSP - 8;
    if (Pure) {
      std::array<uint8_t, 8> Return{};
      if (auto E = Process.read(BeforeSP - 8, Return))
        return E;
      ReturnPC = llvm::support::endian::read64le(Return.data());
    }
    Gate = PC;
  }
  for (size_t I = 0; I < std::size(Registers); ++I) {
    if (Registers[I] == CPURegister::X64PC ||
        (Call && Registers[I] == CPURegister::X64SP) ||
        (*After)[I] == Active->Registers[I])
      continue;
    const auto R = llvm::find(Destinations, Registers[I]);
    if (Call || R == std::end(Destinations) ||
        Registers[I] == CPURegister::X64SP || Destination) {
      Pure = false;
      continue;
    }
    Destination = unsigned(R - std::begin(Destinations));
  }
  if (!Call) {
    if (C.Proven && C.Proven->Register) {
      if (Destination && Destination != C.Proven->Register)
        Pure = false;
      Destination = C.Proven->Register;
    }
    if (!Destination)
      Pure = false;
    else
      Gate = (*After)[registerIndex(Destinations[*Destination])][0];
  }
  std::optional<x64::ImportSite> Site;
  if (Pure && ReturnPC >= Base + C.RVA &&
      ReturnPC - (Base + C.RVA) <= C.Capacity)
    Site = x64::importSite(Code, ReturnPC - Base, !Call, C.RVA, Destination);
  std::optional<ExportBinding> Target;
  if (Pure && Site)
    for (auto &Export : Process.exports()) {
      if (Export.Address != Gate)
        continue;
      ExportBinding Binding{std::move(Export.Module), std::move(Export.Name),
                            Export.Ordinal};
      if (!Target || Binding < *Target)
        Target = std::move(Binding);
    }
  Pure &= bool(Site) && bool(Target);
  if (Pure && C.Proven)
    Pure = C.Proven->Size == Site->Size && C.Proven->Register == Site->Register;
  if (Pure) {
    auto SameMemory = unchanged(Process, *Active);
    if (!SameMemory)
      return SameMemory.takeError();
    Pure = *SameMemory;
  }
  if (Pure) {
    C.Proven = *Site;
    if (!llvm::any_of(Imports, [&](const auto &I) {
          return I.ReturnAddress == ReturnPC && I.Target == *Target &&
                 I.InstructionAddress == Base + C.RVA;
        })) {
      if (Imports.size() == defaults::Imports)
        return failure(text::ImportLimit);
      Imports.push_back(
          {ReturnPC, std::move(*Target), !Call, Base + C.RVA, Destination});
    }
  } else
    reject(Active->Candidate);
  Active.reset();
  return llvm::Error::success();
}
llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
ImportObserver::watched(ProcessView &Process, uint64_t PC) {
  if (Active) {
    const auto &C = Candidates[Active->Candidate];
    const uint64_t Start = Base + C.RVA;
    // A PUSH/POP followed by CALL contains two executed instructions. The
    // CALL's own candidate is a suffix, not a nested helper invocation.
    if (C.CallOffset && PC == Start + C.CallOffset)
      return Watches;
    if (llvm::is_contained(Gates, PC) ||
        (PC >= Start + x64::MinAddressLoadSize && PC - Start <= C.Capacity)) {
      if (auto E = complete(Process, PC))
        return std::move(E);
    } else
      rejectActive();
  }
  for (size_t I = 0; I < Candidates.size(); ++I) {
    const auto &C = Candidates[I];
    if (PC != Base + C.RVA)
      continue;
    // Only one active memory snapshot is retained. A nested candidate
    // invalidates its parent's proof rather than multiplying memory use.
    rejectActive();
    if (C.Rejected)
      break;
    std::array<uint8_t, x64::MaxAddressLoadSize> Bytes{};
    if (auto E = Process.read(
            PC, llvm::MutableArrayRef(Bytes).take_front(C.Capacity))) {
      reject(I);
      return std::move(E);
    }
    if (Bytes != C.Bytes) {
      reject(I);
      break;
    }
    auto Before = registers(Process);
    if (!Before) {
      reject(I);
      return Before.takeError();
    }
    auto Layout = Process.mappings();
    if (!Layout) {
      reject(I);
      return Layout.takeError();
    }
    if (llvm::any_of(*Layout, [](const auto &M) { return M.Device; })) {
      reject(I);
      break;
    }
    const auto Stack = Process.stack();
    const auto NativeCalls = Process.nativeCallCount();
    if (!Stack || !NativeCalls) {
      reject(I);
      break;
    }
    Pending P{I, *NativeCalls, std::move(*Before), std::move(*Layout), {}};
    const uint64_t SP = P.Registers[registerIndex(CPURegister::X64SP)][0];
    if (SP < Stack->Base || SP - Stack->Base >= Stack->Size) {
      reject(I);
      break;
    }
    auto Save = [&](uint64_t Begin, uint64_t End) -> llvm::Error {
      if (Begin == End)
        return llvm::Error::success();
      Region R{Begin, std::vector<uint8_t>(End - Begin)};
      if (auto E = Process.read(Begin, R.Bytes))
        return E;
      P.Memory.push_back(std::move(R));
      return llvm::Error::success();
    };
    for (const auto &M : P.Layout) {
      if (!(M.Permissions & Write))
        continue;
      const uint64_t End = M.Address + M.Size;
      const uint64_t ScratchBegin = std::max(M.Address, Stack->Base);
      const uint64_t ScratchEnd = std::min(End, SP);
      if (ScratchBegin < ScratchEnd) {
        if (auto E = Save(M.Address, ScratchBegin)) {
          reject(I);
          return std::move(E);
        }
        if (auto E = Save(ScratchEnd, End)) {
          reject(I);
          return std::move(E);
        }
      } else if (auto E = Save(M.Address, End)) {
        reject(I);
        return std::move(E);
      }
    }
    Active = std::move(P);
    break;
  }
  return Watches;
}
} // namespace neverd::unpack
