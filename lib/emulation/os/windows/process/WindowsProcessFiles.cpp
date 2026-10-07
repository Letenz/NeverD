//===- WindowsProcessFiles.cpp - Loaded-image file open ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessModules.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;

// Windows-1252 bytes 0x80..0x9F. The loader round-trips a 256-byte code-page
// table through this mapping, including the five bytes Windows leaves
// undefined.
constexpr char16_t Cp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

char16_t decode1252(uint8_t Byte) {
  if (Byte >= 0x80 && Byte < 0xA0)
    return Cp1252High[Byte - 0x80];
  return Byte;
}

bool encode1252(char16_t Unit, uint8_t &Byte) {
  if (Unit <= 0x7F || (Unit >= 0xA0 && Unit <= 0xFF)) {
    Byte = static_cast<uint8_t>(Unit);
    return true;
  }
  for (uint8_t I = 0; I < 32; ++I) {
    if (Cp1252High[I] != Unit)
      continue;
    Byte = static_cast<uint8_t>(0x80 + I);
    return true;
  }
  return false;
}

char16_t foldCase(char16_t Unit, bool Upper) {
  if (Upper) {
    if (Unit >= u'a' && Unit <= u'z')
      return static_cast<char16_t>(Unit - 0x20);
    if (Unit >= 0xE0 && Unit <= 0xFE && Unit != 0xF7)
      return static_cast<char16_t>(Unit - 0x20);
    if (Unit == 0x00FF)
      return 0x0178;
    switch (Unit) {
    case 0x0161:
      return 0x0160;
    case 0x017E:
      return 0x017D;
    case 0x0192:
      return 0x0191;
    default:
      return Unit;
    }
  }
  if (Unit >= u'A' && Unit <= u'Z')
    return static_cast<char16_t>(Unit + 0x20);
  if ((Unit >= 0xC0 && Unit <= 0xD6) || (Unit >= 0xD8 && Unit <= 0xDE))
    return static_cast<char16_t>(Unit + 0x20);
  switch (Unit) {
  case 0x0160:
    return 0x0161;
  case 0x017D:
    return 0x017E;
  case 0x0178:
    return 0x00FF;
  case 0x0191:
    return 0x0192;
  default:
    return Unit;
  }
}

uint16_t ctype1(char16_t Unit) {
  if (Unit < 0x20 || Unit == 0x7F) {
    uint16_t Type = C1Cntrl | C1Defined;
    if (Unit >= 0x09 && Unit <= 0x0D)
      Type |= C1Space;
    if (Unit == 0x09)
      Type |= C1Blank;
    return Type;
  }
  if (Unit == u' ')
    return C1Space | C1Blank | C1Defined;
  if (Unit >= u'0' && Unit <= u'9')
    return C1Digit | C1XDigit | C1Defined;
  if (Unit >= u'A' && Unit <= u'Z')
    return C1Upper | C1Alpha | C1Defined | (Unit <= u'F' ? C1XDigit : 0);
  if (Unit >= u'a' && Unit <= u'z')
    return C1Lower | C1Alpha | C1Defined | (Unit <= u'f' ? C1XDigit : 0);
  if (Unit < 0x80)
    return C1Punct | C1Defined;
  switch (Unit) {
  case 0x00A0:
    return C1Space | C1Blank | C1Defined;
  case 0x0160:
  case 0x0178:
  case 0x017D:
  case 0x0191:
    return C1Upper | C1Alpha | C1Defined;
  case 0x0161:
  case 0x017E:
  case 0x0192:
    return C1Lower | C1Alpha | C1Defined;
  default:
    break;
  }
  if (Unit >= 0x80 && Unit <= 0x9F)
    return C1Cntrl | C1Defined;
  if ((Unit >= 0xC0 && Unit <= 0xD6) || (Unit >= 0xD8 && Unit <= 0xDE))
    return C1Upper | C1Alpha | C1Defined;
  if (Unit == 0xDF || (Unit >= 0xE0 && Unit <= 0xF6) ||
      (Unit >= 0xF8 && Unit <= 0xFF))
    return C1Lower | C1Alpha | C1Defined;
  return C1Punct | C1Defined;
}
} // namespace

llvm::Expected<std::optional<uint64_t>>
Services::openImage(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  auto Word = [&](uint64_t Address, unsigned Size) -> llvm::Expected<uint64_t> {
    auto Readable = access(Address, Size, Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure(text::Access);
    return CPU.readInteger(Address, Size);
  };
  if (!A[0] || !A[1] || !A[2] || !A[3] || (A[4] & ~FileShareMask) ||
      (A[5] & ~FileOpenOptions))
    return Refuse("flags");
  auto Length = Word(A[2], DWordSize);
  if (!Length)
    return Length.takeError();
  auto Root = Word(A[2] + ObjectRootDirectory, PointerSize);
  if (!Root)
    return Root.takeError();
  auto Name = Word(A[2] + ObjectName, PointerSize);
  if (!Name)
    return Name.takeError();
  auto Flags = Word(A[2] + ObjectAttributeFlags, DWordSize);
  if (!Flags)
    return Flags.takeError();
  if (*Length != ObjectAttributesLength || *Root || !*Name ||
      (*Flags & ~ObjectCaseInsensitive))
    return Refuse("attributes");
  auto NameLength = Word(*Name + UnicodeLength, WideSize);
  if (!NameLength)
    return NameLength.takeError();
  auto Buffer = Word(*Name + UnicodeBuffer, PointerSize);
  if (!Buffer)
    return Buffer.takeError();
  if (!*Buffer || !*NameLength || (*NameLength & 1) ||
      *NameLength > MaxName * WideSize)
    return Refuse("name");
  const uint64_t Units = *NameLength / WideSize;
  auto Readable = access(*Buffer, Units * WideSize, Read);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return failure(text::Access);
  std::string Narrow;
  Narrow.reserve(Units);
  for (uint64_t I = 0; I < Units; ++I) {
    auto Unit = CPU.readInteger(*Buffer + I * WideSize, WideSize);
    if (!Unit)
      return Unit.takeError();
    if (!*Unit || *Unit > 0x7f)
      return Refuse("name");
    Narrow.push_back(static_cast<char>(*Unit));
  }
  const auto Slash = Narrow.find_last_of("\\/");
  const llvm::StringRef Base = llvm::StringRef(Narrow).substr(
      Slash == std::string::npos ? 0 : Slash + 1);
  if (Modules.Identities.empty() ||
      !Base.equals_insensitive(Modules.Identities.front().Name))
    return Refuse(Narrow);
  std::error_code EC;
  const auto Size = std::filesystem::file_size(Modules.ImagePath, EC);
  if (EC || !Size)
    return failure(text::ImageFile);
  if (Files.size() >= MaxFileHandles)
    return failure(text::ImageFile);
  const uint64_t Handle = FileHandleBase + Files.size() * FileHandleStride;
  auto Writable = access(A[0], PointerSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  Writable = access(A[3], PointerSize + IoStatusInformation, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  if (auto E = CPU.writeInteger(A[0], Handle, PointerSize))
    return std::move(E);
  if (auto E = CPU.writeInteger(A[3], 0, DWordSize))
    return std::move(E);
  if (auto E =
          CPU.writeInteger(A[3] + IoStatusInformation, FileOpened, PointerSize))
    return std::move(E);
  Files.insert({Handle, OpenedFile{Modules.ImagePath, Size}});
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::createSection(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  auto Word = [&](uint64_t Address, unsigned Size) -> llvm::Expected<uint64_t> {
    auto Readable = access(Address, Size, Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure(text::Access);
    return CPU.readInteger(Address, Size);
  };
  const auto File = Files.find(A[6]);
  if (!A[0] || A[1] != SectionMapRead || A[4] != PageReadOnly ||
      A[5] != SectionCommit || File == Files.end())
    return Refuse("flags");
  uint64_t Size = File->second.Size;
  if (A[3]) {
    auto Maximum = Word(A[3], PointerSize);
    if (!Maximum)
      return Maximum.takeError();
    if (!*Maximum || *Maximum > Size)
      return Refuse("size");
    Size = *Maximum;
  }
  if (A[2]) {
    auto Length = Word(A[2], DWordSize);
    if (!Length)
      return Length.takeError();
    auto Root = Word(A[2] + ObjectRootDirectory, PointerSize);
    if (!Root)
      return Root.takeError();
    auto Name = Word(A[2] + ObjectName, PointerSize);
    if (!Name)
      return Name.takeError();
    auto Flags = Word(A[2] + ObjectAttributeFlags, DWordSize);
    if (!Flags)
      return Flags.takeError();
    if (*Length != ObjectAttributesLength || *Root || *Name ||
        (*Flags & ~ObjectCaseInsensitive))
      return Refuse("attributes");
  }
  if (Sections.size() >= MaxImageSections)
    return failure(text::ImageFile);
  auto Writable = access(A[0], PointerSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  const uint64_t Handle =
      SectionHandleBase + Sections.size() * SectionHandleStride;
  if (auto E = CPU.writeInteger(A[0], Handle, PointerSize))
    return std::move(E);
  Sections.insert({Handle, SectionObject{File->second.Path, Size}});
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::mapSection(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  auto Word = [&](uint64_t Address, unsigned Size) -> llvm::Expected<uint64_t> {
    auto Readable = access(Address, Size, Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure(text::Access);
    return CPU.readInteger(Address, Size);
  };
  if (CPU.architecture() != GuestArchitecture::X64)
    return Refuse("isa");
  const auto Section = Sections.find(A[0]);
  if (Section == Sections.end() || A[1] != CurrentProcess || A[3] || A[4] ||
      A[5] || (A[7] != SectionViewShare && A[7] != SectionViewUnmap))
    return Refuse("flags");
  auto SP = CPU.readRegister(CPURegister::X64SP);
  if (!SP)
    return SP.takeError();
  auto Allocation = Word((*SP)[0] + 0x48, PointerSize);
  if (!Allocation)
    return Allocation.takeError();
  auto Protect = Word((*SP)[0] + 0x50, PointerSize);
  if (!Protect)
    return Protect.takeError();
  if (*Allocation || *Protect != PageReadOnly)
    return Refuse(std::string("tail ") + llvm::utohexstr(*Allocation) + " " +
                  llvm::utohexstr(*Protect));
  auto Preferred = Word(A[2], PointerSize);
  if (!Preferred)
    return Preferred.takeError();
  auto Requested = Word(A[6], PointerSize);
  if (!Requested)
    return Requested.takeError();
  if (*Requested > Section->second.Size)
    return Refuse("size");
  uint64_t Bytes = *Requested ? *Requested : Section->second.Size;
  const uint64_t Mapped = (Bytes + PageSize - 1) & ~(PageSize - 1);
  if (!Bytes || Mapped < Bytes)
    return Refuse("size");
  auto Placed = Virtual.allocate(*Preferred, Mapped, MemReserve | MemCommit,
                                 PageReadWrite);
  if (!Placed)
    return Placed.takeError();
  if (Placed->Unsupported || Placed->Error)
    return Refuse(std::string("place ") + llvm::utohexstr(Placed->Error));
  std::ifstream Input(Section->second.Path, std::ios::binary);
  std::vector<uint8_t> File(Bytes);
  if (!Input.read(reinterpret_cast<char *>(File.data()), Bytes))
    return failure(text::ImageFile);
  if (auto E = CPU.write(Placed->Value, File))
    return std::move(E);
  auto Published = Virtual.protect(Placed->Value, Mapped, PageReadOnly);
  if (!Published)
    return Published.takeError();
  if (Published->Unsupported || Published->Error)
    return Refuse("protect");
  auto Writable = access(A[2], PointerSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  Writable = access(A[6], PointerSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  if (auto E = CPU.writeInteger(A[2], Placed->Value, PointerSize))
    return std::move(E);
  if (auto E = CPU.writeInteger(A[6], Mapped, PointerSize))
    return std::move(E);
  Views.insert({Placed->Value, MappedView{Mapped}});
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::unmapSection(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto View = Views.find(A[1]);
  if (A[0] != CurrentProcess || View == Views.end()) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = std::string(text::ServiceArguments) + S.Name;
    return std::nullopt;
  }
  auto Freed = Virtual.free(A[1], 0, MemRelease);
  if (!Freed)
    return Freed.takeError();
  if (Freed->Unsupported || Freed->Error)
    return failure(text::ImageFile);
  Views.erase(View);
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::closeHandle(const Service &S, const NativeCallEvent &Event) {
  const uint64_t Handle = Event.Arguments[0];
  if (Sections.erase(Handle) || Files.erase(Handle))
    return std::optional<uint64_t>(0);
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = std::string(text::ServiceArguments) + S.Name;
  return std::nullopt;
}

llvm::Expected<std::optional<uint64_t>>
Services::protectMemory(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  auto Word = [&](uint64_t Address, unsigned Size) -> llvm::Expected<uint64_t> {
    auto Readable = access(Address, Size, Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure(text::Access);
    return CPU.readInteger(Address, Size);
  };
  if (A[0] != CurrentProcess || !A[1] || !A[2] || !A[4])
    return Refuse("flags");
  auto BaseIn = Word(A[1], PointerSize);
  if (!BaseIn)
    return BaseIn.takeError();
  auto SizeIn = Word(A[2], PointerSize);
  if (!SizeIn)
    return SizeIn.takeError();
  if (!*SizeIn || *BaseIn >= UserLimit || *SizeIn > UserLimit - *BaseIn)
    return Refuse("range");
  uint64_t Base = *BaseIn & ~(PageSize - 1);
  const uint64_t End = (*BaseIn + *SizeIn + PageSize - 1) & ~(PageSize - 1);
  const uint64_t Size = End - Base;
  auto Protected = Virtual.protect(Base, Size, static_cast<uint32_t>(A[3]));
  if (!Protected)
    return Protected.takeError();
  if (Protected->Unsupported || Protected->Error)
    return Refuse(std::string("protect ") + llvm::utohexstr(A[3]) + " " +
                  llvm::utohexstr(Protected->Error));
  auto Writable = access(A[1], PointerSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  Writable = access(A[2], PointerSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  Writable = access(A[4], DWordSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  if (auto E = CPU.writeInteger(A[1], Base, PointerSize))
    return std::move(E);
  if (auto E = CPU.writeInteger(A[2], Size, PointerSize))
    return std::move(E);
  if (auto E = CPU.writeInteger(A[4], Protected->Value, DWordSize))
    return std::move(E);
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::querySystem(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  if (A[0] != SystemBasicInformation)
    return Refuse(std::string("class ") + llvm::utohexstr(A[0]));
  if (A[3]) {
    auto Writable = access(A[3], DWordSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::Access);
  }
  if (A[2] < SystemBasicInformationSize) {
    if (A[3])
      if (auto E =
              CPU.writeInteger(A[3], SystemBasicInformationSize, DWordSize))
        return std::move(E);
    return std::optional<uint64_t>(
        uint64_t(int64_t(int32_t(StatusInfoLengthMismatch))));
  }
  if (!A[1])
    return Refuse("buffer");
  auto Writable = access(A[1], SystemBasicInformationSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  std::array<uint8_t, SystemBasicInformationSize> Bytes{};
  using namespace llvm::support::endian;
  write32le(Bytes.data() + 0x04, TimerIncrement);
  write32le(Bytes.data() + 0x08, PageSize);
  write32le(Bytes.data() + 0x0c, PhysicalPages);
  write32le(Bytes.data() + 0x10, 1);
  write32le(Bytes.data() + 0x14, PhysicalPages + 1);
  write64le(Bytes.data() + 0x18, ImageAlignment);
  write64le(Bytes.data() + 0x20, ImageAlignment);
  write64le(Bytes.data() + 0x28, UserLimit - 1);
  write64le(Bytes.data() + 0x30, ProcessorCount);
  Bytes[0x38] = ProcessorCount;
  if (auto E = CPU.write(A[1], Bytes))
    return std::move(E);
  if (A[3])
    if (auto E = CPU.writeInteger(A[3], SystemBasicInformationSize, DWordSize))
      return std::move(E);
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::queryProcess(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  if (A[1] != ProcessBasicInformation)
    return Refuse(std::string("class ") + llvm::utohexstr(A[1]));
  if (A[0] != CurrentProcess)
    return Refuse("process");
  if (A[4]) {
    auto Writable = access(A[4], DWordSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::Access);
  }
  if (A[3] < ProcessBasicInformationSize) {
    if (A[4])
      if (auto E =
              CPU.writeInteger(A[4], ProcessBasicInformationSize, DWordSize))
        return std::move(E);
    return std::optional<uint64_t>(
        uint64_t(int64_t(int32_t(StatusInfoLengthMismatch))));
  }
  if (!A[2])
    return Refuse("buffer");
  auto Writable = access(A[2], ProcessBasicInformationSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  std::array<uint8_t, ProcessBasicInformationSize> Bytes{};
  using namespace llvm::support::endian;
  write32le(Bytes.data() + 0x00, StatusPending);
  write64le(Bytes.data() + 0x08, PEB);
  write64le(Bytes.data() + 0x10, ProcessorCount);
  write32le(Bytes.data() + 0x18, BasePriorityNormal);
  write64le(Bytes.data() + 0x20, ProcessID);
  if (auto E = CPU.write(A[2], Bytes))
    return std::move(E);
  if (A[4])
    if (auto E = CPU.writeInteger(A[4], ProcessBasicInformationSize, DWordSize))
      return std::move(E);
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::queryThread(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  if (A[1] != ThreadBasicInformation)
    return Refuse(std::string("class ") + llvm::utohexstr(A[1]));
  if (A[0] != CurrentThread)
    return Refuse("thread");
  if (A[4]) {
    auto Writable = access(A[4], DWordSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::Access);
  }
  if (A[3] < ThreadBasicInformationSize) {
    if (A[4])
      if (auto E =
              CPU.writeInteger(A[4], ThreadBasicInformationSize, DWordSize))
        return std::move(E);
    return std::optional<uint64_t>(
        uint64_t(int64_t(int32_t(StatusInfoLengthMismatch))));
  }
  if (!A[2])
    return Refuse("buffer");
  auto Writable = access(A[2], ThreadBasicInformationSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  std::array<uint8_t, ThreadBasicInformationSize> Bytes{};
  using namespace llvm::support::endian;
  write32le(Bytes.data() + 0x00, StatusPending);
  write64le(Bytes.data() + 0x08, TEB);
  write64le(Bytes.data() + 0x10, ProcessID);
  write64le(Bytes.data() + 0x18, ThreadID);
  write64le(Bytes.data() + 0x20, ProcessorCount);
  write32le(Bytes.data() + 0x28, BasePriorityNormal);
  write32le(Bytes.data() + 0x2c, BasePriorityNormal);
  if (auto E = CPU.write(A[2], Bytes))
    return std::move(E);
  if (A[4])
    if (auto E = CPU.writeInteger(A[4], ThreadBasicInformationSize, DWordSize))
      return std::move(E);
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::setThread(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  if (A[1] != ThreadHideFromDebugger)
    return Refuse(std::string("class ") + llvm::utohexstr(A[1]));
  if (A[0] != CurrentThread)
    return Refuse("thread");
  if (A[3] > PointerSize)
    return std::optional<uint64_t>(
        uint64_t(int64_t(int32_t(StatusInfoLengthMismatch))));
  if (A[3]) {
    if (!A[2])
      return Refuse("buffer");
    auto Readable = access(A[2], A[3], Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure(text::Access);
  }
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::delay(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  if (A[0] > 1 || !A[1])
    return Refuse("flags");
  auto Readable = access(A[1], PointerSize, Read);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return failure(text::Access);
  auto Ticks = CPU.readInteger(A[1], PointerSize);
  if (!Ticks)
    return Ticks.takeError();
  const auto Interval = static_cast<int64_t>(*Ticks);
  if (Interval < 0) {
    const uint64_t Magnitude = Interval == INT64_MIN
                                   ? static_cast<uint64_t>(INT64_MAX) + 1
                                   : static_cast<uint64_t>(-Interval);
    uint64_t Nanoseconds =
        Magnitude > UINT64_MAX / 100 ? DelayCapNanoseconds : Magnitude * 100;
    if (Nanoseconds > DelayCapNanoseconds)
      Nanoseconds = DelayCapNanoseconds;
    timespec Request{};
    Request.tv_sec = static_cast<time_t>(Nanoseconds / 1000000000ull);
    Request.tv_nsec = static_cast<long>(Nanoseconds % 1000000000ull);
    while (nanosleep(&Request, &Request) != 0 && errno == EINTR)
      ;
  }
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
Services::clock(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  const bool File = S.Kind == API::GetSystemTimeAsFileTime;
  timespec Ts{};
  if (clock_gettime(File ? CLOCK_REALTIME : CLOCK_MONOTONIC, &Ts) != 0)
    return failure("host clock");
  if (S.Kind == API::GetTickCount)
    return std::optional<uint64_t>(
        (uint64_t(Ts.tv_sec) * 1000u + uint64_t(Ts.tv_nsec) / 1000000u) &
        0xffffffffu);
  const uint64_t Value =
      File ? uint64_t(Ts.tv_sec) * PerformanceFrequency + FileTimeEpoch +
                 Ts.tv_nsec / 100
           : uint64_t(Ts.tv_sec) * PerformanceFrequency + Ts.tv_nsec / 100;
  if (!A[0])
    return failure(text::Access);
  auto Writable = access(A[0], PointerSize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  if (auto E = CPU.writeInteger(A[0], Value, PointerSize))
    return std::move(E);
  return std::optional<uint64_t>(File ? 0 : 1);
}

llvm::Expected<std::optional<uint64_t>>
Services::encodePointer(const Service &, const NativeCallEvent &Event) {
  return std::optional<uint64_t>(Event.Arguments[0] ^ PointerCookie);
}

llvm::Expected<std::optional<uint64_t>>
Services::criticalSection(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  const bool Init = S.Kind == API::InitializeCriticalSection ||
                    S.Kind == API::InitializeCriticalSectionAndSpinCount ||
                    S.Kind == API::RtlInitializeCriticalSection ||
                    S.Kind == API::RtlInitializeCriticalSectionAndSpinCount;
  const bool Leave = S.Kind == API::LeaveCriticalSection ||
                     S.Kind == API::RtlLeaveCriticalSection;
  const bool Try = S.Kind == API::TryEnterCriticalSection ||
                   S.Kind == API::RtlTryEnterCriticalSection;
  if (!A[0])
    return failure(text::Access);
  if (Init) {
    auto Writable = access(A[0], CriticalSectionSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::Access);
    std::array<uint8_t, CriticalSectionSize> Bytes{};
    using namespace llvm::support::endian;
    write32le(Bytes.data() + CriticalSectionLock, 0xffffffffu);
    const bool WithSpin =
        S.Kind == API::InitializeCriticalSectionAndSpinCount ||
        S.Kind == API::RtlInitializeCriticalSectionAndSpinCount;
    write64le(Bytes.data() + CriticalSectionSpin, WithSpin ? A[1] : 0);
    if (auto E = CPU.write(A[0], Bytes))
      return std::move(E);
    return std::optional<uint64_t>(1);
  }
  auto Readable = access(A[0] + CriticalSectionLock, DWordSize, Read | Write);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return failure(text::Access);
  auto Lock = CPU.readInteger(A[0] + CriticalSectionLock, DWordSize);
  if (!Lock)
    return Lock.takeError();
  auto Depth = CPU.readInteger(A[0] + CriticalSectionRecursion, DWordSize);
  if (!Depth)
    return Depth.takeError();
  if (Leave) {
    const uint32_t Next = *Depth > 1 ? uint32_t(*Lock) - 1 : 0xffffffffu;
    const uint32_t Recursion = *Depth > 1 ? uint32_t(*Depth) - 1 : 0;
    if (auto E = CPU.writeInteger(A[0] + CriticalSectionLock, Next, DWordSize))
      return std::move(E);
    if (auto E = CPU.writeInteger(A[0] + CriticalSectionRecursion, Recursion,
                                  DWordSize))
      return std::move(E);
    if (!Recursion)
      if (auto E =
              CPU.writeInteger(A[0] + CriticalSectionOwner, 0, PointerSize))
        return std::move(E);
    return std::optional<uint64_t>(0);
  }
  const bool Free = uint32_t(*Lock) == 0xffffffffu;
  if (auto E = CPU.writeInteger(A[0] + CriticalSectionLock,
                                Free ? 0 : uint32_t(*Lock) + 1, DWordSize))
    return std::move(E);
  if (auto E = CPU.writeInteger(A[0] + CriticalSectionRecursion,
                                Free ? 1 : uint32_t(*Depth) + 1, DWordSize))
    return std::move(E);
  if (Free)
    if (auto E = CPU.writeInteger(A[0] + CriticalSectionOwner, ThreadID,
                                  PointerSize))
      return std::move(E);
  return std::optional<uint64_t>(Try ? 1 : 0);
}

llvm::Expected<std::optional<uint64_t>>
Services::crt(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto WinError = [&](uint32_t Code,
                      uint64_t Value =
                          0) -> llvm::Expected<std::optional<uint64_t>> {
    auto V = error(Code, Value);
    if (!V)
      return V.takeError();
    return std::optional<uint64_t>(*V);
  };
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };
  // Win64 passes int counts in the low 32 bits. The high half of a stack slot
  // is unused, and -1 (all bits set in those 32 bits) means a C string.
  auto countedWide = [&](uint64_t Address,
                         uint64_t RawCount) -> llvm::Expected<std::u16string> {
    if (uint32_t(RawCount) == 0xffffffffu) {
      auto Text = readWide(Address, MaxStringUnits);
      if (!Text)
        return Text.takeError();
      Text->push_back(u'\0');
      return std::move(*Text);
    }
    const uint32_t Count = uint32_t(RawCount);
    if (Count > MaxStringUnits)
      return failure(text::EnvironmentLimit);
    if (!Count)
      return std::u16string();
    if (!Budget.remainingMicroseconds())
      return failure(text::EnvironmentTimeout);
    const uint64_t Bytes = uint64_t(Count) * WideSize;
    auto Readable = access(Address, Bytes, Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure(text::Access);
    std::vector<uint8_t> Raw(Bytes);
    if (auto E = CPU.read(Address, Raw))
      return std::move(E);
    std::u16string Text(Count, u'\0');
    for (uint32_t I = 0; I < Count; ++I)
      Text[I] = char16_t(
          llvm::support::endian::read16le(Raw.data() + uint64_t(I) * WideSize));
    return Text;
  };
  auto countedBytes = [&](uint64_t Address,
                          uint64_t RawCount) -> llvm::Expected<std::string> {
    if (uint32_t(RawCount) == 0xffffffffu) {
      std::string Text;
      for (uint64_t I = 0; I < MaxStringUnits; ++I) {
        if (!Budget.remainingMicroseconds())
          return failure(text::EnvironmentTimeout);
        auto Readable = access(Address + I, 1, Read);
        if (!Readable)
          return Readable.takeError();
        if (!*Readable)
          return failure(text::Access);
        auto Unit = CPU.readInteger(Address + I, 1);
        if (!Unit)
          return Unit.takeError();
        Text.push_back(static_cast<char>(*Unit));
        if (!*Unit)
          return Text;
      }
      return failure(text::EnvironmentLimit);
    }
    const uint32_t Count = uint32_t(RawCount);
    if (Count > MaxStringUnits)
      return failure(text::EnvironmentLimit);
    if (!Count)
      return std::string();
    if (!Budget.remainingMicroseconds())
      return failure(text::EnvironmentTimeout);
    auto Readable = access(Address, Count, Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure(text::Access);
    std::vector<uint8_t> Raw(Count);
    if (auto E = CPU.read(Address, Raw))
      return std::move(E);
    return std::string(Raw.begin(), Raw.end());
  };
  auto writeUnits = [&](uint64_t Address,
                        const std::u16string &Text) -> llvm::Error {
    if (Text.empty())
      return llvm::Error::success();
    const uint64_t Bytes = Text.size() * WideSize;
    auto Writable = access(Address, Bytes, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::Access);
    std::vector<uint8_t> Raw(Bytes);
    for (size_t I = 0; I < Text.size(); ++I)
      llvm::support::endian::write16le(Raw.data() + I * WideSize, Text[I]);
    return CPU.write(Address, Raw);
  };

  if (S.Kind == API::GetCommandLineA) {
    if (CommandLineA)
      return std::optional<uint64_t>(CommandLineA);
    auto Wide = readWide(Env.CommandLine, MaxStringUnits);
    if (!Wide)
      return Wide.takeError();
    auto Address = allocateHeap(Wide->size() + 1);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      return WinError(ErrorNotEnoughMemory);
    std::vector<uint8_t> Bytes(Wide->size() + 1);
    for (size_t I = 0; I < Wide->size(); ++I)
      Bytes[I] = (*Wide)[I] <= 0xFF ? static_cast<uint8_t>((*Wide)[I])
                                    : static_cast<uint8_t>('?');
    if (auto E = CPU.write(*Address, Bytes))
      return std::move(E);
    CommandLineA = *Address;
    return std::optional<uint64_t>(CommandLineA);
  }

  if (S.Kind == API::GetStartupInfoA) {
    if (!A[0])
      return failure(text::Access);
    // The provider copies a cached STARTUPINFO and does not consult cb.
    // dwFlags stays clear, so the CRT uses GetStdHandle rather than these
    // invalid placeholders.
    auto Writable = access(A[0], StartupInfoSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::Access);
    std::vector<uint8_t> Bytes(StartupInfoSize);
    llvm::support::endian::write32le(Bytes.data(), uint32_t(StartupInfoSize));
    llvm::support::endian::write64le(Bytes.data() + StartupInfoInput,
                                     InvalidHandle);
    llvm::support::endian::write64le(
        Bytes.data() + StartupInfoInput + PointerSize, InvalidHandle);
    llvm::support::endian::write64le(
        Bytes.data() + StartupInfoInput + 2 * PointerSize, InvalidHandle);
    if (auto E = CPU.write(A[0], Bytes))
      return std::move(E);
    return std::optional<uint64_t>(0);
  }

  if (S.Kind == API::GetFileType) {
    if (A[0] == StandardInput || A[0] == StandardOutput ||
        A[0] == StandardError)
      return WinError(ErrorSuccess, FileTypeChar);
    return WinError(ErrorInvalidHandle, 0);
  }
  if (S.Kind == API::SetHandleCount)
    return std::optional<uint64_t>(uint32_t(A[0]));
  if (S.Kind == API::GetACP)
    return std::optional<uint64_t>(CodePageACP);
  if (S.Kind == API::IsValidCodePage)
    return std::optional<uint64_t>(
        uint32_t(A[0]) == 0 || uint32_t(A[0]) == CodePageACP ? 1 : 0);

  if (S.Kind == API::GetCPInfo) {
    if ((uint32_t(A[0]) != 0 && uint32_t(A[0]) != CodePageACP) || !A[1])
      return WinError(ErrorInvalidParameter, 0);
    auto Writable = access(A[1], CPInfoSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::Access);
    std::array<uint8_t, CPInfoSize> Info{};
    llvm::support::endian::write32le(Info.data(), 1);
    Info[4] = '?';
    if (auto E = CPU.write(A[1], Info))
      return std::move(E);
    return std::optional<uint64_t>(1);
  }

  if (S.Kind == API::GetStringTypeW) {
    if (uint32_t(A[0]) != StringTypeCType)
      return WinError(ErrorInvalidParameter, 0);
    auto Text = countedWide(A[1], A[2]);
    if (!Text)
      return Text.takeError();
    if (Text->empty())
      return std::optional<uint64_t>(1);
    const uint64_t Bytes = Text->size() * WideSize;
    auto Writable = access(A[3], Bytes, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::Access);
    std::vector<uint8_t> Raw(Bytes);
    for (size_t I = 0; I < Text->size(); ++I)
      llvm::support::endian::write16le(Raw.data() + I * WideSize,
                                       ctype1((*Text)[I]));
    if (auto E = CPU.write(A[3], Raw))
      return std::move(E);
    return std::optional<uint64_t>(1);
  }

  const bool WideToMulti = S.Kind == API::WideCharToMultiByte;
  const bool MultiToWide = S.Kind == API::MultiByteToWideChar;
  if (WideToMulti || MultiToWide) {
    if (uint32_t(A[0]) != 0 && uint32_t(A[0]) != CodePageACP)
      return Refuse("code page");
    const uint32_t Flags = uint32_t(A[1]);
    if (WideToMulti ? Flags != 0 : Flags > 1)
      return Refuse("flags");
    const uint32_t OutCap = uint32_t(A[5]);
    if (WideToMulti) {
      auto Text = countedWide(A[2], A[3]);
      if (!Text)
        return Text.takeError();
      if (!OutCap)
        return std::optional<uint64_t>(Text->size());
      if (!A[4])
        return WinError(ErrorInvalidParameter, 0);
      if (OutCap < Text->size())
        return WinError(ErrorInsufficientBuffer, 0);
      uint8_t Default = '?';
      if (A[6]) {
        auto Readable = access(A[6], 1, Read);
        if (!Readable)
          return Readable.takeError();
        if (!*Readable)
          return failure(text::Access);
        auto Byte = CPU.readInteger(A[6], 1);
        if (!Byte)
          return Byte.takeError();
        Default = static_cast<uint8_t>(*Byte);
      }
      std::vector<uint8_t> Out(Text->size());
      bool UsedDefault = false;
      for (size_t I = 0; I < Text->size(); ++I) {
        uint8_t Byte = 0;
        if (encode1252((*Text)[I], Byte))
          Out[I] = Byte;
        else {
          Out[I] = Default;
          UsedDefault = true;
        }
      }
      auto Writable = access(A[4], Out.size(), Write);
      if (!Writable)
        return Writable.takeError();
      if (!*Writable)
        return failure(text::Access);
      if (auto E = CPU.write(A[4], Out))
        return std::move(E);
      if (A[7]) {
        auto Flag = access(A[7], DWordSize, Write);
        if (!Flag)
          return Flag.takeError();
        if (!*Flag)
          return failure(text::Access);
        if (auto E = CPU.writeInteger(A[7], UsedDefault ? 1 : 0, DWordSize))
          return std::move(E);
      }
      return std::optional<uint64_t>(Out.size());
    }
    auto Text = countedBytes(A[2], A[3]);
    if (!Text)
      return Text.takeError();
    if (!OutCap)
      return std::optional<uint64_t>(Text->size());
    if (!A[4])
      return WinError(ErrorInvalidParameter, 0);
    if (OutCap < Text->size())
      return WinError(ErrorInsufficientBuffer, 0);
    std::u16string Wide(Text->size(), u'\0');
    for (size_t I = 0; I < Text->size(); ++I)
      Wide[I] = decode1252(static_cast<uint8_t>((*Text)[I]));
    if (auto E = writeUnits(A[4], Wide))
      return std::move(E);
    return std::optional<uint64_t>(Wide.size());
  }

  if (S.Kind == API::LCMapStringW) {
    const uint32_t Flags = uint32_t(A[1]);
    if (Flags != MapLowercase && Flags != MapUppercase)
      return Refuse("flags");
    auto Text = countedWide(A[2], A[3]);
    if (!Text)
      return Text.takeError();
    for (char16_t &Unit : *Text)
      Unit = foldCase(Unit, Flags == MapUppercase);
    const uint32_t OutCap = uint32_t(A[5]);
    if (!OutCap)
      return std::optional<uint64_t>(Text->size());
    if (!A[4])
      return WinError(ErrorInvalidParameter, 0);
    if (OutCap < Text->size())
      return WinError(ErrorInsufficientBuffer, 0);
    if (auto E = writeUnits(A[4], *Text))
      return std::move(E);
    return std::optional<uint64_t>(Text->size());
  }

  if (S.Kind == API::GetModuleFileNameA) {
    if (A[0] || Modules.Identities.empty())
      return Refuse(A[0] ? "module" : "name");
    const std::string &Name = Modules.Identities.front().Name;
    const uint32_t Cap = uint32_t(A[2]);
    if (!A[1] || !Cap)
      return WinError(ErrorInsufficientBuffer, 0);
    auto Writable = access(A[1], Cap, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::Access);
    if (Name.size() + 1 > Cap) {
      std::vector<uint8_t> Bytes(Name.begin(), Name.begin() + Cap);
      if (auto E = CPU.write(A[1], Bytes))
        return std::move(E);
      return WinError(ErrorInsufficientBuffer, Cap);
    }
    std::vector<uint8_t> Bytes(Name.size() + 1);
    for (size_t I = 0; I < Name.size(); ++I)
      Bytes[I] = static_cast<uint8_t>(Name[I]);
    if (auto E = CPU.write(A[1], Bytes))
      return std::move(E);
    return WinError(ErrorSuccess, Name.size());
  }
  return unsupported(S);
}

llvm::Expected<std::optional<uint64_t>>
Services::toolhelp(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto WinError = [&](uint32_t Code,
                      uint64_t Value =
                          0) -> llvm::Expected<std::optional<uint64_t>> {
    auto V = error(Code, Value);
    if (!V)
      return V.takeError();
    return std::optional<uint64_t>(*V);
  };
  auto Refuse =
      [&](const llvm::Twine &Why) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        std::string(text::ServiceArguments) + S.Name + ": " + Why.str();
    return std::nullopt;
  };

  if (S.Kind == API::RtlSetThreadErrorMode) {
    if (uint32_t(A[0]) & ~uint32_t(ThreadErrorModeMask))
      return Refuse("mode");
    if (A[1]) {
      auto Writable = access(A[1], DWordSize, Write);
      if (!Writable)
        return Writable.takeError();
      if (!*Writable)
        return failure(text::Access);
      if (auto E = CPU.writeInteger(A[1], ThreadErrorMode, DWordSize))
        return std::move(E);
    }
    ThreadErrorMode = uint32_t(A[0]);
    return std::optional<uint64_t>(0);
  }

  if (S.Kind == API::GetSystemInfo) {
    if (!A[0])
      return failure(text::Access);
    auto Writable = access(A[0], SystemInfoSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return failure(text::Access);
    std::array<uint8_t, SystemInfoSize> Info{};
    using namespace llvm::support::endian;
    write16le(Info.data(), ProcessorAmd64);
    write32le(Info.data() + 4, PageSize);
    write64le(Info.data() + 8, ImageAlignment);
    write64le(Info.data() + 0x10, UserLimit - 1);
    write64le(Info.data() + 0x18, ProcessorCount);
    write32le(Info.data() + 0x20, ProcessorCount);
    write32le(Info.data() + 0x24, ProcessorTypeAmd64);
    write32le(Info.data() + 0x28, ImageAlignment);
    write16le(Info.data() + 0x2c, 6);
    if (auto E = CPU.write(A[0], Info))
      return std::move(E);
    return std::optional<uint64_t>(0);
  }

  if (S.Kind == API::CreateToolhelp32Snapshot) {
    if (uint32_t(A[0]) != ThreadSnapThread)
      return Refuse("flags");
    if (A[1] && uint32_t(A[1]) != ProcessID)
      return Refuse("process");
    if (ThreadSnapshots.size() >= MaxThreadSnapshots)
      return WinError(ErrorNotEnoughMemory, InvalidHandle);
    const uint64_t Handle = NextThreadSnapshot;
    NextThreadSnapshot += ThreadSnapshotStride;
    ThreadSnapshots.emplace(Handle, false);
    return std::optional<uint64_t>(Handle);
  }

  if (S.Kind == API::CloseHandle) {
    if (ThreadSnapshots.erase(A[0]) || Sections.erase(A[0]) ||
        Files.erase(A[0]))
      return std::optional<uint64_t>(1);
    return WinError(ErrorInvalidHandle, 0);
  }

  auto Found = ThreadSnapshots.find(A[0]);
  if (Found == ThreadSnapshots.end())
    return WinError(ErrorInvalidHandle, 0);
  if (!A[1])
    return failure(text::Access);
  auto Readable = access(A[1], DWordSize, Read | Write);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return failure(text::Access);
  auto Size = CPU.readInteger(A[1], DWordSize);
  if (!Size)
    return Size.takeError();
  if (*Size < ThreadEntrySize || *Size > 0x1000)
    return Refuse(llvm::utohexstr(*Size));
  if (S.Kind == API::Thread32Next) {
    if (!Found->second)
      return WinError(ErrorInvalidParameter, 0);
    return WinError(ErrorNoMoreFiles, 0);
  }
  auto Writable = access(A[1], ThreadEntrySize, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::Access);
  std::array<uint8_t, ThreadEntrySize> Entry{};
  using namespace llvm::support::endian;
  write32le(Entry.data(), uint32_t(*Size));
  write32le(Entry.data() + 8, uint32_t(ThreadID));
  write32le(Entry.data() + 0x0c, uint32_t(ProcessID));
  write32le(Entry.data() + 0x10, uint32_t(BasePriorityNormal));
  if (auto E = CPU.write(A[1], Entry))
    return std::move(E);
  Found->second = true;
  return std::optional<uint64_t>(1);
}
} // namespace neverd::emulation::windows_process
