//===- DebugFileIdentity.cpp - Which build a debug file is of -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/debug/DebugFileIdentity.h"

#include "neverd/support/FilePath.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/BuildID.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MemoryBuffer.h"

namespace neverd {

namespace {
/// The section an ELF image names its companion debug file in.
constexpr llvm::StringLiteral DebugLinkSection(".gnu_debuglink");
} // namespace

bool isTripleArchOf(llvm::Triple::ArchType TripleArch, Arch A) {
  switch (A) {
  case Arch::X64:
    return TripleArch == llvm::Triple::x86_64;
  case Arch::AArch64:
    return TripleArch == llvm::Triple::aarch64 ||
           TripleArch == llvm::Triple::aarch64_be;
  case Arch::X86:
    return TripleArch == llvm::Triple::x86;
  case Arch::ARM:
    return TripleArch == llvm::Triple::arm ||
           TripleArch == llvm::Triple::armeb ||
           TripleArch == llvm::Triple::thumb ||
           TripleArch == llvm::Triple::thumbeb;
  default:
    return false;
  }
}

std::vector<uint8_t>
recordedBuildIdentity(const llvm::object::ObjectFile &Obj) {
  if (const auto *MachO = llvm::dyn_cast<llvm::object::MachOObjectFile>(&Obj)) {
    const llvm::ArrayRef<uint8_t> UUID = MachO->getUuid();
    return {UUID.begin(), UUID.end()};
  }
  const llvm::object::BuildIDRef Identity = llvm::object::getBuildID(&Obj);
  return {Identity.begin(), Identity.end()};
}

std::optional<DebugLink> elfDebugLink(const llvm::object::ObjectFile &Obj) {
  if (!Obj.isELF())
    return std::nullopt;
  for (const llvm::object::SectionRef &Section : Obj.sections()) {
    auto Name = Section.getName();
    if (!Name) {
      llvm::consumeError(Name.takeError());
      continue;
    }
    if (*Name != DebugLinkSection)
      continue;
    auto Contents = Section.getContents();
    if (!Contents) {
      llvm::consumeError(Contents.takeError());
      return std::nullopt;
    }
    // The file name, NUL-terminated and padded to four bytes, then the CRC.
    const size_t End = Contents->find('\0');
    if (End == llvm::StringRef::npos || End == 0)
      return std::nullopt;
    const size_t CRCAt = llvm::alignTo(End + 1, 4);
    if (CRCAt + 4 > Contents->size())
      return std::nullopt;
    DebugLink Link;
    Link.FileName = Contents->take_front(End).str();
    Link.CRC = Obj.isLittleEndian()
                   ? llvm::support::endian::read32le(Contents->data() + CRCAt)
                   : llvm::support::endian::read32be(Contents->data() + CRCAt);
    return Link;
  }
  return std::nullopt;
}

std::string buildIdentityHex(llvm::ArrayRef<uint8_t> Identity) {
  return llvm::toHex(Identity, /*LowerCase=*/true);
}

llvm::Expected<OpenedObject> openObjectFile(const std::filesystem::path &Path,
                                            std::optional<Arch> Slice) {
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return llvm::errorCodeToError(Buffer.getError());
  OpenedObject Opened;
  Opened.Buffer = std::move(*Buffer);
  const llvm::MemoryBufferRef Bytes = Opened.Buffer->getMemBufferRef();
  auto Universal = llvm::object::MachOUniversalBinary::create(Bytes);
  if (!Universal) {
    llvm::consumeError(Universal.takeError());
    auto Object = llvm::object::ObjectFile::createObjectFile(Bytes);
    if (!Object)
      return Object.takeError();
    Opened.Object = std::move(*Object);
    return std::move(Opened);
  }
  for (const auto &Member : (*Universal)->objects()) {
    auto Object = Member.getAsObjectFile();
    if (!Object) {
      llvm::consumeError(Object.takeError());
      continue;
    }
    const bool Wanted = Slice && isTripleArchOf((*Object)->getArch(), *Slice);
    if (Wanted || !Opened.Object)
      Opened.Object = std::move(*Object);
    if (Wanted)
      break;
  }
  return std::move(Opened);
}

} // namespace neverd
