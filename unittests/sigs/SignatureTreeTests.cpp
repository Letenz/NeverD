//===- SignatureTreeTests.cpp - The directories of a signature tree -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/SignatureDB.h"

#include <filesystem>
#include <optional>

using namespace neverd;

namespace {

std::optional<std::filesystem::path> treeDirectory(BinaryFormat Format, Arch A,
                                                   Bitness Bits) {
  BinaryImage Img;
  Img.Format = Format;
  Img.Arch = A;
  Img.Bits = Bits;
  return sigs::SignatureDB::treeDirectory(Img);
}

TEST(SignatureTree, NamesTheDirectoryOfAnImagesFormatArchAndWidth) {
  EXPECT_EQ(treeDirectory(BinaryFormat::COFF, Arch::X64, Bitness::Bits64),
            std::filesystem::path("pe/x86/64"));
  EXPECT_EQ(treeDirectory(BinaryFormat::COFF, Arch::X86, Bitness::Bits32),
            std::filesystem::path("pe/x86/32"));
  EXPECT_EQ(treeDirectory(BinaryFormat::ELF, Arch::ARM, Bitness::Bits32),
            std::filesystem::path("elf/arm/32"));
  EXPECT_EQ(treeDirectory(BinaryFormat::MachO, Arch::AArch64, Bitness::Bits64),
            std::filesystem::path("macho/arm/64"));
}

TEST(SignatureTree, HoldsNoDirectoryForOtherImages) {
  EXPECT_EQ(treeDirectory(BinaryFormat::ELF, Arch::EVM, Bitness::Bits64),
            std::nullopt);
  EXPECT_EQ(treeDirectory(BinaryFormat::Unknown, Arch::X64, Bitness::Bits64),
            std::nullopt);
}

} // namespace
