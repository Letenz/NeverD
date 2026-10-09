//===- PEDelayImports.cpp - Delay loading in a fresh process --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "PEImage.h"

#include "neverd/emulation/GuestMemory.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>

namespace neverd::unpack::pe {
namespace {
using namespace llvm::object;
using namespace llvm::support::endian;

struct Range : DelayImportRange {
  bool Written;
};

class DelayState {
public:
  DelayState(const Image &In, const Capture &C) : In(In), C(C) {}

  bool accessible(uint64_t RVA, uint64_t Size, unsigned Access) const {
    const auto *R = In.regionAt(RVA);
    if (!R || !Size || Size > R->MemorySize - (RVA - R->RVA))
      return false;
    for (uint64_t Page = RVA / unpack::value::PageSize;
         Page <= (RVA + Size - 1) / unpack::value::PageSize; ++Page)
      if ((C.PageAccess[Page] & Access) != Access)
        return false;
    return true;
  }

  bool range(uint64_t RVA, uint64_t Size, bool Written = false) {
    if (!accessible(RVA, Size, emulation::Read))
      return false;
    Ranges.push_back({{RVA, RVA + Size}, Written});
    return true;
  }

  bool name(uint64_t RVA) {
    for (uint64_t I = 0; I < value::MaxDelayNameBytes; ++I) {
      if (!accessible(RVA + I, 1, emulation::Read))
        return false;
      const uint8_t Byte = C.Memory[RVA + I];
      if (!Byte)
        return I && range(RVA, I + 1);
      if (Byte < 0x20 || Byte > 0x7e)
        return false;
    }
    return false;
  }

  llvm::Error descriptor(uint64_t RVA) {
    delay_import_directory_table_entry D;
    std::memcpy(&D, C.Memory.data() + RVA, sizeof(D));
    if (D.Attributes != 1)
      return failure(text::DelayAttributes);
    if (!name(D.Name) || !range(D.ModuleHandle, value::PointerSize, true) ||
        !D.DelayImportAddressTable || !D.DelayImportNameTable)
      return failure(text::DelayLayout);
    const bool HasUnload = D.UnloadDelayImportTable;
    Writes.emplace_back(D.ModuleHandle, 0);
    uint64_t Count = 0;
    for (;; ++Count) {
      const uint64_t IAT =
          uint64_t(D.DelayImportAddressTable) + Count * value::PointerSize;
      const uint64_t INT =
          uint64_t(D.DelayImportNameTable) + Count * value::PointerSize;
      const uint64_t Unload =
          uint64_t(D.UnloadDelayImportTable) + Count * value::PointerSize;
      if (!accessible(IAT, value::PointerSize, emulation::Read) ||
          !accessible(INT, value::PointerSize, emulation::Read) ||
          (HasUnload &&
           !accessible(Unload, value::PointerSize, emulation::Read)))
        return failure(text::DelayLayout);
      const uint64_t Symbol = read64le(C.Memory.data() + INT);
      const uint64_t Current = read64le(C.Memory.data() + IAT);
      if (!Symbol) {
        if (Current || (HasUnload && read64le(C.Memory.data() + Unload)))
          return failure(text::DelayLayout);
        break;
      }
      if (Imports == defaults::Imports)
        return failure(unpack::text::ImportLimit);
      ++Imports;
      if (Symbol & value::OrdinalFlag) {
        if (Symbol & ~(value::OrdinalFlag | uint64_t(UINT16_MAX)))
          return failure(text::DelayLayout);
      } else if (Symbol > UINT32_MAX || !range(Symbol, value::HintBytes) ||
                 !name(Symbol + value::HintBytes)) {
        return failure(text::DelayLayout);
      }
      if (HasUnload && !internalThunk(read64le(C.Memory.data() + Unload)))
        return failure(text::DelayState);
      if (Current >= C.Base && Current - C.Base < In.extent()) {
        if (!internalThunk(Current))
          return failure(text::DelayState);
      } else if (const auto Target = C.Exports.find(Current);
                 Target != C.Exports.end()) {
        // Re-entering a resolved helper could repeat its persistent effects.
        // Rebind the exact declared cell instead; the separate lookup table
        // limits the native loader without consuming the next lazy thunk.
        Bindings.push_back({IAT, &Target->second});
      } else {
        return failure(text::DelayState);
      }
    }
    const uint64_t Bytes = (Count + 1) * value::PointerSize;
    if (!range(D.DelayImportAddressTable, Bytes, true) ||
        !range(D.DelayImportNameTable, Bytes) ||
        (HasUnload && !range(D.UnloadDelayImportTable, Bytes)) ||
        (D.BoundDelayImportTable && !range(D.BoundDelayImportTable, Bytes)))
      return failure(text::DelayLayout);
    // A bound cache belongs to the old provider image. The helper must use
    // ordinary export resolution after loading that provider afresh.
    ClearBindings.push_back(RVA);
    return llvm::Error::success();
  }

  llvm::Expected<DelayImportState>
  apply(llvm::MutableArrayRef<uint8_t> Memory) {
    llvm::sort(Ranges,
               [](const auto &A, const auto &B) { return A.Begin < B.Begin; });
    uint64_t ReadEnd = 0, WriteEnd = 0;
    DelayImportState Out;
    auto &Owned = Out.Metadata;
    for (const auto &R : Ranges) {
      if (R.Begin < WriteEnd || (R.Written && R.Begin < ReadEnd))
        return failure(text::DelayLayout);
      (R.Written ? WriteEnd : ReadEnd) =
          std::max(R.Written ? WriteEnd : ReadEnd, R.End);
      if (!Owned.empty() && R.Begin <= Owned.back().End)
        Owned.back().End = std::max(Owned.back().End, R.End);
      else
        Owned.push_back({R.Begin, R.End});
    }
    for (const auto &R : Ranges)
      if (R.Written && overlapsDirectory(R.Begin, R.End - R.Begin))
        return failure(text::DelayLayout);
    for (uint64_t RVA : ClearBindings)
      if (overlapsDirectory(RVA, sizeof(delay_import_directory_table_entry)))
        return failure(text::DelayLayout);
    for (const auto &[RVA, Value] : Writes)
      write64le(Memory.data() + RVA, Value);
    for (uint64_t RVA : ClearBindings) {
      write32le(Memory.data() + RVA +
                    offsetof(delay_import_directory_table_entry,
                             BoundDelayImportTable),
                0);
      write32le(Memory.data() + RVA +
                    offsetof(delay_import_directory_table_entry, TimeStamp),
                0);
    }
    Out.Resolved = std::move(Bindings);
    return Out;
  }

private:
  bool internalThunk(uint64_t Address) const {
    return Address >= C.Base &&
           accessible(Address - C.Base, 1, emulation::Execute) &&
           (In.architecture() != emulation::GuestArchitecture::AArch64 ||
            Address % 4 == 0);
  }

  bool overlapsDirectory(uint64_t RVA, uint64_t Size) const {
    const auto &Directories = In.headers().Directories;
    for (unsigned I = 0; I < Directories.size(); ++I) {
      if (I == llvm::COFF::DELAY_IMPORT_DESCRIPTOR ||
          I == llvm::COFF::CERTIFICATE_TABLE ||
          I == llvm::COFF::BASE_RELOCATION_TABLE ||
          I == llvm::COFF::BOUND_IMPORT || I == llvm::COFF::IAT)
        continue;
      const auto &D = Directories[I];
      if (D.Size && RVA < uint64_t(D.RelativeVirtualAddress) + D.Size &&
          D.RelativeVirtualAddress < RVA + Size)
        return true;
    }
    return false;
  }

  const Image &In;
  const Capture &C;
  uint64_t Imports = 0;
  std::vector<Range> Ranges;
  std::vector<DelayImportState::Binding> Bindings;
  std::vector<std::pair<uint64_t, uint64_t>> Writes;
  std::vector<uint64_t> ClearBindings;
};
} // namespace

llvm::Expected<DelayImportState>
restoreDelayImports(const Image &In, const Capture &C,
                    llvm::MutableArrayRef<uint8_t> Memory) {
  const auto D = In.directory(llvm::COFF::DELAY_IMPORT_DESCRIPTOR);
  if (!D.RelativeVirtualAddress && !D.Size)
    return DelayImportState{};
  DelayState State(In, C);
  if (!D.Size || D.Size % sizeof(delay_import_directory_table_entry) ||
      D.Size / sizeof(delay_import_directory_table_entry) >
          value::MaxDelayDescriptors ||
      !State.range(D.RelativeVirtualAddress, D.Size, true))
    return failure(text::DelayLayout);
  for (uint64_t Offset = 0; Offset < D.Size;
       Offset += sizeof(delay_import_directory_table_entry)) {
    const uint64_t RVA = uint64_t(D.RelativeVirtualAddress) + Offset;
    if (llvm::all_of(llvm::ArrayRef(C.Memory).slice(
                         RVA, sizeof(delay_import_directory_table_entry)),
                     [](uint8_t Byte) { return Byte == 0; }))
      return State.apply(Memory);
    if (auto E = State.descriptor(RVA))
      return std::move(E);
  }
  return failure(text::DelayLayout);
}
} // namespace neverd::unpack::pe
