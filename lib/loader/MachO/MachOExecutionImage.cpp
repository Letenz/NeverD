//===- MachOExecutionImage.cpp - Original Mach-O execution facts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/loader/MachO/MachOExecutionImage.h"

#include "neverd/loader/BinaryImageFlags.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <cstring>

namespace neverd {
namespace {
using namespace llvm::MachO;
llvm::Error failure(llvm::StringRef Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
llvm::Expected<std::vector<uint8_t>> readFile(const std::filesystem::path &Path,
                                              uint64_t FileByteLimit) {
  const auto Filename = Path.string();
  if (Filename.empty() || Filename.find('\0') != std::string::npos)
    return failure("macho execution: invalid input path");
  // Reject ordinary non-file inputs before opening, then validate the opened
  // handle again so path replacement cannot bypass the size/type checks.
  llvm::sys::fs::file_status Status;
  if (auto EC = llvm::sys::fs::status(Filename, Status))
    return llvm::errorCodeToError(EC);
  if (!llvm::sys::fs::is_regular_file(Status))
    return failure("macho execution: input must be a regular file");
  auto Opened = llvm::sys::fs::openNativeFileForRead(Filename);
  if (!Opened)
    return Opened.takeError();
  auto FD = *Opened;
  auto Close = llvm::scope_exit([&] { llvm::sys::fs::closeFile(FD); });
  if (auto EC = llvm::sys::fs::status(FD, Status))
    return llvm::errorCodeToError(EC);
  if (!llvm::sys::fs::is_regular_file(Status))
    return failure("macho execution: input must be a regular file");
  const uint64_t Size = Status.getSize();
  if (Size > FileByteLimit || Size > SIZE_MAX)
    return failure("macho execution: input exceeds the file byte limit");
  std::vector<uint8_t> Bytes(Size);
  size_t Read = 0;
  while (Read < Bytes.size()) {
    auto Count = llvm::sys::fs::readNativeFile(
        FD,
        {reinterpret_cast<char *>(Bytes.data() + Read), Bytes.size() - Read});
    if (!Count)
      return Count.takeError();
    if (!*Count)
      return failure("macho execution: input changed while reading");
    Read += *Count;
  }
  if (auto EC = llvm::sys::fs::status(FD, Status))
    return llvm::errorCodeToError(EC);
  if (Status.getSize() != Size)
    return failure("macho execution: input changed while reading");
  return Bytes;
}
std::string name(const char (&Bytes)[16]) {
  return llvm::StringRef(Bytes, 16).split('\0').first.str();
}
llvm::Expected<std::string>
commandString(const llvm::object::MachOObjectFile::LoadCommandInfo &LC,
              uint32_t Offset, uint32_t Minimum) {
  if (Offset < Minimum || Offset >= LC.C.cmdsize)
    return failure("macho execution: invalid load-command string offset");
  llvm::StringRef Text(LC.Ptr + Offset, LC.C.cmdsize - Offset);
  if (!Text.contains('\0'))
    return failure("macho execution: unterminated load-command string");
  return Text.split('\0').first.str();
}
bool entryOnlyThread(const llvm::object::MachOObjectFile::LoadCommandInfo &LC,
                     bool X64) {
  using namespace llvm::support::endian;
  const uint64_t StateSize =
      X64 ? sizeof(x86_thread_state64_t) : sizeof(arm_thread_state64_t);
  if (LC.C.cmdsize != 16 + StateSize ||
      read32le(LC.Ptr + 8) !=
          (X64 ? uint32_t(x86_THREAD_STATE64) : uint32_t(ARM_THREAD_STATE64)) ||
      read32le(LC.Ptr + 12) != StateSize / 4)
    return false;
  const size_t PCOffset = X64 ? offsetof(x86_thread_state64_t, rip)
                              : offsetof(arm_thread_state64_t, pc);
  for (size_t I = 0; I < StateSize; ++I)
    if ((I < PCOffset || I >= PCOffset + 8) && LC.Ptr[16 + I])
      return false;
  return true;
}
} // namespace

llvm::Expected<MachOExecutionImage>
loadMachOExecutionImage(const std::filesystem::path &Path) {
  return loadMachOExecutionImage(Path, 64 * 1024 * 1024);
}

llvm::Expected<MachOExecutionImage>
loadMachOExecutionImage(const std::filesystem::path &Path,
                        uint64_t FileByteLimit) {
  auto Bytes = readFile(Path, FileByteLimit);
  if (!Bytes)
    return Bytes.takeError();
  MachOExecutionImage Result;
  auto &Image = Result.Image;
  Image.Raw = std::move(*Bytes);
  const llvm::StringRef Input(reinterpret_cast<const char *>(Image.Raw.data()),
                              Image.Raw.size());
  const auto Filename = Path.string();
  auto Object = llvm::object::ObjectFile::createMachOObjectFile(
      llvm::MemoryBufferRef(Input, Filename));
  if (!Object)
    return Object.takeError();
  const auto &Obj = **Object;
  if (!Obj.is64Bit() || !Obj.isLittleEndian())
    return failure(
        "macho execution: a thin little-endian 64-bit image is required");
  const auto Header = Obj.getHeader64();
  Result.CPUType = Header.cputype;
  Result.CPUSubtype = Header.cpusubtype;
  Result.FileType = Header.filetype;
  Result.Flags = Header.flags;
  Image.Arch = Header.cputype == CPU_TYPE_X86_64  ? Arch::X64
               : Header.cputype == CPU_TYPE_ARM64 ? Arch::AArch64
                                                  : Arch::Unknown;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  uint64_t HeaderAddress = 0;
  unsigned HeaderSegments = 0;
  for (const auto &LC : Obj.load_commands()) {
    switch (LC.C.cmd) {
    case LC_SEGMENT_64: {
      const auto S = Obj.getSegment64LoadCommand(LC);
      if (S.vmsize > UINT64_MAX - S.vmaddr || S.filesize > S.vmsize ||
          S.fileoff > Image.Raw.size() ||
          S.filesize > Image.Raw.size() - S.fileoff)
        return failure("macho execution: invalid segment extent");
      Segment Segment;
      Segment.Name = name(S.segname);
      Segment.VA = S.vmaddr;
      Segment.Size = S.vmsize;
      Segment.FileOff = S.fileoff;
      Segment.FileSz = S.filesize;
      Segment.Flags = machoProtToNd(S.initprot);
      Image.Segments.push_back(std::move(Segment));
      Result.Segments.push_back({S.initprot, S.maxprot, S.flags});
      if (!S.fileoff && S.filesize) {
        if (S.filesize < sizeof(mach_header_64) + uint64_t(Header.sizeofcmds))
          return failure("macho execution: header is not fully mapped");
        HeaderAddress = S.vmaddr;
        ++HeaderSegments;
      }
      for (uint32_t I = 0; I < S.nsects; ++I) {
        const auto Section = Obj.getSection64(LC, I);
        if (Section.addr < S.vmaddr || Section.size > S.vmsize ||
            Section.addr - S.vmaddr > S.vmsize - Section.size)
          return failure("macho execution: section escapes its segment");
        const auto Type = Section.flags & SECTION_TYPE;
        Result.HasInitializers |=
            Section.size &&
            (Type == S_MOD_INIT_FUNC_POINTERS ||
             Type == S_MOD_TERM_FUNC_POINTERS || Type == S_INIT_FUNC_OFFSETS);
        Result.HasTLS |= Section.size && Type >= S_THREAD_LOCAL_REGULAR &&
                         Type <= S_THREAD_LOCAL_INIT_FUNCTION_POINTERS;
        Result.HasFixups |= Section.nreloc != 0;
      }
      break;
    }
    case LC_MAIN: {
      ++Result.MainEntries;
      Result.RequestedStackSize = Obj.getEntryPointCommand(LC).stacksize;
      break;
    }
    case LC_UNIXTHREAD:
      ++Result.ThreadEntries;
      Result.HasNonEntryThreadState |=
          !entryOnlyThread(LC, Image.Arch == Arch::X64);
      break;
    case LC_BUILD_VERSION:
      Result.Platforms.push_back(Obj.getBuildVersionLoadCommand(LC).platform);
      break;
    case LC_VERSION_MIN_MACOSX:
      Result.Platforms.push_back(PLATFORM_MACOS);
      break;
    case LC_VERSION_MIN_IPHONEOS:
      // Legacy Intel iOS load commands denote a simulator binary.
      Result.Platforms.push_back(Image.Arch == Arch::X64 ? PLATFORM_IOSSIMULATOR
                                                         : PLATFORM_IOS);
      break;
    case LC_LOAD_DYLINKER: {
      auto Text = commandString(LC, Obj.getDylinkerCommand(LC).name,
                                sizeof(dylinker_command));
      if (!Text)
        return Text.takeError();
      Result.Interpreters.push_back(std::move(*Text));
      break;
    }
    case LC_LOAD_DYLIB:
    case LC_LOAD_WEAK_DYLIB:
    case LC_REEXPORT_DYLIB:
    case LC_LOAD_UPWARD_DYLIB:
    case LC_LAZY_LOAD_DYLIB: {
      const auto D = Obj.getDylibIDLoadCommand(LC);
      auto Text = commandString(LC, D.dylib.name, sizeof(dylib_command));
      if (!Text)
        return Text.takeError();
      Result.Dependencies.push_back(std::move(*Text));
      break;
    }
    case LC_DYLD_INFO:
    case LC_DYLD_INFO_ONLY: {
      const auto D = Obj.getDyldInfoLoadCommand(LC);
      Result.HasFixups |=
          D.rebase_size || D.bind_size || D.weak_bind_size || D.lazy_bind_size;
      break;
    }
    case LC_DYLD_CHAINED_FIXUPS:
      Result.HasFixups |= Obj.getLinkeditDataLoadCommand(LC).datasize != 0;
      break;
    case LC_DYSYMTAB: {
      const auto D = Obj.getDysymtabLoadCommand();
      Result.HasFixups |= D.nundefsym || D.nextrel || D.nlocrel;
      break;
    }
    case LC_ENCRYPTION_INFO_64:
      Result.Encrypted |= Obj.getEncryptionInfoCommand64(LC).cryptid != 0;
      break;
    case LC_SYMTAB:
    case LC_UUID:
    case LC_CODE_SIGNATURE:
    case LC_FUNCTION_STARTS:
    case LC_DATA_IN_CODE:
    case LC_SOURCE_VERSION:
    case LC_DYLD_EXPORTS_TRIE:
    case LC_LINKER_OPTIMIZATION_HINT:
      break;
    default:
      Result.OtherCommands.push_back(LC.C.cmd);
      break;
    }
  }
  if (HeaderSegments != 1)
    return failure("macho execution: image requires one mapped Mach header");
  Image.Base = HeaderAddress;
  macho_loader::parseEntryPoint(Obj, Image, HeaderAddress);
  return Result;
}
} // namespace neverd
