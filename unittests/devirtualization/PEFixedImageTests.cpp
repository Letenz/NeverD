//===- PEFixedImageTests.cpp - Complete fixed PE image evidence ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/analysis/InterpreterLLVMRefinement.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/COFF/PEFixedImage.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;
using namespace llvm::COFF;

namespace {

constexpr va_t Base = 0x140000000;
constexpr uint64_t Value = 0x123456789abcdef0;
constexpr size_t Optional = 0x98;
constexpr size_t Directories = Optional + 112;
constexpr size_t SectionTable = Optional + 240;
constexpr size_t Text = 0x400;
constexpr size_t RData = 0x600;
constexpr size_t IData = 0x800;
constexpr size_t Reloc = 0xa00;
constexpr size_t SectionBytes = 0x200;

void put16(std::vector<uint8_t> &Bytes, size_t At, uint16_t Value) {
  llvm::support::endian::write16le(Bytes.data() + At, Value);
}
void put32(std::vector<uint8_t> &Bytes, size_t At, uint32_t Value) {
  llvm::support::endian::write32le(Bytes.data() + At, Value);
}
void put64(std::vector<uint8_t> &Bytes, size_t At, uint64_t Value) {
  llvm::support::endian::write64le(Bytes.data() + At, Value);
}
void putName(std::vector<uint8_t> &Bytes, size_t At, llvm::StringRef Name) {
  std::copy(Name.begin(), Name.end(), Bytes.begin() + At);
}
void directory(std::vector<uint8_t> &Bytes, unsigned Index, uint32_t RVA,
               uint32_t Size) {
  put32(Bytes, Directories + Index * 8, RVA);
  put32(Bytes, Directories + Index * 8 + 4, Size);
}

std::vector<uint8_t> fixture() {
  std::vector<uint8_t> Bytes(0xc00, 0);
  putName(Bytes, 0, "MZ");
  put32(Bytes, 0x3c, 0x80);
  putName(Bytes, 0x80, llvm::StringRef("PE\0\0", 4));
  put16(Bytes, 0x84, IMAGE_FILE_MACHINE_AMD64);
  put16(Bytes, 0x86, 4);
  put16(Bytes, 0x94, 240);
  put16(Bytes, 0x96,
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE);
  put16(Bytes, Optional, 0x20b);
  put32(Bytes, Optional + 4, SectionBytes);
  put32(Bytes, Optional + 8, 3 * SectionBytes);
  put32(Bytes, Optional + 16, 0x1000);
  put32(Bytes, Optional + 20, 0x1000);
  put64(Bytes, Optional + 24, Base);
  put32(Bytes, Optional + 32, 0x1000);
  put32(Bytes, Optional + 36, SectionBytes);
  put16(Bytes, Optional + 40, 6);
  put16(Bytes, Optional + 48, 6);
  put32(Bytes, Optional + 56, 0x5000);
  put32(Bytes, Optional + 60, Text);
  put16(Bytes, Optional + 68, IMAGE_SUBSYSTEM_WINDOWS_CUI);
  put64(Bytes, Optional + 72, 0x100000);
  put64(Bytes, Optional + 80, 0x1000);
  put64(Bytes, Optional + 88, 0x100000);
  put64(Bytes, Optional + 96, 0x1000);
  put32(Bytes, Optional + 108, NUM_DATA_DIRECTORIES + 1);
  for (unsigned I = 0; I != 4; ++I) {
    const char *Names[] = {".text", ".rdata", ".idata", ".reloc"};
    const size_t At = SectionTable + I * 40;
    putName(Bytes, At, Names[I]);
    put32(Bytes, At + 8, SectionBytes);
    put32(Bytes, At + 12, 0x1000 * (I + 1));
    put32(Bytes, At + 16, SectionBytes);
    put32(Bytes, At + 20, Text + I * SectionBytes);
    put32(Bytes, At + 36,
          IMAGE_SCN_MEM_READ |
              (I == 0 ? IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE
                      : IMAGE_SCN_CNT_INITIALIZED_DATA) |
              (I == 2 ? IMAGE_SCN_MEM_WRITE : 0));
  }
  // Two unrelated scalar fields. Only the first is an instruction immediate.
  Bytes[Text] = 0x48;
  Bytes[Text + 1] = 0xb8; // movabs rax, Value; ret.
  put64(Bytes, Text + 2, Value);
  Bytes[Text + 10] = 0xc3;
  put64(Bytes, RData + 0x40, Value + 1);
  directory(Bytes, BASE_RELOCATION_TABLE, 0x4000, 24);
  for (unsigned I = 0; I != 2; ++I) {
    put32(Bytes, Reloc + I * 12, 0x1000 * (I + 1));
    put32(Bytes, Reloc + I * 12 + 4, 12);
    put16(Bytes, Reloc + I * 12 + 8,
          (IMAGE_REL_BASED_DIR64 << 12) | (I == 0 ? 2 : 0x40));
  }
  // One ordinal import, its independent lookup table and its writable IAT.
  directory(Bytes, IMPORT_TABLE, 0x3000, 40);
  directory(Bytes, IAT, 0x3080, 16);
  put32(Bytes, IData, 0x3060);
  put32(Bytes, IData + 12, 0x3030);
  put32(Bytes, IData + 16, 0x3080);
  putName(Bytes, IData + 0x30, "example.dll");
  put64(Bytes, IData + 0x60, (uint64_t{1} << 63) | 1);
  put64(Bytes, IData + 0x80, (uint64_t{1} << 63) | 1);
  return Bytes;
}

llvm::Expected<BinaryImage> load(llvm::ArrayRef<uint8_t> Bytes) {
  int FD = -1;
  llvm::SmallString<128> Path;
  if (const auto Error = llvm::sys::fs::createTemporaryFile(
          "neverd-fixed-image", "exe", FD, Path))
    return llvm::errorCodeToError(Error);
  const auto Remove = llvm::scope_exit([&] { llvm::sys::fs::remove(Path); });
  llvm::raw_fd_ostream File(FD, true);
  File.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  File.close();
  COFFLoader Loader;
  return Loader.load(std::string(Path));
}

class PEFixedImageTest : public testing::Test {
protected:
  BinaryImage Image;
  SpecializationOptions Options;
  LowIRIndependenceContract Contract;

  void SetUp() override {
    auto Loaded = load(fixture());
    ASSERT_TRUE(static_cast<bool>(Loaded))
        << llvm::toString(Loaded.takeError());
    Image = std::move(*Loaded);
    Options.ExplicitMachineState = true;
    Options.NormalNonfaultingExecution = true;
    Options.X64CetDisabled = true;
    Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
    Options.X64FlagsProfile = Contract.X64FlagsProfile =
        InterpreterMachineStateProfile::UserX64NoFaultV1;
    Contract.Frame = LowIRIndependenceFrame{{x86reg::RSP, 8}, -32, 8};
    Contract.ReturnRegisters = {{x86reg::RAX, 8}};
  }

  // Mutated file fixtures get matching mapped bytes, so metadata negatives
  // exercise the parser rather than only the raw/mapped equality guard.
  void sync() {
    const auto Sync = [&](auto &Mapping) {
      Mapping.Data.assign(Image.Raw.begin() + Mapping.FileOff,
                          Image.Raw.begin() + Mapping.FileOff + Mapping.FileSz);
    };
    for (auto &M : Image.Segments)
      Sync(M);
    for (auto &S : Image.Sections)
      Sync(S);
  }

  std::string refusal() const {
    auto View = PEFixedImageView::create(Image);
    EXPECT_FALSE(static_cast<bool>(View));
    return View ? "" : llvm::toString(View.takeError());
  }

  BinaryUndefinedIndependenceResult proof() const {
    return checkBinaryUndefinedIndependence(Image, Base + 0x1000, Options,
                                            Contract);
  }
};

TEST_F(PEFixedImageTest, CompleteFieldsAdmitCodeAndDataButExcludeLoaderWrites) {
  auto View = PEFixedImageView::create(Image);
  ASSERT_TRUE(static_cast<bool>(View)) << llvm::toString(View.takeError());
  ASSERT_TRUE(View->read(Base + 0x1000, 11, true));
  const auto Field = View->read(Base + 0x1002, 8, true);
  ASSERT_TRUE(Field);
  EXPECT_EQ(llvm::support::endian::read64le(Field->data()), Value);
  EXPECT_TRUE(View->read(Base + 0x2040, 8, false));
  EXPECT_FALSE(View->read(Base + 0x2040, 8, true));
  EXPECT_FALSE(View->read(Base + 0x3080, 8, false));
  EXPECT_FALSE(View->read(Base + 0x3050, 1, false));
  EXPECT_TRUE(View->read(Base + 0x21fd, 3, false));
  EXPECT_FALSE(View->read(Base + 0x21fd, 4, false));
  EXPECT_FALSE(View->read(InvalidVA, 2, false));
  EXPECT_FALSE(View->read(Base, 0, false));
}

TEST_F(PEFixedImageTest, RecoveryAndNativeProofUseTheSameInstructionEvidence) {
  const auto Recovery =
      specializeBinaryInterpreter(Image, Base + 0x1000, Options);
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Proof = proof();
  ASSERT_TRUE(Proof.proved()) << Proof.Proof.Diagnostic;
  ASSERT_FALSE(Proof.Certificate->Instructions.empty());
  EXPECT_EQ(Proof.Certificate->Instructions.front().NativeBytes.size(), 10u);
}

TEST_F(PEFixedImageTest, RelocatedImmutableReadsHaveCompleteEvidence) {
  // mov rax, qword ptr [rip+disp32]; ret. The literal has a DIR64 field.
  const uint8_t Code[] = {0x48, 0x8b, 0x05, 0x39, 0x10, 0, 0, 0xc3};
  std::copy(std::begin(Code), std::end(Code), Image.Raw.begin() + Text);
  std::copy_n(Image.Raw.begin() + Reloc + 12, 12, Image.Raw.begin() + Reloc);
  directory(Image.Raw, BASE_RELOCATION_TABLE, 0x4000, 12);
  Image.BaseRelocations.erase(Image.BaseRelocations.begin());
  sync();
  const auto Recovery =
      specializeBinaryInterpreter(Image, Base + 0x1000, Options);
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Proof = proof();
  ASSERT_TRUE(Proof.proved()) << Proof.Proof.Diagnostic;
  ASSERT_EQ(Proof.Certificate->Reads.size(), 1u);
  EXPECT_EQ(llvm::support::endian::read64le(
                Proof.Certificate->Reads.front().Bytes.data()),
            Value + 1);
}

TEST_F(PEFixedImageTest, FreshCertificatesBindTheCompleteSnapshot) {
  const auto Before = proof();
  ASSERT_TRUE(Before.proved()) << Before.Proof.Diagnostic;
  Image.Raw[RData + 0x70] ^= 1; // Unread bytes still identify the snapshot.
  sync();
  const auto After = proof();
  ASSERT_TRUE(After.proved()) << After.Proof.Diagnostic;
  EXPECT_NE(Before.Certificate->InputDigest, After.Certificate->InputDigest);
}

TEST_F(PEFixedImageTest, PartialReadsCannotHideChangesElsewhereInTheField) {
  Image.Segments[0].Data[9] ^= 1;
  EXPECT_NE(refusal().find("mapped bytes"), std::string::npos);
  EXPECT_FALSE(proof().proved());
}

TEST_F(PEFixedImageTest, ChangedBaseInventoryPermissionsAndImportsRefuse) {
  const BinaryImage Original = Image;
  for (unsigned Case = 0; Case != 7; ++Case) {
    SCOPED_TRACE(Case);
    Image = Original;
    switch (Case) {
    case 0:
      Image.Base += 0x10000;
      break;
    case 1:
      Image.BaseRelocations.clear();
      break;
    case 2:
      Image.BaseRelocations[0].Type = IMAGE_REL_BASED_HIGHLOW;
      break;
    case 3:
      Image.Segments[0].Flags = SegmentFlags::Readable;
      break;
    case 4:
      Image.Segments[0].FileSz--;
      break;
    case 5:
      Image.Sections[0].Data[2] ^= 1;
      break;
    case 6:
      Image.Imports.push_back({.IATAddr = Base + 0x1002});
      break;
    }
    EXPECT_FALSE(refusal().empty());
    EXPECT_FALSE(proof().proved());
  }
}

TEST_F(PEFixedImageTest, AdditionalWriterDirectoriesAndBudgetsRefuse) {
  const BinaryImage Original = Image;
  for (unsigned Index : {TLS_TABLE, LOAD_CONFIG_TABLE, DELAY_IMPORT_DESCRIPTOR,
                         CLR_RUNTIME_HEADER, BOUND_IMPORT}) {
    SCOPED_TRACE(Index);
    Image = Original;
    directory(Image.Raw, Index, 0x3100, 16);
    EXPECT_FALSE(refusal().empty());
  }
  Image = Original;
  for (PEFixedImageLimits Limits : {PEFixedImageLimits{.MaxBytes = 1},
                                    PEFixedImageLimits{.MaxRecords = 1}}) {
    auto View = PEFixedImageView::create(Image, Limits);
    ASSERT_FALSE(static_cast<bool>(View));
    EXPECT_NE(llvm::toString(View.takeError()).find("budget"),
              std::string::npos);
  }
}

TEST_F(PEFixedImageTest,
       ExactInPlaceLookupIsAllowedButItsTerminatorIsNotWritten) {
  put32(Image.Raw, IData, 0); // OriginalFirstThunk absent.
  sync();
  auto View = PEFixedImageView::create(Image);
  ASSERT_TRUE(static_cast<bool>(View)) << llvm::toString(View.takeError());
  EXPECT_TRUE(View->read(Base + 0x1000, 11, true));
  EXPECT_FALSE(View->read(Base + 0x3080, 16, false));
}

TEST_F(PEFixedImageTest, ImportTerminationDestinationsAndEncodingAreValidated) {
  const BinaryImage Original = Image;
  for (unsigned Case = 0; Case != 6; ++Case) {
    SCOPED_TRACE(Case);
    Image = Original;
    switch (Case) {
    case 0:
      directory(Image.Raw, IMPORT_TABLE, 0x3000, 20);
      break;
    case 1:
      put32(Image.Raw, IData + 16, 0);
      break;
    case 2:
      put32(Image.Raw, IData + 16, 0x3200);
      break;
    case 3:
      put32(Image.Raw, IData, 0x31f8);
      put64(Image.Raw, IData + 0x1f8, (uint64_t{1} << 63) | 1);
      break;
    case 4:
      put64(Image.Raw, IData + 0x60, (uint64_t{1} << 63) | 0x10001);
      break;
    case 5:
      directory(Image.Raw, IAT, 0x3088, 8);
      break;
    }
    sync();
    EXPECT_FALSE(refusal().empty());
  }
}

TEST_F(PEFixedImageTest, ImportWritesCannotChangeLaterLoaderMetadata) {
  const BinaryImage Original = Image;
  for (unsigned Case = 0; Case != 2; ++Case) {
    Image = Original;
    directory(Image.Raw, IMPORT_TABLE, 0x3000, 60);
    directory(Image.Raw, IAT, 0, 0); // Footprint comes from descriptors.
    std::copy_n(Image.Raw.begin() + IData, 20, Image.Raw.begin() + IData + 20);
    putName(Image.Raw, IData + 0xb0, "example.dll");
    std::fill_n(Image.Raw.begin() + IData + 0x30, 12, 0);
    put32(Image.Raw, IData + 12, 0x30b0);
    put32(Image.Raw, IData + 20 + 12, 0x30b0);
    put32(Image.Raw, IData + 20 + 16, 0x3090);
    put64(Image.Raw, IData + 0x90, (uint64_t{1} << 63) | 1);
    // First IAT overwrites the next descriptor's FirstThunk or a later
    // lookup terminator; a static inventory cannot authorize either write.
    put32(Image.Raw, IData + 16, Case == 0 ? 0x3024 : 0x3068);
    sync();
    EXPECT_NE(refusal().find("loader metadata"), std::string::npos);
  }
}

TEST_F(PEFixedImageTest, RawAndVirtualAliasesCannotAuthenticateBytes) {
  const BinaryImage Original = Image;
  put32(Image.Raw, SectionTable + 40 + 20, Text);
  EXPECT_NE(refusal().find("overlapping"), std::string::npos);
  Image = Original;
  put32(Image.Raw, SectionTable + 40 + 12, 0x1000);
  EXPECT_NE(refusal().find("overlapping"), std::string::npos);
}

TEST_F(PEFixedImageTest, MalformedRelocationTailNeverPublishesAPrefix) {
  put32(Image.Raw, Reloc + 12 + 4, 7);
  auto Object = llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(Image.Raw.data()),
                      Image.Raw.size()),
      {}));
  ASSERT_TRUE(static_cast<bool>(Object)) << llvm::toString(Object.takeError());
  Image.BaseRelocations.clear();
  auto Error = coff_loader::parseBaseRelocations(**Object, Image, Base);
  ASSERT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
  EXPECT_TRUE(Image.BaseRelocations.empty());
  EXPECT_TRUE(Image.CodePtrRelocSlots.empty());
  EXPECT_TRUE(Image.DataPtrRelocSlots.empty());
}

TEST_F(PEFixedImageTest, HighAdjPayloadIsNeverMisreadAsAnotherRelocation) {
  auto Bytes = fixture();
  directory(Bytes, BASE_RELOCATION_TABLE, 0x4000, 12);
  put16(Bytes, Reloc + 8, (IMAGE_REL_BASED_HIGHADJ << 12) | 2);
  put16(Bytes, Reloc + 10, 0xa070);
  auto Loaded = load(Bytes);
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  ASSERT_EQ(Loaded->BaseRelocations.size(), 1u);
  EXPECT_EQ(Loaded->BaseRelocations[0].Type, IMAGE_REL_BASED_HIGHADJ);
  auto View = PEFixedImageView::create(*Loaded);
  ASSERT_FALSE(static_cast<bool>(View));
  EXPECT_NE(llvm::toString(View.takeError()).find("only DIR64"),
            std::string::npos);
}

TEST_F(PEFixedImageTest, MissingRawEvidenceCannotAuthorizeRelocatedBytes) {
  Image.Raw.clear();
  EXPECT_FALSE(refusal().empty());
  EXPECT_FALSE(proof().proved());
}

TEST_F(PEFixedImageTest, MutableRawHeadersAreCheckedBeforeObjectConstruction) {
  const BinaryImage Original = Image;
  for (size_t Size : {size_t{0}, size_t{63}, size_t{64}, size_t{0x84},
                      Optional + 111, SectionTable - 1, SectionTable + 159}) {
    SCOPED_TRACE(Size);
    Image = Original;
    Image.Raw.resize(Size);
    EXPECT_FALSE(refusal().empty());
  }
  for (unsigned Case = 0; Case != 7; ++Case) {
    SCOPED_TRACE(Case);
    Image = Original;
    switch (Case) {
    case 0:
      put32(Image.Raw, 0x3c, UINT32_MAX);
      break;
    case 1:
      put32(Image.Raw, 0x3c, Image.Raw.size() - 3);
      break;
    case 2:
      put16(Image.Raw, 0x94, 111);
      break;
    case 3:
      put16(Image.Raw, 0x94, 120);
      break;
    case 4:
      put32(Image.Raw, Optional + 108, 17);
      break;
    case 5:
      put16(Image.Raw, 0x86, UINT16_MAX);
      break;
    case 6:
      directory(Image.Raw, LOAD_CONFIG_TABLE, 0x31ff, 1);
      break;
    }
    EXPECT_FALSE(refusal().empty());
    EXPECT_FALSE(proof().proved());
  }
}

TEST_F(PEFixedImageTest,
       NaturalPointerProvenanceAndEveryOperandMemberAreBound) {
  for (bool CodeTarget : {false, true}) {
    SCOPED_TRACE(CodeTarget);
    auto Bytes = fixture();
    const va_t Target = Base + (CodeTarget ? 0x1000 : 0x2080);
    put64(Bytes, Text + 2, Target);
    put64(Bytes, RData + 0x40, Target);
    auto Loaded = load(Bytes);
    ASSERT_TRUE(static_cast<bool>(Loaded))
        << llvm::toString(Loaded.takeError());
    Image = std::move(*Loaded);
    auto &Fields = CodeTarget ? Image.CodeAddressRelocOperands
                              : Image.DataAddressRelocOperands;
    ASSERT_EQ(Fields.size(), 1u);
    EXPECT_EQ(CodeTarget ? Image.CodePtrRelocSlots.size()
                         : Image.DataPtrRelocSlots.size(),
              1u);
    auto View = PEFixedImageView::create(Image);
    ASSERT_TRUE(static_cast<bool>(View)) << llvm::toString(View.takeError());
    ASSERT_TRUE(proof().proved());
    const auto Original = Fields.begin()->second;
    for (unsigned Case = 0; Case != 6; ++Case) {
      SCOPED_TRACE(Case);
      auto &Field = Fields.begin()->second;
      Field = Original;
      switch (Case) {
      case 0:
        ++Field.EncodedValue;
        break;
      case 1:
        ++Field.TargetVA;
        break;
      case 2:
        --Field.Width;
        break;
      case 3:
        ++Field.TargetOwnerVA;
        break;
      case 4:
        Field.PCRelativeFromInstructionEnd = true;
        break;
      case 5:
        Field.Kind = RelocatedAddressFieldKind::I386ELFGOTOFF;
        break;
      }
      EXPECT_FALSE(refusal().empty());
      EXPECT_FALSE(proof().proved());
    }
  }
}

TEST_F(PEFixedImageTest, ExtraProvenanceAndChangedMappingNamesRefuse) {
  const BinaryImage Original = Image;
  for (unsigned Case = 0; Case != 9; ++Case) {
    SCOPED_TRACE(Case);
    Image = Original;
    const va_t Slot = Base + 0x2040;
    switch (Case) {
    case 0:
      Image.CodePtrRelocSlots.insert(Slot);
      break;
    case 1:
      Image.DataPtrRelocSlots.insert(Slot);
      break;
    case 2:
      Image.DataPtrRelocTargetOwners[Slot] = Base + 0x2000;
      break;
    case 3:
      Image.RelCodeRelocSlots.insert(Slot);
      break;
    case 4:
      Image.RelDataPtrRelocSlots.insert(Slot);
      break;
    case 5:
      Image.CodeAddressRelocOperands[Slot] = {};
      break;
    case 6:
      Image.DataAddressRelocOperands[Slot] = {};
      break;
    case 7:
      Image.Sections[1].Name = ".changed";
      break;
    case 8:
      Image.Segments[1].Name = ".changed";
      break;
    }
    EXPECT_FALSE(refusal().empty());
    EXPECT_FALSE(proof().proved());
  }
}

TEST_F(PEFixedImageTest, RelocationBlockAddressesMustBeAligned) {
  std::copy_backward(Image.Raw.begin() + Reloc, Image.Raw.begin() + Reloc + 24,
                     Image.Raw.begin() + Reloc + 25);
  directory(Image.Raw, BASE_RELOCATION_TABLE, 0x4001, 24);
  sync();
  EXPECT_NE(refusal().find("unaligned"), std::string::npos);
  EXPECT_FALSE(proof().proved());
}

TEST_F(PEFixedImageTest, SnapshotBytesReachBothRelationProofStages) {
  const auto Recovery =
      specializeBinaryInterpreter(Image, Base + 0x1000, Options);
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto CheckNative = [&](const LowFunc &Candidate) {
    return checkBinaryLowIRRefinement(Image, Base + 0x1000, Options, Candidate,
                                      Contract);
  };
  const auto Native = CheckNative(Recovery.Residual);
  ASSERT_TRUE(Native.proved()) << Native.Proof.Diagnostic;
  const auto IR = [&](uint64_t Result, unsigned Status) {
    return "target datalayout = \"e-p:64:64-i64:64-n8:16:32:64\"\n"
           "target triple = \"x86_64-unknown-linux-gnu\"\n"
           "define i64 @model(ptr %state) {\nstore i64 " +
           std::to_string(Result) + ", ptr %state, align 8\nret i64 " +
           std::to_string(Status) + "\n}\n";
  };
  const auto CheckLLVM = [&](llvm::StringRef Text) {
    return checkBinaryLLVMRefinement(Image, Base + 0x1000, Options,
                                     Recovery.Residual, Text, "model",
                                     *Contract.Frame);
  };
  const auto Composed = CheckLLVM(IR(Value, 0));
  ASSERT_TRUE(Composed.proved()) << Composed.Diagnostic;
  for (const auto &Bad : {IR(Value + 1, 0), IR(Value, 1)}) {
    const auto Rejected = CheckLLVM(Bad);
    EXPECT_EQ(Rejected.Stage, InterpreterLLVMRefinementStage::LLVM);
    EXPECT_EQ(Rejected.LLVM.Status, LowIRRefinementStatus::Different);
    EXPECT_FALSE(Rejected.Certificate);
  }
  // A fresh, valid snapshot with changed instruction semantics must reject
  // the previous residual at the original/native boundary.
  put64(Image.Raw, Text + 2, Value + 2);
  sync();
  const auto WrongNative = CheckNative(Recovery.Residual);
  EXPECT_EQ(WrongNative.Proof.Status, LowIRRefinementStatus::Different);
  EXPECT_FALSE(WrongNative.Certificate);
  const auto WrongComposition = CheckLLVM(IR(Value, 0));
  EXPECT_EQ(WrongComposition.Stage, InterpreterLLVMRefinementStage::Native);
  EXPECT_FALSE(WrongComposition.Certificate);
}

TEST_F(PEFixedImageTest, PreparationExhaustionIsExplicitAndCanBeRetried) {
  const auto Recovery =
      specializeBinaryInterpreter(Image, Base + 0x1000, Options);
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Original = Options;
  for (bool Bytes : {false, true}) {
    Options = Original;
    if (Bytes)
      Options.MaxImagePreparationBytes = 1;
    else
      Options.MaxImagePreparationRecords = 1;
    const auto Refused =
        specializeBinaryInterpreter(Image, Base + 0x1000, Options);
    EXPECT_EQ(Refused.Status, SpecializationStatus::BudgetExceeded);
    const auto Independent = proof();
    EXPECT_EQ(Independent.Proof.Status,
              LowIRIndependenceStatus::BudgetExceeded);
    EXPECT_FALSE(Independent.Certificate);
    const auto Relation = checkBinaryLowIRRefinement(
        Image, Base + 0x1000, Options, Recovery.Residual, Contract);
    EXPECT_EQ(Relation.Proof.Status, LowIRRefinementStatus::BudgetExceeded);
    EXPECT_FALSE(Relation.Certificate);
  }
  Options = Original;
  EXPECT_TRUE(
      specializeBinaryInterpreter(Image, Base + 0x1000, Options).complete());
  EXPECT_TRUE(proof().proved());
}

TEST_F(PEFixedImageTest, OrdinaryLoadingDoesNotInheritTheAnalysisRecordBudget) {
  constexpr unsigned Count = 40000;
  constexpr unsigned PerPage = 4096 / sizeof(uint64_t);
  constexpr unsigned Pages = (Count + PerPage - 1) / PerPage;
  constexpr unsigned FieldsSize = Pages * 4096;
  std::vector<uint8_t> Table;
  for (unsigned Page = 0; Page != Pages; ++Page) {
    const unsigned Entries = std::min(PerPage, Count - Page * PerPage);
    const unsigned Size = 8 + ((Entries + 1) & ~1u) * 2;
    const size_t At = Table.size();
    Table.resize(At + Size, 0);
    put32(Table, At, 0x2000 + Page * 4096);
    put32(Table, At + 4, Size);
    for (unsigned I = 0; I != Entries; ++I)
      put16(Table, At + 8 + I * 2, (IMAGE_REL_BASED_DIR64 << 12) | (I * 8));
  }
  auto Bytes = fixture();
  const size_t DataSize = (FieldsSize + Table.size() + 511) & ~size_t{511};
  Bytes.resize(RData + DataSize, 0);
  std::fill(Bytes.begin() + RData, Bytes.end(), 0);
  put16(Bytes, 0x86, 2);
  put32(Bytes, SectionTable + 40 + 8, DataSize);
  put32(Bytes, SectionTable + 40 + 16, DataSize);
  put32(Bytes, Optional + 56, (0x2000 + DataSize + 4095) & ~size_t{4095});
  directory(Bytes, IMPORT_TABLE, 0, 0);
  directory(Bytes, IAT, 0, 0);
  directory(Bytes, BASE_RELOCATION_TABLE, 0x2000 + FieldsSize, Table.size());
  std::copy(Table.begin(), Table.end(), Bytes.begin() + RData + FieldsSize);
  auto Loaded = load(Bytes);
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  ASSERT_EQ(Loaded->BaseRelocations.size(), Count);
  auto Limited = PEFixedImageView::create(*Loaded);
  ASSERT_FALSE(static_cast<bool>(Limited));
  EXPECT_NE(llvm::toString(Limited.takeError()).find("budget"),
            std::string::npos);
  auto Expanded = PEFixedImageView::create(*Loaded, {.MaxRecords = 200000});
  ASSERT_TRUE(static_cast<bool>(Expanded))
      << llvm::toString(Expanded.takeError());
}

TEST_F(PEFixedImageTest, RepeatedLongSectionNamesConsumeThePreparationBudget) {
  auto Bytes = fixture();
  const size_t Strings = Bytes.size();
  constexpr size_t NameBytes = 4096;
  Bytes.resize(Strings + sizeof(uint32_t) + NameBytes + 1, 'n');
  Bytes.back() = 0;
  put32(Bytes, 0x8c, Strings); // COFF symbol table with zero symbols.
  put32(Bytes, Strings, sizeof(uint32_t) + NameBytes + 1);
  for (unsigned I = 0; I != 4; ++I) {
    std::fill_n(Bytes.begin() + SectionTable + I * 40, 8, 0);
    putName(Bytes, SectionTable + I * 40, "/4");
  }
  auto Loaded = load(Bytes);
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  auto Limited =
      PEFixedImageView::create(*Loaded, {.MaxBytes = 2 * Bytes.size()});
  ASSERT_FALSE(static_cast<bool>(Limited));
  EXPECT_NE(llvm::toString(Limited.takeError()).find("budget"),
            std::string::npos);
  auto Expanded =
      PEFixedImageView::create(*Loaded, {.MaxBytes = 8 * Bytes.size()});
  ASSERT_TRUE(static_cast<bool>(Expanded))
      << llvm::toString(Expanded.takeError());
}

} // namespace
