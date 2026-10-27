//===- CompiledSEHStateWriteTests.cpp - SEH placement receipts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks callback state-write identity across rebasing and final placement.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationSEHStateWrites.h"
#include "../../../lib/backend/codegen/X86/CompiledSEHStateWrites.h"
#include "gtest/gtest.h"

#include <vector>

TEST(CompiledSEHStateStoreBytes, BindsEveryArchitecturalBaseRegister) {
  for (uint8_t Register = 0; Register != 8; ++Register) {
    SCOPED_TRACE(Register);
    std::vector<uint8_t> Bytes{0xc7, uint8_t(0x80 | Register)};
    if (Register == 4)
      Bytes.push_back(0x24);
    Bytes.insert(Bytes.end(), {0xd4, 0xff, 0xff, 0xff, 7, 0, 0, 0});
    EXPECT_EQ(neverd::getCOFFRegistrationSEHStateWriteRegister(Bytes, -44, 7),
              Register);
    EXPECT_FALSE(
        neverd::getCOFFRegistrationSEHStateWriteRegister(Bytes, -40, 7));
    EXPECT_FALSE(
        neverd::getCOFFRegistrationSEHStateWriteRegister(Bytes, -44, 8));
  }
  const uint8_t Short[] = {0xc7, 0x45, 0xfc, 0xff, 0xff, 0xff, 0xff};
  EXPECT_EQ(neverd::getCOFFRegistrationSEHStateWriteRegister(Short, -4, -1), 5);
  const uint8_t Zero[] = {0xc7, 0x00, 0xfe, 0xff, 0xff, 0xff};
  EXPECT_EQ(neverd::getCOFFRegistrationSEHStateWriteRegister(Zero, 0, -2), 0);
  const uint8_t SIB[] = {0xc7, 0x04, 0xe4, 0xfe, 0xff, 0xff, 0xff};
  EXPECT_EQ(neverd::getCOFFRegistrationSEHStateWriteRegister(SIB, 0, -2), 4);
}

TEST(CompiledSEHStateStoreBytes, RefusesPrefixesIndexingAndExtraInstructions) {
  const std::vector<uint8_t> Valid{0xc7, 0x80, 0xd4, 0xff, 0xff,
                                   0xff, 7,    0,    0,    0};
  auto Reject = [&](auto Mutate) {
    auto Bytes = Valid;
    Mutate(Bytes);
    EXPECT_FALSE(
        neverd::getCOFFRegistrationSEHStateWriteRegister(Bytes, -44, 7));
  };
  Reject([](auto &B) { B.insert(B.begin(), 0x66); });
  Reject([](auto &B) { B.insert(B.begin(), 0x65); });
  Reject([](auto &B) { B.insert(B.begin(), 0xf0); });
  Reject([](auto &B) { B[0] = 0x89; });
  Reject([](auto &B) { B[1] = 0xc0; });
  Reject([](auto &B) { B[1] = 0x88; });
  Reject([](auto &B) { B[1] = 0x05; });
  Reject([](auto &B) {
    B[1] = 0x84;
    B.insert(B.begin() + 2, 0x0c);
  });
  Reject([](auto &B) { B.push_back(0x90); });
  Reject([](auto &B) { B.pop_back(); });
  const uint8_t Absolute[] = {0xc7, 0x04, 0x25, 0xd4, 0xff, 0xff,
                              0xff, 7,    0,    0,    0};
  EXPECT_FALSE(
      neverd::getCOFFRegistrationSEHStateWriteRegister(Absolute, -44, 7));
}

#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES

#include <algorithm>
#include <limits>

using namespace neverd;
using namespace llvm::mc_rewrite;

namespace {

RewriteResult makeReceipts(bool EH4 = false) {
  RewriteResult Result;
  Result.SourceFunctionOwners = {{"owner", "_owner", 0x4000, false},
                                 {"cleanup", "_cleanup", 0x4100, true}};
  Result.FunctionOwnerAddrs = {{"_owner", 0x4000}, {"_cleanup", 0x4100}};
  Result.FunctionRanges = {
      {11, "_owner", 0x4000, "entry", 0x4000, "entry.end", 0x4080},
      {21, "_cleanup", 0x4100, "cleanup.begin", 0x4100, "cleanup.end", 0x4180}};

  RewriteWinEHSemanticRecord Scope;
  Scope.Token.Region = 31;
  Scope.Token.Digest = {23, 41, 71, 101};
  Scope.SourceFunction = "owner";
  Scope.OwnerSymbol = "_owner";
  Scope.OwnerVA = 0x4000;
  Scope.ContainerSymbol = "scope.table";
  Scope.ContainerVA = 0x4200;
  Scope.ContainerEndSymbol = "scope.end";
  Scope.ContainerEndVA = 0x420c + (EH4 ? 16 : 0);
  Scope.RecordVA = 0x4200 + (EH4 ? 16 : 0);
  Scope.RecordSize = 12;
  Scope.HandlerSymbol = "finally.dispatch";
  Scope.HandlerVA = 0x4010;
  Scope.Encoding = EH4 ? RewriteWinEHSemanticEncoding::X86SEH4
                       : RewriteWinEHSemanticEncoding::X86SEH3;
  Scope.GeneratedState = 0;
  Scope.EnclosingState = EH4 ? -2 : -1;
  if (EH4)
    Scope.RegistrationCookieOffsets = {-2, 0, -40, 0};
  Result.WinEHSemanticRecords.push_back(Scope);
  Result.WinX86SEHFrames.push_back(
      {"owner", "_owner", 0x4000, -64, 24, 20, Scope.Encoding});

  RewriteWinX86SEHStateWrite Enter;
  Enter.WriteID = 73;
  Enter.SourceFunction = "cleanup";
  Enter.OwnerSymbol = "_cleanup";
  Enter.OwnerVA = 0x4100;
  Enter.ParentFrame = Result.WinX86SEHFrames.front();
  Enter.Semantic = Scope.Token;
  Enter.GeneratedState = 0;
  Enter.AddressRegister = 1;
  Enter.BeginSymbol = "enter.begin";
  Enter.BeginVA = 0x4110;
  Enter.EndSymbol = "enter.end";
  Enter.EndVA = 0x4117;
  Result.WinX86SEHStateWrites.push_back(Enter);
  auto Reset = Enter;
  Reset.WriteID = 14;
  Reset.Semantic.reset();
  Reset.GeneratedState = Scope.EnclosingState;
  Reset.BeginSymbol = "reset.begin";
  Reset.BeginVA = 0x4130;
  Reset.EndSymbol = "reset.end";
  Reset.EndVA = 0x4137;
  Result.WinX86SEHStateWrites.push_back(Reset);
  return Result;
}

CompiledImage makeImage(const RewriteResult &Result) {
  CompiledImage Image;
  Image.BaseVA = 0x4000;
  Image.TargetArch = Arch::X86;
  Image.Format = BinaryFormat::COFF;
  Image.PointerWidth = 4;
  Image.Bytes.resize(0x220);
  Image.Sections = {
      {".text", 0, 0x4000, 0x80, 16, RewriteSectionKind::Code},
      {".text$cleanup", 0x100, 0x4100, 0x80, 16, RewriteSectionKind::Code},
      {".xdata", 0x200, 0x4200, 0x20, 4, RewriteSectionKind::ReadOnlyData}};
  Image.FunctionOwnerAddrs = Result.FunctionOwnerAddrs;
  Image.FunctionRanges = Result.FunctionRanges;
  Image.SourceFunctionOwners = Result.SourceFunctionOwners;
  Image.WinEHSemanticRecords = Result.WinEHSemanticRecords;
  Image.WinX86SEHFrames = Result.WinX86SEHFrames;
  Image.WinX86SEHStateWrites = Result.WinX86SEHStateWrites;
  Image.FunctionRangesValid = Result.FunctionRangesValid;
  Image.WinEHSemanticsValid = Result.WinEHSemanticsValid;
  Image.WinX86SEHStateWritesValid = Result.WinX86SEHStateWritesValid;
  return Image;
}

// Change every compiler symbol spelling and every nonzero address while
// retaining source-issued names and state tokens.
void relocateReceipts(RewriteResult &Result, uint64_t Delta) {
  auto Symbol = [](std::string &Name) {
    if (!Name.empty())
      Name = "iteration." + Name;
  };
  auto Address = [&](uint64_t &VA) {
    if (VA)
      VA += Delta;
  };
  std::map<std::string, uint64_t> Owners;
  for (const auto &[Name, VA] : Result.FunctionOwnerAddrs)
    Owners.emplace("iteration." + Name, VA + Delta);
  Result.FunctionOwnerAddrs = std::move(Owners);
  for (auto &Owner : Result.SourceFunctionOwners) {
    Symbol(Owner.OwnerSymbol);
    Address(Owner.OwnerVA);
  }
  for (auto &Range : Result.FunctionRanges) {
    Range.Id += 100;
    Symbol(Range.OwnerSymbol);
    Symbol(Range.BeginSymbol);
    Symbol(Range.EndSymbol);
    Symbol(Range.ParentOwnerSymbol);
    Address(Range.OwnerVA);
    Address(Range.BeginVA);
    Address(Range.EndVA);
    Address(Range.ParentOwnerVA);
  }
  for (auto &Row : Result.WinEHSemanticRecords) {
    Symbol(Row.OwnerSymbol);
    Symbol(Row.ContainerSymbol);
    Symbol(Row.ContainerEndSymbol);
    Symbol(Row.HandlerSymbol);
    Address(Row.OwnerVA);
    Address(Row.ContainerVA);
    Address(Row.ContainerEndVA);
    Address(Row.RecordVA);
    Address(Row.HandlerVA);
  }
  auto Frame = [&](auto &F) {
    Symbol(F.OwnerSymbol);
    Address(F.OwnerVA);
  };
  for (auto &F : Result.WinX86SEHFrames)
    Frame(F);
  for (auto &Write : Result.WinX86SEHStateWrites) {
    Symbol(Write.OwnerSymbol);
    Symbol(Write.BeginSymbol);
    Symbol(Write.EndSymbol);
    Address(Write.OwnerVA);
    Address(Write.BeginVA);
    Address(Write.EndVA);
    Frame(Write.ParentFrame);
  }
}

TEST(CompiledSEHStateWrites, IgnoresRebasingAndTemporaryCompilerSymbols) {
  for (bool EH4 : {false, true}) {
    auto Initial = makeReceipts(EH4);
    const auto Shape = collectX86SEHStateEmissionShape(Initial);
    ASSERT_TRUE(Shape);
    ASSERT_TRUE(validateCompiledX86SEHStateWrites(makeImage(Initial)));
    auto Relocated = Initial;
    relocateReceipts(Relocated, 0x100000);
    // Receipts need not arrive in physical-code order. Relaxation before a
    // state store may also change its offset without changing the store.
    std::reverse(Relocated.WinX86SEHStateWrites.begin(),
                 Relocated.WinX86SEHStateWrites.end());
    Relocated.WinX86SEHStateWrites[0].BeginVA += 5;
    Relocated.WinX86SEHStateWrites[0].EndVA += 5;
    EXPECT_EQ(collectX86SEHStateEmissionShape(Relocated), Shape);
  }
}

TEST(CompiledSEHStateWrites, PreservesWriteIdentityOrderTokenAndFrame) {
  const auto Original = makeReceipts();
  const auto Shape = collectX86SEHStateEmissionShape(Original);
  ASSERT_TRUE(Shape);
  auto ChangedShape = [&](auto Mutate) {
    auto Changed = Original;
    Mutate(Changed);
    auto Actual = collectX86SEHStateEmissionShape(Changed);
    ASSERT_TRUE(Actual);
    EXPECT_NE(Actual, Shape);
  };
  ChangedShape([](auto &R) { R.WinX86SEHStateWrites[0].WriteID += 1; });
  ChangedShape([](auto &R) { R.WinX86SEHStateWrites[0].AddressRegister += 1; });
  ChangedShape([](auto &R) {
    std::swap(R.WinX86SEHStateWrites[0].BeginVA,
              R.WinX86SEHStateWrites[1].BeginVA);
    std::swap(R.WinX86SEHStateWrites[0].EndVA, R.WinX86SEHStateWrites[1].EndVA);
  });
  ChangedShape([](auto &R) { R.WinX86SEHStateWrites[0].EndVA += 1; });
  ChangedShape([](auto &R) {
    R.WinX86SEHFrames[0].RegistrationOffset -= 4;
    for (auto &Write : R.WinX86SEHStateWrites)
      Write.ParentFrame = R.WinX86SEHFrames[0];
  });
  ChangedShape([](auto &R) {
    R.WinEHSemanticRecords[0].Token.Digest[3] += 1;
    R.WinX86SEHStateWrites[0].Semantic = R.WinEHSemanticRecords[0].Token;
  });
  ChangedShape([](auto &R) { R.SourceFunctionOwners[1].IsPrivate = false; });
  ChangedShape([](auto &R) { R.WinX86SEHStateWrites.pop_back(); });
}

TEST(CompiledSEHStateWrites, RefusesInvalidPortableReceiptsBeforeLayout) {
  auto Reject = [](auto Mutate) {
    auto Result = makeReceipts();
    Mutate(Result);
    EXPECT_FALSE(collectX86SEHStateEmissionShape(Result));
    EXPECT_FALSE(validateCompiledX86SEHStateWrites(makeImage(Result)));
  };
  Reject([](auto &R) { R.WinX86SEHStateWritesValid = false; });
  Reject([](auto &R) { R.FunctionRangesValid = false; });
  Reject([](auto &R) { R.WinEHSemanticsValid = false; });
  Reject([](auto &R) {
    R.SourceFunctionOwners.erase(R.SourceFunctionOwners.begin());
  });
  Reject([](auto &R) { R.FunctionOwnerAddrs["_cleanup"] += 1; });
  Reject([](auto &R) { R.WinX86SEHStateWrites[0].Semantic->Digest[2] += 1; });
  Reject([](auto &R) { R.WinX86SEHStateWrites[0].GeneratedState = 31; });
  Reject([](auto &R) { R.WinX86SEHStateWrites[0].WriteID = 0; });
  Reject([](auto &R) { R.WinX86SEHStateWrites[1].WriteID = 73; });
  Reject(
      [](auto &R) { R.WinX86SEHStateWrites[0].ParentFrame.StateOffset = 16; });
  Reject([](auto &R) { R.WinX86SEHStateWrites[0].EndVA = 0x4190; });
}

TEST(CompiledSEHStateWrites, RequiresFinalUniqueAllocatedCodeStorage) {
  auto Reject = [](auto Mutate) {
    auto Image = makeImage(makeReceipts());
    Mutate(Image);
    EXPECT_FALSE(validateCompiledX86SEHStateWrites(Image));
  };
  Reject([](auto &I) { I.TargetArch = Arch::X64; });
  Reject([](auto &I) { I.Format = BinaryFormat::ELF; });
  Reject([](auto &I) { I.PointerWidth = 8; });
  Reject([](auto &I) { I.ByteOrder = llvm::endianness::big; });
  Reject([](auto &I) { I.Sections[1].IsAllocated = false; });
  Reject(
      [](auto &I) { I.Sections[1].Kind = RewriteSectionKind::ReadOnlyData; });
  Reject([](auto &I) { I.Sections[1].Size = 0x16; });
  Reject([](auto &I) { I.Sections[1].Offset += 1; });
  Reject([](auto &I) { I.Bytes.resize(0x110); });
  Reject([](auto &I) { I.Sections.push_back(I.Sections[1]); });
  Reject([](auto &I) {
    I.Sections[2].VA = 0x4114;
    I.Sections[2].Size = 1;
  });
  Reject([](auto &I) {
    I.Sections[1].VA = std::numeric_limits<uint64_t>::max() - 4;
  });
  Reject([](auto &I) { I.Sections[1].IsInImage = false; });

  auto External = makeImage(makeReceipts());
  External.Sections[1].IsInImage = false;
  External.Sections[1].ExternalBytes.resize(0x80);
  EXPECT_TRUE(validateCompiledX86SEHStateWrites(External));
}

TEST(CompiledSEHStateWrites, RechecksGlobalFunctionOverlapAfterSizing) {
  auto Result = makeReceipts();
  auto &CallbackRange = Result.FunctionRanges[1];
  CallbackRange.OwnerVA -= 0xc0;
  CallbackRange.BeginVA -= 0xc0;
  CallbackRange.EndVA -= 0xc0;
  Result.FunctionOwnerAddrs["_cleanup"] -= 0xc0;
  Result.SourceFunctionOwners[1].OwnerVA -= 0xc0;
  for (auto &Write : Result.WinX86SEHStateWrites) {
    Write.OwnerVA -= 0xc0;
    Write.BeginVA -= 0xc0;
    Write.EndVA -= 0xc0;
  }
  ASSERT_TRUE(collectX86SEHStateEmissionShape(Result));
  auto Image = makeImage(Result);
  Image.Sections.erase(Image.Sections.begin() + 1);
  Image.Sections[0].Size = 0x100;
  EXPECT_FALSE(validateCompiledX86SEHStateWrites(Image));
}

} // namespace

#else

TEST(CompiledSEHStateWrites, RequiresCompilerReceiptCapability) {
  GTEST_SKIP() << "LLVM does not expose PE32 SEH state-write receipts";
}

#endif
