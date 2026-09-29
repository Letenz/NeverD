//===- InputDigestTests.cpp - SHA-256 of a loaded input file --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/InputDigest.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <cstdint>
#include <random>
#include <vector>

using namespace neverd;

namespace {

TEST(InputDigest, KnownDigests) {
  EXPECT_EQ(llvm::toHex(sha256({}), /*LowerCase=*/true),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const uint8_t ABC[] = {'a', 'b', 'c'};
  EXPECT_EQ(llvm::toHex(sha256(ABC), /*LowerCase=*/true),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(InputDigest, AgreesWithLLVMAcrossEveryPaddingBoundary) {
  // Every length up to three blocks, so the message ends at every offset of
  // a block and its padding takes one block or two.
  std::mt19937 Random(0x5EED);
  std::vector<uint8_t> Data(3 * 64 + 1);
  for (uint8_t &Byte : Data)
    Byte = static_cast<uint8_t>(Random());
  for (size_t Size = 0; Size <= Data.size(); ++Size) {
    const llvm::ArrayRef<uint8_t> Prefix(Data.data(), Size);
    EXPECT_EQ(sha256(Prefix), llvm::SHA256::hash(Prefix)) << Size;
  }
}

TEST(InputDigest, AgreesWithLLVMOnALargeInput) {
  std::mt19937 Random(0xB16);
  std::vector<uint8_t> Data((1 << 20) + 37);
  for (uint8_t &Byte : Data)
    Byte = static_cast<uint8_t>(Random());
  EXPECT_EQ(sha256(Data), llvm::SHA256::hash(Data));
}

} // namespace
