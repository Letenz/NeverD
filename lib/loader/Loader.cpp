//===- Loader.cpp - Binary format auto-detection and factory -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the Loader factory methods that auto-detect binary format
/// from file content and instantiate the appropriate format-specific
/// loader.  Follows the LLVM ObjectFile::createObjectFile pattern.
///
//===----------------------------------------------------------------------===//

#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/EVM/EVMLoader.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/ObjectFileUtils.h"

#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>

namespace neverd {

llvm::StringRef getLoadRowLoader(LoadRow Row) {
  switch (Row) {
#define NEVERD_LOAD_ROW(Id, Loader, Text)                                      \
  case LoadRow::Id:                                                            \
    return Loader;
#include "neverd/loader/LoadRows.def"
  }
  llvm_unreachable("unknown load row");
}

llvm::StringRef getLoadRowText(LoadRow Row) {
  switch (Row) {
#define NEVERD_LOAD_ROW(Id, Loader, Text)                                      \
  case LoadRow::Id:                                                            \
    return Text;
#include "neverd/loader/LoadRows.def"
  }
  llvm_unreachable("unknown load row");
}

llvm::StringRef getLoadReasonText(LoadReason Reason) {
  switch (Reason) {
#define NEVERD_LOAD_REASON(Id, Text)                                           \
  case LoadReason::Id:                                                         \
    return Text;
#include "neverd/loader/LoadRows.def"
  }
  llvm_unreachable("unknown load reason");
}

std::vector<LoadCandidate> identifyFile(const std::filesystem::path &Path) {
  std::vector<LoadCandidate> Rows;
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (Buffer) {
    // The loaders Loader::create picks for the file, in its order.
    const llvm::MemoryBufferRef Ref = (*Buffer)->getMemBufferRef();
    switch (magicToFormat(llvm::identify_magic(Ref.getBuffer()))) {
    case BinaryFormat::ELF:
      ELFLoader::identify(Ref, Rows);
      break;
    case BinaryFormat::COFF:
      COFFLoader::identify(Ref, Rows);
      break;
    case BinaryFormat::MachO:
      MachOLoader::identify(Ref, Rows);
      break;
    case BinaryFormat::EVM:
    case BinaryFormat::Unknown:
      // Bytecode text or an artifact shows in the contents; an EVM file
      // name takes any contents.
      if (evm::looksLikeEVMInput(Path))
        EVMLoader::identify(Rows, /*ByName=*/false);
      else if (evm::hasEVMFileExtension(Path))
        EVMLoader::identify(Rows, /*ByName=*/true);
      break;
    }
  }
  std::stable_partition(Rows.begin(), Rows.end(),
                        [](const LoadCandidate &Row) { return Row.First; });
  // Any file can be read as a binary file.
  LoadCandidate Binary;
  Binary.Row = LoadRow::Binary;
  Binary.Description = getLoadRowText(LoadRow::Binary).str();
  Binary.Reason = getLoadReasonText(LoadReason::Binary).str();
  Rows.push_back(std::move(Binary));
  return Rows;
}

std::unique_ptr<Loader> Loader::create(BinaryFormat Format) {
  switch (Format) {
  case BinaryFormat::ELF:
    return std::make_unique<ELFLoader>();
  case BinaryFormat::COFF:
    return std::make_unique<COFFLoader>();
  case BinaryFormat::MachO:
    return std::make_unique<MachOLoader>();
  case BinaryFormat::EVM:
    return std::make_unique<EVMLoader>();
  default:
    return nullptr;
  }
}

std::unique_ptr<Loader> Loader::create(const std::filesystem::path &Path) {
  BinaryFormat Fmt = detectFormat(Path);
  if (Fmt == BinaryFormat::Unknown) {
    if (evm::hasEVMFileExtension(Path) || evm::looksLikeEVMInput(Path))
      Fmt = BinaryFormat::EVM;
    else
      return nullptr;
  }
  return create(Fmt);
}

} // namespace neverd
