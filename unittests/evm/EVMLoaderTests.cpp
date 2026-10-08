//===- EVMLoaderTests.cpp - EVM BinaryImage loader tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <filesystem>
#include <fstream>

namespace neverd {
namespace {

class EVMLoaderTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> UniqueDirectory;
    const std::error_code Error = llvm::sys::fs::createUniqueDirectory(
        "neverd-evm-loader", UniqueDirectory);
    ASSERT_FALSE(Error) << Error.message();
    Directory = UniqueDirectory.c_str();
  }

  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Directory, EC);
  }

  std::filesystem::path write(llvm::StringRef Name,
                              llvm::StringRef Contents) const {
    const auto Path = Directory / Name.str();
    std::ofstream Output(Path, std::ios::binary);
    Output.write(Contents.data(),
                 static_cast<std::streamsize>(Contents.size()));
    Output.close();
    return Path;
  }

  std::filesystem::path Directory;
};

TEST_F(EVMLoaderTest, AutoDetectsAndBuildsAnEVMImage) {
  const auto Path = write("counter.evm", "0x6001600055");
  auto Image = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());

  EXPECT_EQ(Image->Arch, Arch::EVM);
  EXPECT_EQ(Image->Format, BinaryFormat::EVM);
  EXPECT_EQ(Image->Bits, Bitness::Bits256);
  EXPECT_EQ(Image->getPointerSize(), 32u);
  EXPECT_EQ(Image->getFormatName(), std::string("EVM"));
  EXPECT_EQ(Image->Entry, 0u);
  EXPECT_EQ(Image->Raw, (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x00, 0x55}));

  ASSERT_EQ(Image->Segments.size(), 1u);
  EXPECT_EQ(Image->Segments[0].Name, "EVM_CODE");
  EXPECT_TRUE(Image->Segments[0].isReadable());
  EXPECT_TRUE(Image->Segments[0].isExecutable());
  ASSERT_NE(Image->getTextSection(), nullptr);
  EXPECT_EQ(Image->getTextSection()->Name, ".evm.code");
  ASSERT_EQ(Image->getFunctionSymbols().size(), 1u);
  EXPECT_EQ(Image->getFunctionSymbols()[0]->Name, "evm_entry");
}

TEST_F(EVMLoaderTest, KeepsTheContainerAndWhatWasLearnedFromIt) {
  // A constructor that returns five bytes, followed by those bytes and the
  // trailer naming the compiler.
  const auto Path = write("counter.creation.evm", "0x6005600c60003960056000f3"
                                                  "6001600055"
                                                  "a164736f6c634300081e000a");
  auto Image = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());

  // The image keeps the container. Which of its bytes are code depends on the
  // fork a session picks, and the loader is not the place that decides.
  EXPECT_EQ(Image->Raw.size(), 29u);
  ASSERT_EQ(Image->Segments.size(), 1u);
  EXPECT_EQ(Image->Segments[0].Data,
            (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x00, 0x55}));

  ASSERT_TRUE(Image->EVM.has_value());
  EXPECT_EQ(Image->EVM->Source, evm::BytecodeSourceKind::Hex);
  EXPECT_EQ(Image->EVM->Container, evm::BytecodeContainer::Legacy);
  EXPECT_TRUE(Image->EVM->RuntimeExtracted);
  // The constructor returns only the five code bytes, so its trailing compiler
  // map belongs to the input container rather than to the deployed runtime.
  EXPECT_FALSE(Image->EVM->MetadataStripped);
  ASSERT_TRUE(Image->EVM->InputMetadata.has_value());
  EXPECT_EQ(Image->EVM->InputMetadata->compilerVersion(), "0.8.30");
  EXPECT_FALSE(Image->EVM->RuntimeMetadata.has_value());
}

TEST_F(EVMLoaderTest, ReportsADelegationIndicatorInsteadOfDecodingIt) {
  const auto Path = write("delegated.evm",
                          "0xef01003333333333333333333333333333333333333333");
  auto Image = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());

  ASSERT_TRUE(Image->EVM.has_value());
  EXPECT_EQ(Image->EVM->Container, evm::BytecodeContainer::Delegation);
  // The address is the one thing these bytes say, so loading has to keep it:
  // it is where a reader has to go to find the code that actually runs.
  EXPECT_EQ(Image->EVM->DelegateTarget.size(), evm::kAddressBytes);
  EXPECT_EQ(Image->EVM->DelegateTarget.front(), 0x33);
  EXPECT_FALSE(Image->EVM->RuntimeExtracted);
  EXPECT_FALSE(Image->EVM->MetadataStripped);
}

TEST_F(EVMLoaderTest, ReadsBytesUnderASharedNameOnlyWhenChosen) {
  // A firmware image is as likely a .bin as raw bytecode is.
  const std::string Bytes("\x60\x01\x60\x00\x55\x00\xff\x07", 8);
  const auto Firmware = write("firmware.bin", Bytes);
  EXPECT_EQ(Loader::create(Firmware), nullptr);
  auto Refused = loadBinary(Firmware);
  ASSERT_FALSE(static_cast<bool>(Refused));
  EXPECT_NE(llvm::toString(Refused.takeError()).find("choose its loader"),
            std::string::npos);
  BinaryLoadOptions Chosen;
  Chosen.Choice.Format = BinaryFormat::EVM;
  auto Image = loadBinary(Firmware, Chosen);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Arch, Arch::EVM);
  // A name that says EVM reads the same bytes by itself, and hex text under
  // a shared name decodes as bytecode.
  auto Named = loadBinary(write("contract.evmraw", Bytes));
  ASSERT_TRUE(static_cast<bool>(Named)) << llvm::toString(Named.takeError());
  EXPECT_EQ(Named->Arch, Arch::EVM);
  auto Text = loadBinary(write("contract.bin", "6001600055"));
  ASSERT_TRUE(static_cast<bool>(Text)) << llvm::toString(Text.takeError());
  EXPECT_EQ(Text->Arch, Arch::EVM);
}

TEST_F(EVMLoaderTest, RefusesUnknownTextFiles) {
  const auto Path = write("notes.txt", "hello, not bytecode");
  EXPECT_EQ(Loader::create(Path), nullptr);
}

TEST_F(EVMLoaderTest, DetectsValidatedHexWithoutAnExtension) {
  const auto Path = write("contract", "6001600055");
  auto Image = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Arch, Arch::EVM);
}

TEST_F(EVMLoaderTest, PreservesDetailedErrorsForExplicitEVMFiles) {
  const auto Path = write("broken.evm", "0x600");
  auto Image = loadBinary(Path);
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("odd number"), std::string::npos);
  EXPECT_EQ(Error.find("unknown binary format"), std::string::npos);
}

} // namespace
} // namespace neverd
