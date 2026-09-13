#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>

using namespace neverd;
using namespace neverd::sdk;
namespace {
struct Fixture {
  BinaryImage Image;
  HighFunc Function;
  Fixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Mapping;
    Mapping.VA = 0x1000;
    Mapping.Size = Mapping.FileSz = 0x100;
    Mapping.Flags = SegmentFlags::Readable;
    Mapping.Data.resize(0x100);
    Image.Segments.push_back(Mapping);
    Section Data;
    Data.VA = 0x1000;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Data);
    Image.ObjCSourceReferences.emplace(
        0x1010,
        ObjCSourceReference{
            ObjCSourceReference::Kind::Selector, 0x1010, 8, "step:", {}});
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal =
        HighExpr::makeLoad(HighExpr::makeConst(0x1010, 8), NdType::makeInt(8));
    Function.Body.push_back(Return);
  }
};
} // namespace

TEST(ObjCSourceBindings, ReplacesLoadedSelectorAndKeepsOriginalProjection) {
  Fixture F;
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  auto Call = Result.Function.Body[0].RetVal;
  ASSERT_EQ(Call->Kind, ExprKind::Call);
  ASSERT_TRUE(Call->SourceCallHint);
  EXPECT_EQ(Call->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSelector);
  EXPECT_EQ(Call->SourceCallHint->TargetName, "step:");
  EXPECT_TRUE(Call->Operands.empty());
  EXPECT_EQ(F.Function.Body[0].RetVal->Kind, ExprKind::Load);
  EXPECT_EQ(F.Function.Body[0].RetVal->Operands[0]->ConstVal, 0x1010u);
  EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
}

TEST(ObjCSourceBindings, SlotAddressIsNotTheRuntimeValue) {
  Fixture F;
  F.Function.Body[0].RetVal = HighExpr::makeConst(0x1010, 8);
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_EQ(Result.Function.Body[0].RetVal->Kind, ExprKind::Const);
  F.Function.Body[0].RetVal =
      HighExpr::makeLoad(HighExpr::makeConst(0x1014, 8), NdType::makeInt(4));
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, OrderedOrWrongWidthLoadsDoNotBecomeQueries) {
  Fixture F;
  F.Function.Body[0].RetVal->Type = NdType::makeInt(4);
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  F.Function.Body[0].RetVal->Type = NdType::makeInt(8);
  F.Function.Body[0].RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, IvarCarrierPreservesReadWidthAndDeclaringClass) {
  Fixture F;
  F.Image.ObjCSourceReferences[0x1010] = {ObjCSourceReference::Kind::IvarOffset,
                                          0x1010, 8, "_wide", "Base"};
  for (uint16_t Width : {4, 8}) {
    F.Function.Body[0].RetVal->Type = NdType::makeInt(Width);
    auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    ASSERT_TRUE(Result.Function.Body[0].RetVal->SourceCallHint);
    EXPECT_EQ(Result.Function.Body[0]
                  .RetVal->SourceCallHint->Signature.ReturnType->Size,
              Width);
    EXPECT_EQ(Result.InstanceLayoutClasses, std::set<std::string>{"Base"});
    EXPECT_TRUE(
        objcSourceCallBound(*Result.Function.Body[0].RetVal, F.Image, {}));
  }
  F.Image.ObjCSourceReferences[0x1010].Size = 4;
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, ForgedRuntimeIdentityCannotReuseAnotherSlot) {
  Fixture F;
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  auto Expression = Result.Function.Body[0].RetVal;
  auto Hint = std::make_shared<SourceCallTypeHint>(*Expression->SourceCallHint);
  Hint->TargetName = "different:";
  Expression->SourceCallHint = Hint;
  EXPECT_FALSE(objcSourceCallBound(*Expression, F.Image, {}));
}

TEST(ObjCSourceBindings, RuntimeCallsRevalidateImportedSignatureAndRegister) {
  Fixture F;
  F.Image.ImportPtrSlots[0x1020] = "_objc_retain_x19";
  const auto Original = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(Original);
  auto Call = HighExpr::makeCall("objc_retain", 0x1020,
                                 {HighExpr::makeConst(0x5678, 8)});
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Original);
  EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
  HighFunc Function;
  HighStmt Statement;
  Statement.Kind = StmtKind::Return;
  Statement.RetVal = Call;
  Function.Body.push_back(Statement);
  EXPECT_TRUE(bindObjCSourceReferences(Function, F.Image).Dependencies.empty());
  auto Changed = *Original;
  Changed.Signature.Parameters[0].Location.RegisterOffset = 0;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Changed);
  EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Original);
  F.Image.ImportPtrSlots[0x1020] = "_objc_release_x19";
  EXPECT_FALSE(objcSourceCallBound(*Call, F.Image, {}));
}

TEST(ObjCSourceBindings, StaticAssociationKeysKeepExactContextAndIdentity) {
  Fixture F;
  F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
  F.Image.ImportPtrSlots[0x1020] = "_objc_getAssociatedObject";
  const auto Hint = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(Hint);
  auto Key = HighExpr::makeConst(0x1031, 8);
  auto Call = HighExpr::makeCall("objc_getAssociatedObject", 0x1020,
                                 {HighExpr::makeConst(0, 8), Key});
  Call->Type = NdType::makePtr(NdType::makeVoid());
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  F.Function.Body[0].RetVal = Call;
  for (va_t Address : {0x1031, 0x1032, 0x1031}) {
    Key->ConstVal = Address;
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_EQ(Result.AssociationKeys, std::set<va_t>{Address});
    const auto Bound = Result.Function.Body[0].RetVal->Operands[1];
    ASSERT_TRUE(Bound->SourceCallHint);
    EXPECT_EQ(Bound->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeAssociationKey);
    EXPECT_EQ(Bound->SourceCallHint->TargetAddress, Address);
    EXPECT_TRUE(objcSourceCallBound(*Bound, F.Image, {}));
    EXPECT_EQ(Key->Kind, ExprKind::Const);
  }
  // Reusing the same expression as an ordinary address must not reuse the
  // contextual key substitution through the projection's clone cache.
  F.Function.Body[0].RetVal = HighExpr::makeBinop(NdOp::INT_ADD, Call, Key);
  const auto Mixed = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Mixed.Limitation.empty());
  EXPECT_EQ(Mixed.Function.Body[0].RetVal->Operands[1]->Kind, ExprKind::Const);

  F.Image.Arch = Arch::X64;
  const auto X64Hint = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(X64Hint);
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*X64Hint);
  F.Function.Body[0].RetVal = Call;
  const auto X64 = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(X64.Limitation.empty()) << X64.Limitation;
  const auto X64Key = X64.Function.Body[0].RetVal->Operands[1];
  ASSERT_TRUE(X64Key->SourceCallHint);
  EXPECT_EQ(X64Key->SourceCallHint->Signature.Architecture, Arch::X64);
  EXPECT_TRUE(objcSourceCallBound(*X64Key, F.Image, {}));
}

TEST(ObjCSourceBindings,
     StaticAssociationKeysRejectUnprovedConsumersAndStorage) {
  Fixture F;
  F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
  F.Image.ImportPtrSlots[0x1020] = "_objc_getAssociatedObject";
  const auto Hint = objcRuntimeSourceCallHint(F.Image, 0x1020);
  ASSERT_TRUE(Hint);
  auto Call = HighExpr::makeCall(
      "objc_getAssociatedObject", 0x1020,
      {HighExpr::makeConst(0, 8), HighExpr::makeConst(0x1031, 8)});
  Call->Type = NdType::makePtr(NdType::makeVoid());
  F.Function.Body[0].RetVal = Call;
  for (unsigned Case = 0; Case < 5; ++Case) {
    SCOPED_TRACE(Case);
    F.Image.Sections[0].Type = llvm::MachO::S_CSTRING_LITERALS;
    F.Image.Segments[0].Flags = SegmentFlags::Readable;
    auto Changed = *Hint;
    if (Case == 0)
      Changed.TargetName = "objc_setAssociatedObject";
    else if (Case == 1)
      Changed.Signature.Parameters[1].Location.RegisterOffset += 8;
    else if (Case == 2)
      F.Image.Sections[0].Type = 0;
    else if (Case == 3)
      F.Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    else
      Call->Operands[1] = HighExpr::makeConst(0x1031, 4);
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Changed);
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.AssociationKeys.empty());
  }
}

namespace {
struct ProfileFixture : Fixture {
  ProfileFixture() {
    Image.ObjCSourceReferences.clear();
    Image.Segments[0].Name = "__DATA";
    Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Sections[0].Name = "__llvm_prf_cnts";
    Image.Sections[0].SegmentName = "__DATA";
    Image.Sections[0].Flags = Image.Segments[0].Flags;
    for (size_t I = 0; I < 0x100; ++I)
      Image.Segments[0].Data[I] = uint8_t(I * 37 + 9);
  }
};
} // namespace

TEST(ObjCSourceBindings, ProfileCountersKeepOverlappingStorageAndAccessWidths) {
  ProfileFixture F;
  const ObjCProfileStorage Storage(F.Image);
  for (uint16_t Width : {1, 2, 4, 8, 16}) {
    auto Address = HighExpr::makeConst(0x1007, 8);
    F.Function.Body[0].RetVal =
        HighExpr::makeLoad(Address, NdType::makeInt(Width, false));
    const auto Result = bindObjCSourceReferences(F.Function, F.Image, &Storage);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    EXPECT_EQ(Result.ProfileCounterSections, std::set<va_t>{0x1000});
    const auto Load = Result.Function.Body[0].RetVal;
    EXPECT_EQ(Load->Kind, ExprKind::Load);
    EXPECT_EQ(Load->Type->Size, Width);
    const auto BoundAddress = Load->Operands[0];
    ASSERT_EQ(BoundAddress->Kind, ExprKind::BinOp);
    EXPECT_EQ(BoundAddress->Operands[1]->ConstVal, 7U);
    EXPECT_TRUE(
        objcSourceCallBound(*BoundAddress->Operands[0], F.Image, {}, &Storage));
    EXPECT_EQ(Address->ConstVal, 0x1007U);
    // A shared node used outside a memory access still has no source binding.
    F.Function.Body[0].RetVal =
        HighExpr::makeBinop(NdOp::INT_ADD, F.Function.Body[0].RetVal, Address);
    EXPECT_FALSE(bindObjCSourceReferences(F.Function, F.Image, &Storage)
                     .Limitation.empty());
  }
  EXPECT_EQ(Storage.sectionFor(0x10f0, 16), 0x1000U);
  EXPECT_FALSE(Storage.sectionFor(0x10f1, 16));
  EXPECT_FALSE(Storage.sectionFor(UINT64_MAX - 7, 16));
  EXPECT_FALSE(Storage.sectionFor(0x1000, 0));
  std::set<std::string> Names;
  const auto Source = Storage.render({0x1000}, Names);
  EXPECT_EQ(Names,
            std::set<std::string>{"neverd_profile_counters_1000_address"});
  EXPECT_NE(Source.find("[0] = 9"), std::string::npos);
  EXPECT_NE(Source.find("counters[256]"), std::string::npos);
}

TEST(ObjCSourceBindings, ProfileCountersRejectUnprovedStorageAndEffects) {
  for (unsigned Case = 0; Case < 18; ++Case) {
    SCOPED_TRACE(Case);
    ProfileFixture F;
    switch (Case) {
    case 0:
      F.Image.Sections[0].Name = "__llvm_prf_data";
      break;
    case 1:
      F.Image.Sections[0].FileSz--;
      break;
    case 2:
      F.Image.Segments[0].Data.resize(8);
      break;
    case 3:
      F.Image.Sections[0].Flags = SegmentFlags::Readable;
      break;
    case 4:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 5:
      F.Image.DataPtrRelocSlots.insert(0x1040);
      break;
    case 6:
      F.Image.MachOResolvedChainedPointerSlots.insert(0x1040);
      break;
    case 7:
      F.Image.DyldBindSlots[0x1040] = {};
      break;
    case 8:
      F.Image.BaseRelocations.push_back({0xff9, 0});
      break;
    case 9:
      F.Image.Sections.push_back(F.Image.Sections[0]);
      break;
    case 10:
      F.Image.Segments.push_back(F.Image.Segments[0]);
      break;
    case 11:
      F.Image.IsRelocatable = true;
      break;
    case 12:
      F.Image.Arch = Arch::ARM;
      break;
    case 13:
      F.Function.Body[0].RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 14:
      F.Function.Body[0].RetVal->Type = NdType::makePtr(NdType::makeVoid());
      break;
    case 15:
      F.Function.Body[0].RetVal->Operands[0] = HighExpr::makeConst(0x10f9, 8);
      break;
    case 16:
      F.Function.Body[0].RetVal->Type = NdType::makeFloat(16);
      break;
    case 17:
      F.Image.Segments[0].FileSz = 16;
      break;
    }
    const auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_FALSE(Result.Limitation.empty());
    EXPECT_TRUE(Result.ProfileCounterSections.empty());
  }
}

TEST(ObjCSourceBindings, ProfileCountersExecuteAcrossTranslationUnits) {
  ProfileFixture F;
  const ObjCProfileStorage Storage(F.Image);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-profile-storage",
                                                    Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove_all(Work, Error);
    }
  } Cleanup{Work};
  std::vector<std::string> Sources;
  std::set<va_t> Used;
  for (bool Store : {false, true}) {
    for (uint16_t Width : {1, 2, 4, 8, 16}) {
      const auto Type = NdType::makeInt(Width, false);
      HighFunc Function;
      Function.Name =
          std::string(Store ? "put" : "get") + std::to_string(Width);
      Function.Entry = 0x2000 + Sources.size() * 16;
      Function.ReturnType = Store ? NdType::makeVoid() : Type;
      const auto Address = HighExpr::makeConst(0x1007, 8);
      if (Store) {
        Function.Params.push_back({"arg0", Type});
        MedVar Value;
        Value.Kind = MedVar::Param;
        Value.Id = 0;
        Value.Size = Width;
        Value.TheArch = Arch::X64;
        HighStmt Statement;
        Statement.Kind = StmtKind::Store;
        Statement.StoreAddr = Address;
        Statement.StoreVal = HighExpr::makeVar(Value, Type);
        Function.Body.push_back(Statement);
      }
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      if (!Store)
        Return.RetVal = HighExpr::makeLoad(Address, Type);
      Function.Body.push_back(Return);
      auto Bound = bindObjCSourceReferences(Function, F.Image, &Storage);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      Used.insert(Bound.ProfileCounterSections.begin(),
                  Bound.ProfileCounterSections.end());
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
      const auto Path = (Work / (Function.Name + ".c")).string();
      std::ofstream(Path) << Source;
      Sources.push_back(Path);
    }
  }
  std::set<std::string> Helpers;
  std::string Harness = "#include <stdint.h>\n#include <string.h>\n" +
                        Storage.render(Used, Helpers);
  for (uint16_t Width : {1, 2, 4, 8, 16}) {
    const auto Type = Width == 16 ? "unsigned __int128"
                                  : "uint" + std::to_string(Width * 8) + "_t";
    Harness += "extern " + Type + " get" + std::to_string(Width) + "(void);\n";
    Harness += "extern void put" + std::to_string(Width) + "(" + Type + ");\n";
  }
  Harness += R"(
int main(void) {
  unsigned char expected[256];
  unsigned char *data = (unsigned char *)neverd_profile_counters_1000_address();
  for (unsigned i = 0; i != 256; ++i) expected[i] = (unsigned char)(i * 37 + 9);
  if (memcmp(data, expected, sizeof expected)) return 1;
  for (unsigned round = 0; round != 64; ++round) {
    unsigned __int128 value = ((unsigned __int128)(UINT64_MAX - round) << 64) | round;
    put16(value); memcpy(expected + 7, &value, 16);
    if (get16() != value || memcmp(data, expected, sizeof expected)) return 2;
    uint64_t a = UINT64_MAX - round; put8(a); memcpy(expected + 7, &a, 8);
    if (get8() != a || memcmp(data, expected, sizeof expected)) return 3;
    uint32_t b = UINT32_MAX - round; put4(b); memcpy(expected + 7, &b, 4);
    if (get4() != b || memcmp(data, expected, sizeof expected)) return 4;
    uint16_t c = UINT16_MAX - round; put2(c); memcpy(expected + 7, &c, 2);
    if (get2() != c || memcmp(data, expected, sizeof expected)) return 5;
    uint8_t d = (uint8_t)round; put1(d); memcpy(expected + 7, &d, 1);
    if (get1() != d || memcmp(data, expected, sizeof expected)) return 6;
    memcpy(&value, expected + 7, 16);
    if (get16() != value) return 7;
  }
  return 0;
}
)";
  const auto HarnessPath = (Work / "harness.c").string();
  std::ofstream(HarnessPath) << Harness;
  Sources.push_back(HarnessPath);
  const std::string Compiler = NEVERD_TEST_CLANG;
  const auto Executable = (Work / "test.exe").string();
  const auto ErrorPath = (Work / "stderr").string();
  std::vector<std::string> Arguments{
      Compiler,  "-std=c11", "-O3",     "-fstrict-aliasing",
      "-Werror", "-o",       Executable};
  Arguments.insert(Arguments.end(), Sources.begin(), Sources.end());
  std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, ErrorPath};
  std::string Error;
  const auto Status = llvm::sys::ExecuteAndWait(Compiler, Refs, std::nullopt,
                                                Redirects, 60, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Status, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "");
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                      Redirects, 30, 0, &Error),
            0)
      << Error;
}

TEST(ObjCSourceBindings, ExplicitABIPositionDriftIsDetected) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(8);
  Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                     {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
  std::string Reason;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Reason));
  auto Changed = Hint;
  Changed.Parameters[1].Location.RegisterOffset += 8;
  EXPECT_FALSE(objc_projection_detail::sameHint(Hint, Changed));
  Changed = Hint;
  Changed.ReturnLocation.RegisterOffset += 8;
  EXPECT_FALSE(objc_projection_detail::sameHint(Hint, Changed));
}

namespace {
struct ObjectFixture {
  BinaryImage Image;
  static constexpr va_t ClassAddress = 0x2020;
  static constexpr va_t MetaAddress = 0x2080;
  static constexpr va_t ClassSlot = 0x2010;
  ObjectFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Segment;
    Segment.VA = 0x2000;
    Segment.Size = Segment.FileSz = 0x1000;
    Segment.Flags = SegmentFlags::Readable;
    Segment.Data.resize(0x1000);
    Image.Segments.push_back(Segment);
    Section Section;
    Section.VA = 0x2000;
    Section.Size = Section.FileSz = 0x1000;
    Section.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Section);
    put(ClassAddress, MetaAddress);
    put(ClassAddress + 32, 0x2200);
    put(MetaAddress + 32, 0x2280);
    put(0x2280, 1); // RO_META
    put(0x2218, 0x2380);
    put(0x2298, 0x2380);
    const char Name[] = "Receiver";
    std::copy(std::begin(Name), std::end(Name),
              Image.Segments[0].Data.begin() + 0x380);
    ObjCClass Class;
    Class.Address = ClassAddress;
    Class.Name = "Receiver";
    Image.ObjCClasses.push_back(Class);
    Image.ObjCSourceReferences[ClassSlot] = {
        ObjCSourceReference::Kind::Class, ClassSlot, 8, "Receiver", {}};
  }
  void put(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(
        Image.Segments[0].Data.data() + Address - 0x2000, Value);
  }
  HighFunc message(ExprPtr Receiver) {
    auto Binding = std::make_shared<SourceCallTypeHint>();
    Binding->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Binding->TargetName = "objc_msgSend";
    Binding->Selector = "answer";
    Binding->Signature.ReturnType = NdType::makeInt(4);
    Binding->Signature.Parameters = {
        {"objc_self", NdType::makePtr(NdType::makeVoid())},
        {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
    std::string Error;
    EXPECT_TRUE(
        assignDarwinObjCSourceABI(Binding->Signature, Image.Arch, Error));
    auto Call = HighExpr::makeCall("objc_msgSend", 0,
                                   {Receiver, HighExpr::makeConst(0, 8)});
    Call->SourceCallHint = Binding;
    HighFunc Function;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body.push_back(Return);
    return Function;
  }
};
} // namespace

TEST(ObjCSourceBindings,
     DirectClassAndMetaclassReceiverUseVerifiedObjectIdentity) {
  ObjectFixture Fixture;
  for (va_t Address :
       {ObjectFixture::ClassAddress, ObjectFixture::MetaAddress}) {
    auto Original = Fixture.message(HighExpr::makeConst(Address, 8));
    auto Result = bindObjCSourceReferences(Original, Fixture.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    auto Receiver = Result.Function.Body[0].RetVal->Operands[0];
    ASSERT_TRUE(Receiver->SourceCallHint);
    EXPECT_EQ(Receiver->SourceCallHint->TargetName, "Receiver");
    EXPECT_EQ(Receiver->SourceCallHint->CallKind,
              Address == ObjectFixture::ClassAddress
                  ? SourceCallTypeHint::Kind::RuntimeClass
                  : SourceCallTypeHint::Kind::RuntimeMetaclass);
    EXPECT_TRUE(objcSourceCallBound(*Receiver, Fixture.Image, {}));
    EXPECT_EQ(Original.Body[0].RetVal->Operands[0]->Kind, ExprKind::Const);
  }
}

TEST(ObjCSourceBindings,
     ObjectReceiverRewriteDoesNotAffectSharedScalarConstant) {
  ObjectFixture Fixture;
  auto Shared = HighExpr::makeConst(ObjectFixture::ClassAddress, 8);
  auto Function = Fixture.message(Shared);
  HighStmt Scalar;
  Scalar.Kind = StmtKind::Return;
  Scalar.RetVal = Shared;
  Function.Body.push_back(Scalar);
  auto Result = bindObjCSourceReferences(Function, Fixture.Image);
  ASSERT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind, ExprKind::Call);
  EXPECT_EQ(Result.Function.Body[1].RetVal->Kind, ExprKind::Const);
  EXPECT_FALSE(Result.Limitation.empty());
}

TEST(ObjCSourceBindings, ClassrefSlotAddressCannotBecomeClassObjectReceiver) {
  ObjectFixture Fixture;
  auto Function =
      Fixture.message(HighExpr::makeConst(ObjectFixture::ClassSlot, 8));
  auto Result = bindObjCSourceReferences(Function, Fixture.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind, ExprKind::Const);
  Function = Fixture.message(HighExpr::makeLoad(
      HighExpr::makeConst(ObjectFixture::ClassSlot, 8), NdType::makeInt(8)));
  Result = bindObjCSourceReferences(Function, Fixture.Image);
  EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  auto Receiver = Result.Function.Body[0].RetVal->Operands[0];
  ASSERT_TRUE(Receiver->SourceCallHint);
  EXPECT_EQ(Receiver->SourceCallHint->TargetAddress, ObjectFixture::ClassSlot);
}

TEST(ObjCSourceBindings, MetaObjectNeedsResolvedIsaAndMatchingMetadata) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    ObjectFixture Fixture;
    if (Mutation == 0)
      Fixture.Image.MachOHasChainedFixups = true;
    if (Mutation == 1)
      Fixture.put(0x2280, 0); // A class is not a metaclass.
    if (Mutation == 2)
      Fixture.Image.ObjCClasses.push_back(Fixture.Image.ObjCClasses[0]);
    if (Mutation == 3)
      ++Fixture.Image.Sections[0].FileOff;
    auto Function =
        Fixture.message(HighExpr::makeConst(ObjectFixture::MetaAddress, 8));
    auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_FALSE(Result.Limitation.empty()) << Mutation;
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
  }
  ObjectFixture Fixture;
  Fixture.Image.MachOHasChainedFixups = true;
  Fixture.Image.MachOResolvedChainedPointerSlots = {
      ObjectFixture::ClassAddress, ObjectFixture::ClassAddress + 32,
      ObjectFixture::MetaAddress + 32, 0x2218, 0x2298};
  auto Function =
      Fixture.message(HighExpr::makeConst(ObjectFixture::MetaAddress, 8));
  EXPECT_TRUE(
      bindObjCSourceReferences(Function, Fixture.Image).Limitation.empty());
}

TEST(ObjCSourceBindings,
     NativeOrSuperStructurePointersAreNotMessageObjectReceivers) {
  ObjectFixture Fixture;
  for (auto Kind : {SourceCallTypeHint::Kind::Native,
                    SourceCallTypeHint::Kind::ObjCSuper2}) {
    auto Function =
        Fixture.message(HighExpr::makeConst(ObjectFixture::ClassAddress, 8));
    auto Binding = std::make_shared<SourceCallTypeHint>(
        *Function.Body[0].RetVal->SourceCallHint);
    Binding->CallKind = Kind;
    Function.Body[0].RetVal->SourceCallHint = Binding;
    auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
    EXPECT_FALSE(Result.Limitation.empty());
  }
}
