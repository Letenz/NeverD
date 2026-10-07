//===- PETLS.cpp - TLS metadata validated by observed callbacks -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "PEImage.h"

#include "neverd/emulation/GuestMemory.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <cstring>
#include <iterator>

namespace neverd::unpack::pe {
llvm::Expected<std::optional<uint64_t>> recoverTLSDirectory(const Image &In,
                                                            const Capture &C) {
  using llvm::object::coff_tls_directory64;
  using llvm::support::endian::read64le;
  if (C.Memory.size() != In.extent() || C.Baseline.size() != In.extent() ||
      C.PageAccess.size() != In.extent() / unpack::value::PageSize)
    return failure(unpack::text::ImageChanged);
  const llvm::ArrayRef<uint8_t> Memory(C.Memory);
  const uint64_t Original =
      In.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress;
  if (!Original || Original > Memory.size() ||
      sizeof(coff_tls_directory64) > Memory.size() - Original)
    return std::nullopt;

  auto Load = [](llvm::ArrayRef<uint8_t> Bytes, uint64_t RVA) {
    coff_tls_directory64 Record;
    std::memcpy(&Record, Bytes.data() + RVA, sizeof(Record));
    return Record;
  };
  // The loader allocated TLS from this record. A replacement must describe
  // the same allocation, independently of any protector's metadata layout.
  const auto Loaded = Load(C.Baseline, Original);
  auto Accessible = [&](uint64_t VA, uint64_t Size, unsigned Permissions) {
    if (VA < C.Base || VA - C.Base >= Memory.size() ||
        Size > Memory.size() - (VA - C.Base))
      return false;
    const uint64_t Begin = VA - C.Base;
    for (uint64_t At = Begin; At < Begin + Size;) {
      if ((C.PageAccess[At / unpack::value::PageSize] & Permissions) !=
          Permissions)
        return false;
      At += std::min(Begin + Size - At,
                     unpack::value::PageSize - At % unpack::value::PageSize);
    }
    return true;
  };
  auto Matches = [&](uint64_t RVA, bool Generated) {
    if (!Accessible(C.Base + RVA, sizeof(coff_tls_directory64),
                    emulation::Read))
      return false;
    const auto Record = Load(Memory, RVA);
    const uint64_t Begin = Record.StartAddressOfRawData;
    const uint64_t End = Record.EndAddressOfRawData;
    if (End < Begin || Record.AddressOfIndex != Loaded.AddressOfIndex ||
        uint64_t(Loaded.EndAddressOfRawData) < Loaded.StartAddressOfRawData ||
        End - Begin != uint64_t(Loaded.EndAddressOfRawData) -
                           Loaded.StartAddressOfRawData ||
        Record.SizeOfZeroFill != Loaded.SizeOfZeroFill ||
        Record.Characteristics != Loaded.Characteristics ||
        !Accessible(Record.AddressOfIndex, sizeof(uint32_t),
                    emulation::Write) ||
        (Begin && !Accessible(Begin, std::max<uint64_t>(End - Begin, 1),
                              emulation::Read)) ||
        (!Begin && End) || !Record.AddressOfCallBacks ||
        Record.AddressOfCallBacks % value::PointerSize)
      return false;
    uint64_t At = Record.AddressOfCallBacks;
    bool Witnessed = false;
    for (uint64_t I = 0; I <= value::MaxTLSCallbacks; ++I) {
      if (!Accessible(At, value::PointerSize, emulation::Read))
        return false;
      const uint64_t Target = read64le(Memory.data() + At - C.Base);
      if (!Target)
        return Witnessed;
      if (I == value::MaxTLSCallbacks ||
          !Accessible(Target, 1, emulation::Execute) ||
          (In.architecture() == emulation::GuestArchitecture::AArch64 &&
           Target % sizeof(uint32_t)))
        return false;
      const bool Initialized = llvm::is_contained(C.Initializers, Target);
      Witnessed |= llvm::any_of(C.Transfers, [&](const UnpackTransfer &T) {
        return (!T.StackBalanced || !T.ProgramInvocation) && T.Generation &&
               T.RVA == Target - C.Base &&
               (!Generated || !Initialized || !T.ProgramInvocation);
      });
      if (!Generated)
        Witnessed |= Initialized;
      At += value::PointerSize;
    }
    return false;
  };
  auto Find = [&](bool Generated) -> llvm::Expected<std::optional<uint64_t>> {
    // A loader may have restored the header's original record in place.
    if (Matches(Original, Generated))
      return Original;
    std::optional<uint64_t> Found;
    for (const auto &Region : In.regions()) {
      const uint64_t End = Region.RVA + Region.MemorySize;
      for (uint64_t RVA = Region.RVA; RVA + sizeof(coff_tls_directory64) <= End;
           ++RVA) {
        // Check allocation identity before walking candidate callback lists.
        if (RVA == Original ||
            read64le(Memory.data() + RVA +
                     offsetof(coff_tls_directory64, AddressOfIndex)) !=
                Loaded.AddressOfIndex ||
            !Matches(RVA, Generated))
          continue;
        if (Found)
          return failure(text::AmbiguousTLS);
        Found = RVA;
      }
    }
    return Found;
  };
  // A completed loader initializer, even revisited by the program after a
  // rewrite, cannot outrank newly generated callbacks. An OS-owned call into
  // generated code remains strong evidence even if it completed. Use weaker
  // initialization evidence only when no generated callback record matches.
  auto Found = Find(true);
  if (!Found || *Found)
    return Found;
  return Find(false);
}

llvm::Expected<TLSRebuild> rebuildTLS(const Image &In, const Capture &C,
                                      uint64_t MetadataRVA,
                                      std::vector<uint8_t> &Metadata) {
  using llvm::object::coff_tls_directory64;
  using namespace llvm::support::endian;
  TLSRebuild Result;
  auto DirectoryRVA = recoverTLSDirectory(In, C);
  if (!DirectoryRVA)
    return DirectoryRVA.takeError();
  if (!*DirectoryRVA)
    return Result;
  Result.DirectoryRVA = **DirectoryRVA;
  coff_tls_directory64 Record;
  std::memcpy(&Record, C.Memory.data() + **DirectoryRVA, sizeof(Record));
  std::vector<uint64_t> Callbacks;
  const uint64_t Array = uint64_t(Record.AddressOfCallBacks) - C.Base;
  for (uint64_t I = 0; I < value::MaxTLSCallbacks; ++I) {
    const uint64_t Target = read64le(C.Memory.data() + Array + I * 8);
    if (!Target)
      break;
    Callbacks.push_back(Target);
    Result.MaterializedCallbacks += llvm::is_contained(C.Initializers, Target);
  }
  if (!Result.MaterializedCallbacks)
    return Result;

  const uint64_t TLSSize =
      uint64_t(Record.EndAddressOfRawData) - Record.StartAddressOfRawData;
  if (TLSSize && (!C.ThreadLocal || C.ThreadLocal->size() != TLSSize))
    return failure(text::TLSState);
  bool RestoreTLS =
      TLSSize &&
      !std::equal(C.ThreadLocal->begin(), C.ThreadLocal->end(),
                  C.Memory.begin() +
                      (uint64_t(Record.StartAddressOfRawData) - C.Base));

  // Never overwrite original code or TLS storage. The adapters, callback
  // table and replacement directory all belong to the appended section.
  const uint64_t Directory = llvm::alignTo(Metadata.size(), uint64_t(8));
  const uint64_t Pointers = Directory + sizeof(Record);
  const uint64_t Code =
      llvm::alignTo(Pointers + (Callbacks.size() + 1) * 8, uint64_t(16));
  const bool X64 = In.architecture() == emulation::GuestArchitecture::X64;
  const uint64_t Stride = X64 ? 24 : 32;
  const uint64_t CopyStride = X64 ? 80 : 96;
  const uint64_t Data = Code + Result.MaterializedCallbacks * Stride +
                        (RestoreTLS ? CopyStride - Stride : 0);
  const uint64_t End = Data + (RestoreTLS ? TLSSize : 0);
  if (MetadataRVA > UINT32_MAX || End > UINT32_MAX - MetadataRVA)
    return failure(text::ImageSize);
  Metadata.resize(End, 0);
  if (RestoreTLS)
    std::copy(C.ThreadLocal->begin(), C.ThreadLocal->end(),
              Metadata.begin() + Data);
  uint64_t Next = Code;
  for (size_t I = 0; I < Callbacks.size(); ++I) {
    const uint64_t Target = Callbacks[I];
    uint64_t Callback = Target;
    if (llvm::is_contained(C.Initializers, Target)) {
      Callback = C.Base + MetadataRVA + Next;
      if (RestoreTLS && X64) {
        // Restore the captured main-thread block only on process attach.
        // The loader supplies the new TLS index through AddressOfIndex.
        // Read GS:[0x58], select the loader's index, and copy through volatile
        // r10/r11/rcx. Other notifications tail-call the original callback.
        constexpr uint8_t Adapter[] = {
            0x83, 0xfa, 0x01, 0x75, 0x39, 0x65, 0x4c, 0x8b, 0x14, 0x25,
            0x58, 0x00, 0x00, 0x00, 0x48, 0xb8, 0,    0,    0,    0,
            0,    0,    0,    0,    0x8b, 0x00, 0x4d, 0x8b, 0x14, 0xc2,
            0x49, 0xbb, 0,    0,    0,    0,    0,    0,    0,    0,
            0xb9, 0,    0,    0,    0,    0x41, 0x8a, 0x03, 0x41, 0x88,
            0x02, 0x49, 0xff, 0xc3, 0x49, 0xff, 0xc2, 0xff, 0xc9, 0x75,
            0xf0, 0xc3, 0xff, 0x25, 0,    0,    0,    0,
        };
        std::copy(std::begin(Adapter), std::end(Adapter),
                  Metadata.begin() + Next);
        write64le(Metadata.data() + Next + 16, Record.AddressOfIndex);
        write64le(Metadata.data() + Next + 32, C.Base + MetadataRVA + Data);
        write32le(Metadata.data() + Next + 41, TLSSize);
        write64le(Metadata.data() + Next + sizeof(Adapter), Target);
      } else if (RestoreTLS) {
        constexpr uint32_t Adapter[] = {
            0x7100043f, // cmp w1, #DLL_PROCESS_ATTACH
            0x54000181, // b.ne tail
            0xf9402e50, // ldr x16, [x18, #0x58]
            0x580001b1, // ldr x17, index
            0xb9400231, // ldr w17, [x17]
            0xf8717a10, // ldr x16, [x16, x17, lsl #3]
            0x58000189, // ldr x9, data
            0x180001ea, // ldr w10, count
            0x3840152b, // copy: ldrb w11, [x9], #1
            0x3800160b, // strb w11, [x16], #1
            0x7100054a, // subs w10, w10, #1
            0x54ffffa1, // b.ne copy
            0xd65f03c0, // ret
            0x580000f0, // tail: ldr x16, target
            0xd61f0200, // br x16
            0xd503201f, // nop
        };
        for (size_t J = 0; J < std::size(Adapter); ++J)
          write32le(Metadata.data() + Next + J * 4, Adapter[J]);
        write64le(Metadata.data() + Next + 64, Record.AddressOfIndex);
        write64le(Metadata.data() + Next + 72, C.Base + MetadataRVA + Data);
        write64le(Metadata.data() + Next + 80, Target);
        write32le(Metadata.data() + Next + 88, TLSSize);
      } else if (X64) {
        // cmp edx, DLL_PROCESS_ATTACH; je ret; jmp [rip + target]; ret.
        constexpr uint8_t Adapter[] = {0x83, 0xfa, 0x01, 0x74, 0x06, 0xff,
                                       0x25, 0x05, 0x00, 0x00, 0x00, 0xc3,
                                       0x90, 0x90, 0x90, 0x90};
        std::copy(std::begin(Adapter), std::end(Adapter),
                  Metadata.begin() + Next);
        write64le(Metadata.data() + Next + sizeof(Adapter), Target);
      } else {
        constexpr uint32_t Adapter[] = {
            0x7100043f, // cmp w1, #DLL_PROCESS_ATTACH
            0x54000060, // b.eq ret
            0x58000090, // ldr x16, target
            0xd61f0200, // br x16
            0xd65f03c0, // ret
            0xd503201f, // nop
        };
        for (size_t J = 0; J < std::size(Adapter); ++J)
          write32le(Metadata.data() + Next + J * 4, Adapter[J]);
        write64le(Metadata.data() + Next + sizeof(Adapter), Target);
      }
      Next += RestoreTLS ? CopyStride : Stride;
      RestoreTLS = false;
    }
    write64le(Metadata.data() + Pointers + I * 8, Callback);
  }
  Record.AddressOfCallBacks = C.Base + MetadataRVA + Pointers;
  std::memcpy(Metadata.data() + Directory, &Record, sizeof(Record));
  Result.DirectoryRVA = MetadataRVA + Directory;
  return Result;
}
} // namespace neverd::unpack::pe
