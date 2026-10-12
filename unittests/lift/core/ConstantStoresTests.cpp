//===- ConstantStoresTests.cpp - Native initializer projection guards ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent x64 encodings, exact values and refusal of unknown effects.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/pipeline/ConstantStores.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/MemoryBuffer.h"

#include <array>
#include <cstdlib>

using namespace neverd;

namespace {
BinaryImage fixture() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Image.Entry = 0x1000;
  Segment Code;
  Code.VA = 0x1000;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  const uint8_t Bytes[] = {
      0x48, 0x8d, 0x05, 0xf9, 0x0f, 0x00, 0x00,    // lea rax, [rip+0xff9]
      0x49, 0x89, 0x45, 0x10,                      // mov [r13+16], rax
      0x49, 0xc7, 0x45, 0x18, 3,    0,    0,    0, // mov qword [r13+24], 3
      0x48, 0x8d, 0x05, 0x26, 0x10, 0x00, 0x00,    // lea rax, [rip+0x1026]
      0x49, 0x89, 0x45, 0x20,                      // mov [r13+32], rax
      0x49, 0xc7, 0x45, 0x28, 15,   0,    0,    0, // mov qword [r13+40], 15
      0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
      0x22, 0x11, 0x49, 0x89, 0x45, 0x30, // mov [r13+48], rax
      0xc3};
  Code.Data.assign(std::begin(Bytes), std::end(Bytes));
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(Code);
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 0x100;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(0x100);
  Image.Segments.push_back(Data);
  Image.Symbols.push_back({"inert_initializer", 0x1000, Code.Size, true});
  return Image;
}
std::string refusal(const BinaryImage &Image, va_t Start = 0x1000,
                    uint32_t Count = 5) {
  auto R = projectNativeConstantStores(Image, Start, Count);
  if (R) {
    ADD_FAILURE() << "Expected atomic refusal";
    return {};
  }
  return llvm::toString(R.takeError());
}
} // namespace

TEST(NativeConstantStores, FiveFieldsComeFromLowIRWithExactAddressRoles) {
  const auto Image = fixture();
  auto R = projectNativeConstantStores(Image, 0x1000, 5);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Stores.size(), 5u);
  const std::array<uint64_t, 5> Values{0x2000, 3, 0x2040, 15,
                                       0x1122334455667788ULL};
  for (size_t I = 0; I < Values.size(); ++I)
    EXPECT_EQ(R->Stores[I].Value, Values[I]);
  EXPECT_TRUE(isExactAddressProvenance(R->Stores[0].Provenance));
  EXPECT_TRUE(isExactAddressProvenance(R->Stores[2].Provenance));
  EXPECT_FALSE(isAddressProvenance(R->Stores[1].Provenance));
  EXPECT_FALSE(isAddressProvenance(R->Stores[4].Provenance));
  EXPECT_EQ(R->BaseOffset, 16);
  EXPECT_EQ(R->Start, 0x1000u);
  EXPECT_EQ(R->End, 0x1034u);
  EXPECT_EQ(R->Instructions, 8u);
}

TEST(NativeConstantStores, ForeignArchitectureAndInstructionBoundariesRefuse) {
  auto Image = fixture();
  EXPECT_EQ(refusal(Image, 0x1001),
            "native_store_instruction_boundary_required");
  EXPECT_EQ(refusal(Image, 0x1000, 0), "native_store_profile_unavailable");
  EXPECT_EQ(refusal(Image, 0x1000, 17), "native_store_profile_unavailable");
  Image.Arch = Arch::AArch64;
  EXPECT_EQ(refusal(Image), "native_store_profile_unavailable");
  Image = fixture();
  Image.Symbols.clear();
  EXPECT_EQ(refusal(Image), "native_store_function_entry_unavailable");
  Image = fixture();
  Image.Symbols[0].IsBoundaryGuess = true;
  EXPECT_EQ(refusal(Image), "native_store_function_entry_unavailable");
  Image = fixture();
  Image.Bits = Bitness::Bits32;
  EXPECT_EQ(refusal(Image), "native_store_profile_unavailable");
  Image = fixture();
  Image.Segments[0].VA = UINT64_MAX - 5;
  Image.Symbols[0].Addr = UINT64_MAX - 5;
  EXPECT_EQ(refusal(Image, UINT64_MAX - 5),
            "native_store_invalid_instruction_range");
}

TEST(NativeConstantStores, EffectsFieldGapsAndFunctionBoundariesRefuse) {
  auto Image = fixture();
  Image.Segments[0].Data[29] = 0x28; // third store skips a field
  EXPECT_EQ(refusal(Image), "native_store_nonconsecutive_fields");
  Image = fixture();
  Image.Segments[0].Data[30] = 0xe8; // call replaces the next constant store
  EXPECT_FALSE(refusal(Image).empty());
  Image = fixture();
  Image.Symbols.push_back({"foreign_entry", 0x1013, 34, true});
  EXPECT_EQ(refusal(Image), "native_store_function_boundary_crossed");
  Image = fixture();
  Image.Segments[0].Data.resize(7);
  EXPECT_FALSE(refusal(Image).empty());
}

TEST(NativeConstantStores,
     NumericAddressLookingBitsNeverGainAddressProvenance) {
  auto Image = fixture();
  const std::vector<uint8_t> Bytes{0x48, 0xb8, 0x00, 0x20, 0, 0,
                                   0,    0,    0,    0, // movabs rax, 0x2000
                                   0x49, 0x89, 0x45, 0x10};
  Image.Segments[0].Data = Bytes;
  Image.Segments[0].Size = Bytes.size();
  auto R = projectNativeConstantStores(Image, 0x1000, 1);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Stores.size(), 1u);
  EXPECT_EQ(R->Stores[0].Value, 0x2000u);
  EXPECT_FALSE(isAddressProvenance(R->Stores[0].Provenance));
}

TEST(NativeConstantStores,
     OptionalChatGPT110X64InitializerHasFivePinnedFields) {
  const auto *Path = std::getenv("NEVERD_TAURI_CHATGPT_110_X64_NATIVE");
  if (!Path)
    GTEST_SKIP() << "preserved public Tauri qualification input unavailable";
  auto Buffer = llvm::MemoryBuffer::getFile(Path, false, false);
  ASSERT_TRUE(bool(Buffer));
  auto Image = loadBinaryBuffer((*Buffer)->getMemBufferRef());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->InputFileSHA256.has_value());
  EXPECT_EQ(llvm::toHex(*Image->InputFileSHA256, true),
            "351cffd71cc63205dda57990b1432d124150bd625ae886fc577a03e49e9bed92");
  auto R = projectNativeConstantStores(*Image, 0x1000135a8, 5);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Stores.size(), 5u);
  const std::array<uint64_t, 5> Values{0x10048e484, 3, 0x100808058, 15,
                                       0xb33780d1db3dcb27ULL};
  for (size_t I = 0; I < Values.size(); ++I)
    EXPECT_EQ(R->Stores[I].Value, Values[I]);
  EXPECT_EQ(R->End, 0x1000135dcu);
  EXPECT_EQ(R->Instructions, 8u);
  EXPECT_TRUE(isExactAddressProvenance(R->Stores[0].Provenance));
  EXPECT_TRUE(isExactAddressProvenance(R->Stores[2].Provenance));
}
