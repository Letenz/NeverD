#include "../../../lib/loader/MachO/ImmutableNativeFrame.h"
#include "../../../lib/loader/Swift/SwiftMangledClassMethodABI.h"
#include "../../../lib/loader/Swift/SwiftMangledValueConstructorABI.h"
#include "../../../lib/sdk/capi/ObjCNativeSwiftReceiverSources.h"
#include "../../../lib/sdk/capi/SourceSwiftValueConstructorProjection.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/Swift/SwiftMetadata.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Support/Endian.h"

using namespace neverd;
namespace {
struct FieldFixture {
  static constexpr va_t Entry = 0x1100, Call = Entry + 44, Slot = 0x5200;
  BinaryImage Image;
  llvm::LLVMContext Context;
  PipelineResult Result;
  FieldFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.MachOTwoLevelNamespace = true;
    addSegment(0x1000, SegmentFlags::Readable | SegmentFlags::Executable);
    addSegment(0x3000, SegmentFlags::Readable);
    addSegment(0x5000, SegmentFlags::Readable | SegmentFlags::Writable);
    Image.Sections[0].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    const uint32_t Words[] = {0xd10283ff, 0xa9087bf4, 0x90000028, 0xf9410108,
                              0xf8686a80, 0x6f00e400, 0xad0003e0, 0xad0103e0,
                              0xad0203e0, 0xad0303e0, 0x910003e2, 0x94000075,
                              0xa9487bf4, 0x910283ff, 0xd65f03c0};
    for (unsigned I = 0; I < std::size(Words); ++I)
      u32(Entry + 4 * I, Words[I]);
    const uint32_t Stub[] = {0xd0000001, 0xf9430021, 0xd0000010, 0xf9431210,
                             0xd61f0200};
    for (unsigned I = 0; I < std::size(Stub); ++I)
      u32(0x1300 + I * 4, Stub[I]);
    Image.Symbols.push_back(
        {"_$s4Demo7DerivedC6layoutyyF", Entry, sizeof(Words), true});
    Image.Symbols.push_back(
        {"_$s4Demo7DerivedC5layerSo7CALayerCvpWvd", Slot, 8, false});
    Image.RuntimeFunctionAddrs = {Entry, 0x1300};
    Image.DynInfo.NeededLibs = {
        "/usr/lib/libobjc.A.dylib",
        "/System/Library/Frameworks/Foundation.framework/Foundation",
        "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
        "/System/Library/Frameworks/UIKit.framework/UIKit"};
    Image.ImportPtrSlots[0x3620] = "_objc_msgSend";
    Image.DyldBindSlots[0x3620] = {"_objc_msgSend", 0,
                                   "/usr/lib/libobjc.A.dylib", false};
    text(0x3660, "removeAllAnimations");
    pointer(0x3600, 0x3660);
    Image.ObjCSourceReferences[0x3600] = {ObjCSourceReference::Kind::Selector,
                                          0x3600, 8, "removeAllAnimations"};
    Image.ObjCSourceReferences[Slot] = {ObjCSourceReference::Kind::IvarOffset,
                                        Slot, 8, "layer", "_TtC4Demo7Derived"};
    u32(0x3000, 0x50);
    relative(0x3004, 0x3080);
    relative(0x3008, 0x30c0);
    relative(0x3010, 0x3100);
    relative(0x3014, 0x3188);
    u32(0x3018, 3);
    u32(0x301c, 13);
    u32(0x3020, 1);
    u32(0x3024, 1);
    u32(0x3028, 12);
    relative(0x3088, 0x30b0);
    text(0x30b0, "Demo");
    text(0x30c0, "Derived");
    relative(0x3100, 0x3180);
    relative(0x3104, 0x3188);
    u32(0x3108, (12u << 16) | 7);
    u32(0x310c, 1);
    relative(0x3114, 0x3190);
    relative(0x3118, 0x31a0);
    *bytes(0x3180) = 1;
    relative(0x3181, 0x3000);
    *bytes(0x3188) = 1;
    relative(0x3189, 0x3800);
    text(0x3190, "So7CALayerC");
    text(0x31a0, "layer");
    pointer(0x5008, 0x5100);
    pointer(0x5020, 0x3302);
    u32(0x5038, 128);
    u32(0x503c, 24);
    pointer(0x5040, 0x3000);
    u64(0x5060, 8);
    pointer(0x5120, 0x3382);
    pointer(0x5140, 0x3800);
    pointer(0x3318, 0x3500);
    pointer(0x3330, 0x3400);
    pointer(0x3398, 0x3540);
    u32(0x3304, 8);
    u32(0x3308, 16);
    text(0x3500, "_TtC4Demo7Derived");
    text(0x3540, "_TtC4Demo4Base");
    u32(0x3400, 32);
    u32(0x3404, 1);
    pointer(0x3408, Slot);
    pointer(0x3410, 0x31a0);
    pointer(0x3418, 0x31f0);
    u32(0x3420, 3);
    u32(0x3424, 8);
    u64(Slot, 8);
    ObjCClass C;
    C.Name = "_TtC4Demo7Derived";
    C.Address = 0x5000;
    C.SuperclassAddress = 0x5100;
    C.SuperclassName = "_TtC4Demo4Base";
    C.InheritanceStatus = "resolved";
    C.IvarStatus = "recovered";
    C.InstanceStart = 8;
    C.InstanceSize = 16;
    C.Ivars.push_back({"layer", "", 0x3408, Slot, 8, 8, 8});
    Image.ObjCClasses.push_back(C);
    ObjCClass Base;
    Base.Name = C.SuperclassName;
    Base.Address = 0x5100;
    Base.SuperclassName = "CALayer";
    Base.InheritanceStatus = "resolved";
    Base.IvarStatus = "recovered";
    Image.ObjCClasses.push_back(Base);
  }
  void addSegment(va_t A, SegmentFlags Flags) {
    Segment S;
    S.VA = A;
    S.Size = S.FileSz = 0x1000;
    S.FileOff = Image.Segments.size() * 0x1000;
    S.Flags = Flags;
    S.Data.resize(0x1000);
    Image.Segments.push_back(S);
    Section R;
    R.VA = A;
    R.Size = R.FileSz = 0x1000;
    R.FileOff = S.FileOff;
    R.Flags = Flags;
    Image.Sections.push_back(R);
  }
  uint8_t *bytes(va_t A) {
    for (auto &S : Image.Segments)
      if (A >= S.VA && A < S.VA + S.Size)
        return S.Data.data() + A - S.VA;
    return nullptr;
  }
  void u32(va_t A, uint32_t V) {
    llvm::support::endian::write32le(bytes(A), V);
  }
  void u64(va_t A, uint64_t V) {
    llvm::support::endian::write64le(bytes(A), V);
  }
  void relative(va_t A, va_t B) { u32(A, uint32_t(B - A)); }
  void pointer(va_t A, va_t B) {
    u64(A, B);
    Image.DataPtrRelocSlots.insert(A);
    Image.DataPtrRelocTargetOwners[A] = Image.getSectionFor(B)->VA;
  }
  void text(va_t A, const std::string &S) {
    std::copy(S.c_str(), S.c_str() + S.size() + 1, bytes(A));
  }
  void run() {
    PipelineOptions O;
    O.EmitDumpOutput = false;
    O.OnlyFunctionEntries = {Entry};
    const auto ABI = swiftMangledZeroArgClassMethodSourceABI(Image, Entry);
    ASSERT_TRUE(ABI);
    O.SourceTypeHints.emplace(Entry, *ABI);
    Result = Pipeline().run(Image, Context, O);
    ASSERT_TRUE(Result.Success) << Result.Error;
  }
  HighFunc *high() {
    for (auto &F : Result.HighFuncs)
      if (F.Entry == Entry)
        return &F;
    return nullptr;
  }
  LowFunc *low() {
    for (auto &F : Result.LowFuncs)
      if (F.Entry == Entry)
        return &F;
    return nullptr;
  }
  static ExprPtr call(HighFunc &F) {
    ExprPtr Result;
    walkStmts(F.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        std::vector<ExprPtr> P{E};
        while (!P.empty()) {
          auto V = P.back();
          P.pop_back();
          if (!V)
            continue;
          if (V->SourceCallHint && V->SourceCallHint->NativeSwiftReceiver)
            Result = V;
          V->forEachChildExpr([&](const ExprPtr &C) { P.push_back(C); });
        }
      });
    });
    return Result;
  }
};
} // namespace

TEST(SwiftFieldReceiver, ReflectionAndIvarIdentityRemainSeparateFromLayout) {
  FieldFixture F;
  const auto C = swiftObjCClassIdentity(F.Image, 0x5000);
  ASSERT_TRUE(C);
  EXPECT_EQ(C->Module, "Demo");
  EXPECT_EQ(C->Name, "Derived");
  EXPECT_EQ(swiftObjCStoredFieldClass(F.Image, C->RuntimeName, F.Slot),
            "CALayer");
  EXPECT_TRUE(F.Image.ObjCClasses[0].Ivars[0].TypeEncoding.empty());
  const auto Unambiguous = objcSelectorSourceTypeHint(F.Image, "setTransform:");
  ASSERT_TRUE(Unambiguous);
  EXPECT_EQ(Unambiguous->Parameters[2].Type->Size, 128U);
  auto Conflict = F.Image;
  ObjCMethod Affine;
  Affine.ClassName = "OtherView";
  Affine.Selector = "setTransform:";
  Affine.TypeEncoding = "v64@0:8{CGAffineTransform=dddddd}16";
  Affine.TypeHint =
      parseObjCMethodEncoding(Affine.Selector, Affine.TypeEncoding);
  ASSERT_TRUE(Affine.TypeHint);
  Conflict.ObjCMethods.push_back(Affine);
  EXPECT_FALSE(objcSelectorSourceTypeHint(Conflict, "setTransform:"));
  const auto Root = objcNativeSwiftSelfTypeHint(F.Image, F.Entry);
  ASSERT_TRUE(Root);
  const auto Field = objcReceiverIvarTypeHint(F.Image, *Root, F.Slot);
  ASSERT_TRUE(Field);
  const auto Decl =
      objcReceiverSourceTypeHint(F.Image, "setTransform:", *Field);
  // The complete class declaration now has a logical record and a physical
  // copy pointer. Neither reflection nor the ABI proves the call's storage.
  EXPECT_TRUE(Decl.HasDeclaration);
  ASSERT_TRUE(Decl.Signature);
  ASSERT_EQ(Decl.Signature->Parameters.size(), 3U);
  EXPECT_EQ(Decl.Signature->Parameters[2].Type->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Decl.Signature->Parameters[2].Type->Size, 128U);
  EXPECT_TRUE(Decl.Signature->Parameters[2].IndirectByValue);
  EXPECT_EQ(Decl.Signature->Parameters[2].Location.ValueBytes, 8U);
  const auto Void =
      objcReceiverSourceTypeHint(F.Image, "removeAllAnimations", *Field);
  ASSERT_TRUE(Void.Signature);
  EXPECT_EQ(Void.Signature->ReturnType->Kind, NdTypeKind::Void);
  auto Other = F.Image;
  Other.Arch = Arch::X64;
  EXPECT_EQ(swiftObjCStoredFieldClass(Other, C->RuntimeName, F.Slot),
            "CALayer");
  EXPECT_FALSE(objcNativeSwiftSelfTypeHint(Other, F.Entry));
  for (const auto Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    Other.Format = Format;
    EXPECT_FALSE(swiftObjCStoredFieldClass(Other, C->RuntimeName, F.Slot));
    EXPECT_FALSE(objcNativeSwiftSelfTypeHint(Other, F.Entry));
  }
  Other = F.Image;
  Other.Arch = Arch::ARM;
  EXPECT_FALSE(swiftObjCStoredFieldClass(Other, C->RuntimeName, F.Slot));
  EXPECT_FALSE(objcNativeSwiftSelfTypeHint(Other, F.Entry));
  auto Literal = *Field;
  Literal.Steps[0].ByteOffset = 8;
  EXPECT_FALSE(objcReceiverTypeHintValid(F.Image, Literal));
  F.u64(F.Slot, 24);
  F.u64(0x5060, 24);
  F.Image.ObjCClasses[0].InstanceStart = 24;
  F.Image.ObjCClasses[0].InstanceSize = 32;
  F.Image.ObjCClasses[0].Ivars[0].Offset = 24;
  F.u32(0x3304, 24);
  F.u32(0x3308, 32);
  EXPECT_EQ(swiftObjCStoredFieldClass(F.Image, C->RuntimeName, F.Slot),
            "CALayer");
}

TEST(SwiftFieldReceiver, IncompleteRecordDeclarationDoesNotIssueCopyReceipt) {
  FieldFixture F;
  F.text(0x3660, "setTransform:");
  F.Image.ObjCSourceReferences[0x3600].Name = "setTransform:";
  const auto Root = objcNativeSwiftSelfTypeHint(F.Image, F.Entry);
  ASSERT_TRUE(Root);
  const auto Field = objcReceiverIvarTypeHint(F.Image, *Root, F.Slot);
  ASSERT_TRUE(Field);
  const auto Declaration =
      objcReceiverSourceTypeHint(F.Image, "setTransform:", *Field);
  ASSERT_TRUE(Declaration.Signature);
  ASSERT_TRUE(Declaration.Signature->Parameters[2].IndirectByValue);
  F.u32(F.Entry + 36, 0x6d0603e0); // only 16 of the last 32 bytes initialized
  F.run();
  ASSERT_TRUE(F.low());
  EXPECT_FALSE(buildObjCSourceCallHints(F.Image, *F.low()).count(F.Call));
  size_t Calls = 0;
  for (const auto &M : F.Result.MedFuncs)
    for (const auto &B : M.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Addr == F.Call && Op.Opcode == NdOp::CALL) {
          ++Calls;
          EXPECT_FALSE(Op.SourceCallHint);
        }
  EXPECT_EQ(Calls, 1U);
}

TEST(SwiftFieldReceiver, InitializedRecordCopyKeepsLogicalMessageAndPublishes) {
  FieldFixture F;
  F.text(0x3660, "setTransform:");
  F.Image.ObjCSourceReferences[0x3600].Name = "setTransform:";
  F.run();
  ASSERT_TRUE(F.high());
  auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
  const auto Call = F.call(Bound.Function);
  ASSERT_TRUE(Call);
  ASSERT_EQ(Call->Operands.size(), 3U);
  EXPECT_EQ(Call->Operands[2]->Type->Size, 8U);
  EXPECT_EQ(Call->SourceCallHint->Signature.Parameters[2].Type->Size, 128U);
  EXPECT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
      *Call, F.Image, F.Result, Bound.Function, {}));
}

TEST(SwiftFieldReceiver, RecordCopyRejectsStaleStorageAndPublication) {
  for (unsigned Case = 0; Case < 30; ++Case) {
    SCOPED_TRACE(Case);
    FieldFixture F;
    F.text(0x3660, "setTransform:");
    F.Image.ObjCSourceReferences[0x3600].Name = "setTransform:";
    F.run();
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    auto Call = F.call(Bound.Function);
    ASSERT_TRUE(Call);
    ASSERT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = Hint;
    MedOp *Med = nullptr;
    for (auto &M : F.Result.MedFuncs)
      for (auto &B : M.Blocks)
        for (auto &Op : B.Ops)
          if (Op.Addr == F.Call && Op.Opcode == NdOp::CALL)
            Med = &Op;
    ASSERT_NE(Med, nullptr);
    auto Store =
        std::find_if(Bound.Function.Body.begin(), Bound.Function.Body.end(),
                     [](const auto &S) { return S.Kind == StmtKind::Store; });
    ASSERT_NE(Store, Bound.Function.Body.end());
    switch (Case) {
    case 0:
      Hint->ByValueCopy.reset();
      break;
    case 1:
      Hint->ByValueCopy->FrameOffset += 8;
      break;
    case 2:
      Hint->ByValueCopy->Bytes = 64;
      break;
    case 3:
      Hint->ByValueCopy->Parameter = 1;
      break;
    case 4:
      ++Hint->ByValueCopy->FunctionEntry;
      break;
    case 5:
      ++Hint->ByValueCopy->Site.Sequence;
      break;
    case 6:
      Hint->Signature.Parameters[2].Type = NdType::makePtr(NdType::makeVoid());
      break;
    case 7:
      Hint->Signature.Parameters[2].Location.ValueBytes = 4;
      break;
    case 8:
      Hint->Signature.Parameters[2].IndirectByValue = false;
      break;
    case 9:
      Hint->NativeSwiftReceiver.reset();
      break;
    case 10:
      Call->Operands[2] = Call->Operands[0];
      break;
    case 11: {
      HighStmt S;
      S.Kind = StmtKind::Call;
      S.CallExpr = Call;
      Bound.Function.Body.push_back(S);
      break;
    }
    case 12:
      Store->StoreVal = HighExpr::makeConst(1, Store->StoreVal->Type->Size);
      break;
    case 13:
      Bound.Function.Body.erase(Store);
      break;
    case 14:
      std::swap(Bound.Function.Body[0], Bound.Function.Body[1]);
      break;
    case 15:
      Bound.Function.FrameSize += 16;
      break;
    case 16: {
      auto H = std::make_shared<SourceCallTypeHint>(*Med->SourceCallHint);
      H->ByValueCopy.reset();
      Med->SourceCallHint = H;
      break;
    }
    case 17:
      for (auto &B : F.low()->Blocks)
        for (auto &Op : B.Ops)
          if (Op.Opcode == NdOp::STORE)
            Op.Inputs[1] = NdVar::scalar(1, Op.Inputs[1].Size);
      break;
    case 18:
      F.u32(F.Entry + 36, 0xd503201f);
      break;
    case 19:
      Bound.Function.SourceTypeHint->Parameters[0].Location.RegisterOffset =
          a64reg::X0;
      break;
    case 20:
      Hint->WeakImport = true;
      break;
    case 21:
      Call->Operands[2] = HighExpr::makeConst(0, 4);
      break;
    case 22: {
      HighStmt S = *Store;
      Bound.Function.Body.push_back(S);
      break;
    }
    case 23:
      Call->Type = NdType::makeInt(8, false);
      break;
    case 24:
      for (auto &M : F.Result.MedFuncs)
        for (auto &B : M.Blocks)
          for (auto &Op : B.Ops)
            if (Op.Opcode == NdOp::STORE)
              Op.Inputs[1] = MedVar::makeConst(1, Op.Inputs[1].Size);
      break;
    case 25:
      Hint->Receiver->Steps[0].OffsetSlot += 8;
      break;
    case 26:
      Call->Operands[0] = HighExpr::makeConst(0, 8);
      break;
    case 27:
      Call->CallAddr += 4;
      break;
    case 28: {
      Hint->ByValueCopy.reset();
      auto H = std::make_shared<SourceCallTypeHint>(*Med->SourceCallHint);
      H->ByValueCopy.reset();
      Med->SourceCallHint = H;
      break;
    }
    case 29:
      Hint->Signature.Parameters[2].Type =
          NdType::makeStruct(std::vector<TypeRef>(6, NdType::makeFloat(8)));
      break;
    }
    EXPECT_FALSE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
  }
}

TEST(SwiftFieldReceiver, MutableObjCInitializersDoNotMakeReflectionMutable) {
  FieldFixture F;
  std::copy(F.bytes(0x3300), F.bytes(0x3300) + 72, F.bytes(0x5300));
  std::copy(F.bytes(0x3400), F.bytes(0x3400) + 40, F.bytes(0x5400));
  F.pointer(0x5020, 0x5302);
  F.pointer(0x5318, 0x3500);
  F.pointer(0x5330, 0x5400);
  F.pointer(0x5408, F.Slot);
  F.pointer(0x5410, 0x31a0);
  F.pointer(0x5418, 0x31f0);
  F.Image.ObjCClasses[0].Ivars[0].MetadataAddress = 0x5408;
  EXPECT_EQ(swiftObjCStoredFieldClass(F.Image, "_TtC4Demo7Derived", F.Slot),
            "CALayer");
  EXPECT_FALSE(readImmutableImageBytes(F.Image, 0x5300, 12));
  EXPECT_TRUE(readInitialImageBytes(F.Image, 0x5300, 12));
  F.Image.DataPtrRelocSlots.insert(0x5424);
  EXPECT_FALSE(swiftObjCStoredFieldClass(F.Image, "_TtC4Demo7Derived", F.Slot));
}

TEST(SwiftFieldReceiver, NativeEntryAndDynamicFieldReachCompleteMessageABI) {
  FieldFixture F;
  F.run();
  ASSERT_NE(F.low(), nullptr);
  ASSERT_NE(F.high(), nullptr);
  auto H = buildObjCSourceCallHints(F.Image, *F.low());
  ASSERT_TRUE(H.count(F.Call));
  ASSERT_TRUE(H.at(F.Call).NativeSwiftReceiver);
  ASSERT_TRUE(H.at(F.Call).Receiver);
  EXPECT_EQ(H.at(F.Call).Receiver->Steps.size(), 1u);
  auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
  const auto Call = F.call(Bound.Function);
  ASSERT_TRUE(Call);
  EXPECT_FALSE(sdk::objcSourceCallBound(*Call, F.Image, {}, nullptr, nullptr,
                                        &Bound.Function));
  EXPECT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
      *Call, F.Image, F.Result, Bound.Function, {}));
}

TEST(SwiftFieldReceiver, ReflectionRejectsContradictoryOrIncompleteOwners) {
  for (unsigned M = 0; M < 60; ++M) {
    SCOPED_TRACE(M);
    FieldFixture F;
    switch (M) {
    case 0:
      F.u32(0x3000, 0xd0);
      break;
    case 1:
      F.u32(0x3000, 0x20000050);
      break;
    case 2:
      F.u32(0x3000, 0x10050);
      break;
    case 3:
      F.u32(0x3004, 0x7d);
      break;
    case 4:
      F.u32(0x3084, 4);
      break;
    case 5:
      F.text(0x30b0, "Fake");
      break;
    case 6:
      F.text(0x30c0, "Renamed");
      break;
    case 7:
      F.u32(0x3108, (12u << 16) | 1);
      break;
    case 8:
      F.u32(0x3108, (16u << 16) | 7);
      break;
    case 9:
      F.u32(0x310c, 2);
      break;
    case 10:
      F.u32(0x3024, 65);
      break;
    case 11:
      F.u32(0x3028, 13);
      break;
    case 12:
      F.relative(0x3181, 0x3800);
      break;
    case 13:
      F.relative(0x3189, 0x3000);
      break;
    case 14:
      F.u32(0x3110, 4);
      break;
    case 15:
      F.text(0x3190, "So7CALayerCSg");
      break;
    case 16:
      F.text(0x31a0, "other");
      break;
    case 17:
      F.Image.Symbols[1].Name = "_$s4Demo7DerivedC5layerSo6UIViewCvpWvd";
      break;
    case 18:
      F.Image.Symbols[1].Name = "_$s4Demo7DerivedC5layerSo7CALayerCvg";
      break;
    case 19:
      F.Image.Symbols[1].Name = "_$s4Demo7DerivedC5layerSo7CALayerCvpWvi";
      break;
    case 20:
      F.Image.Symbols[1].Size = 4;
      break;
    case 21:
      F.Image.Symbols.push_back(F.Image.Symbols[1]);
      break;
    case 22:
      F.Image.ObjCClasses.push_back(F.Image.ObjCClasses[0]);
      break;
    case 23:
      F.Image.ObjCClasses[0].InheritanceStatus = "unresolved";
      break;
    case 24:
      F.Image.ObjCClasses[0].SuperclassAddress += 8;
      break;
    case 25:
      F.Image.ObjCClasses[0].Name = "_TtC4Demo5Other";
      break;
    case 26:
      F.Image.ObjCClasses[0].Ivars[0].MetadataAddress += 8;
      break;
    case 27:
      F.Image.ObjCClasses[0].Ivars[0].TypeEncoding = "@";
      break;
    case 28:
      F.Image.ObjCClasses[0].Ivars[0].Size = 4;
      break;
    case 29:
      F.Image.ObjCClasses[0].Ivars[0].Alignment = 4;
      break;
    case 30:
      F.u32(0x3420, 2);
      break;
    case 31:
      F.u32(0x3424, 4);
      break;
    case 32:
      F.u64(F.Slot, 16);
      break;
    case 33:
      F.u64(0x5060, 16);
      break;
    case 34:
      F.Image.ObjCSourceReferences[F.Slot].Size = 4;
      break;
    case 35:
      F.Image.ObjCSourceReferences[F.Slot].ClassName = "Other";
      break;
    case 36:
      F.Image.MachOChainedFixupsAmbiguous = true;
      break;
    case 37:
      F.Image.MachOHasChainedFixups = true;
      break;
    case 38:
      F.Image.IsRelocatable = true;
      break;
    case 39:
      F.Image.Bits = Bitness::Bits32;
      break;
    case 40:
      F.Image.Segments[1].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Sections[1].Flags = F.Image.Segments[1].Flags;
      break;
    case 41:
      F.Image.Sections[1].FileSz = 0x110;
      break;
    case 42:
      F.Image.DyldBindSlots[0x5040] = {"_descriptor", 0, "bad", false};
      break;
    case 43:
      F.Image.DataPtrRelocSlots.insert(0x3114);
      break;
    case 44:
      F.u32(0x3400, 40);
      break;
    case 45:
      F.u32(0x3308, 32);
      break;
    case 46:
      *F.bytes(0x3185) = 1;
      break;
    case 47:
      F.u64(0x5020, 0x3301);
      break;
    case 48:
      F.Image.Symbols.push_back(
          {F.Image.Symbols[1].Name, F.Slot + 16, 8, false});
      break;
    case 49:
      F.Image.Symbols.push_back({"overlap", F.Slot - 4, 8, false});
      break;
    case 50:
      F.Image.Symbols.push_back({"interior", F.Slot + 4, 0, false});
      break;
    case 51:
      F.Image.DataPtrRelocSlots.erase(0x3418);
      break;
    case 52:
      F.Image.DataPtrRelocTargetOwners.erase(0x3410);
      break;
    case 53:
      F.Image.DataPtrRelocSlots.insert(0x3409);
      break;
    case 54:
      F.Image.DyldBindSlots[0x3418] = {"_type", 0, "bad", false};
      break;
    case 55:
      F.Image.DataPtrRelocSlots.insert(0x3424);
      break;
    case 56:
      F.Image.Sections.push_back(F.Image.Sections[2]);
      break;
    case 57:
      F.Image.DataPtrRelocTargetOwners[0x3408] = 0x1000;
      break;
    case 58:
      F.Image.DataPtrRelocSlots.erase(0x5020);
      break;
    case 59:
      F.Image.DataPtrRelocSlots.erase(0x5008);
      break;
    }
    EXPECT_FALSE(
        swiftObjCStoredFieldClass(F.Image, "_TtC4Demo7Derived", F.Slot));
  }
}

TEST(SwiftFieldReceiver, NativeReceiverRejectsStaleMachineAndPublication) {
  for (unsigned M = 0; M < 18; ++M) {
    SCOPED_TRACE(M);
    FieldFixture F;
    F.run();
    ASSERT_NE(F.high(), nullptr);
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    auto Call = F.call(Bound.Function);
    ASSERT_TRUE(Call);
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = Hint;
    switch (M) {
    case 0:
      Hint->NativeSwiftReceiver.reset();
      break;
    case 1:
      Hint->Receiver->Address += 4;
      break;
    case 2:
      Hint->Receiver->ClassName = "_TtC4Demo4Base";
      break;
    case 3:
      Hint->Receiver->Steps[0].OffsetSlot += 8;
      break;
    case 4:
      Hint->Receiver->Steps[0].ByteOffset = 8;
      break;
    case 5:
      Call->Operands[0] = HighExpr::makeConst(0, 8);
      break;
    case 6:
      Hint->NativeSwiftReceiver->Instruction += 4;
      break;
    case 7:
      F.u32(F.Entry + 16, 0xf8686a60);
      break; // receiver is x19, not swiftself
    case 8:
      F.Image.Symbols[0].Name = "_$s4Demo7DerivedC6layoutyySbF";
      break;
    case 9:
      Bound.Function.SourceTypeHint->Parameters[0].Location.RegisterOffset = 0;
      break;
    case 10:
      Bound.Function.Params[0].Type = NdType::makeInt(4);
      break;
    case 11:
      Call->Operands[0]->Type = NdType::makeFloat(8);
      break;
    case 12:
      Call->IsIndirectCall = true;
      break;
    case 13:
      Hint->Signature.Parameters[0].Type = NdType::makeInt(4);
      break;
    case 14:
      F.Result.LowFuncs[0].Blocks[0].Ops.front().Addr += 4;
      break;
    case 15:
      F.Result.FunctionAudits[0].MedIRVerified = false;
      break;
    case 16:
      F.u32(0x3108, (12u << 16) | 1);
      break;
    case 17:
      Hint->Receiver->Steps[0].OffsetWidth = 4;
      break;
    }
    EXPECT_FALSE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
  }
}

TEST(SwiftFieldReceiver, PublicationKeepsArgumentsAndUniqueEvaluation) {
  for (unsigned M = 0; M < 11; ++M) {
    SCOPED_TRACE(M);
    FieldFixture F;
    F.text(0x3660, "setMasksToBounds:");
    F.Image.ObjCSourceReferences[0x3600].Name = "setMasksToBounds:";
    F.u32(F.Entry + 40, 0x52800022); // mov w2, #1
    F.run();
    ASSERT_NE(F.high(), nullptr);
    auto Bound = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
    auto Call = F.call(Bound.Function);
    ASSERT_TRUE(Call);
    ASSERT_EQ(Call->Operands.size(), 3u);
    ASSERT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
    MedOp *Med = nullptr;
    for (auto &B : F.Result.MedFuncs.front().Blocks)
      for (auto &Op : B.Ops)
        if (Op.SourceCallHint && Op.SourceCallHint->NativeSwiftReceiver)
          Med = &Op;
    ASSERT_NE(Med, nullptr);
    switch (M) {
    case 0: {
      auto H = std::make_shared<SourceCallTypeHint>(*Med->SourceCallHint);
      H->NativeSwiftReceiver.reset();
      Med->SourceCallHint = H;
      break;
    }
    case 1:
      Med->Inputs[3].Size = 4;
      break;
    case 2:
      Med->SourceCallHint.reset();
      break;
    case 3:
      Call->Operands[2] = HighExpr::makeConst(0, 1);
      break;
    case 4:
      Call->Operands[2] = HighExpr::makeConst(0, 1);
      F.call(*F.high())->Operands[2] = HighExpr::makeConst(0, 1);
      break;
    case 5: {
      HighStmt S;
      S.Kind = StmtKind::Call;
      S.CallExpr = Call;
      Bound.Function.Body.push_back(S);
      break;
    }
    case 6:
      Bound.Function.DoesNotReturn = true;
      break;
    case 7:
      Call->Operands[1] = HighExpr::makeConst(0, 8);
      break;
    case 8:
      Med->DoesNotReturn = true;
      break;
    case 9:
      Med->PreservesCallerSaved = true;
      break;
    case 10:
      F.Image.Symbols.push_back(F.Image.Symbols[0]);
      break;
    }
    EXPECT_FALSE(sdk::objCNativeSwiftReceiverSourceCallBound(
        *Call, F.Image, F.Result, Bound.Function, {}));
  }
}

TEST(SwiftFieldReceiver, NativeSelfRequiresEveryReachingMachinePath) {
  for (unsigned M = 0; M < 5; ++M) {
    SCOPED_TRACE(M);
    FieldFixture F;
    std::vector<uint32_t> W;
    for (unsigned I = 0; I < 15; ++I)
      W.push_back(llvm::support::endian::read32le(F.bytes(F.Entry + I * 4)));
    W.insert(W.begin() + 2,
             {0xaa1403e9, 0xb4000074, 0xaa0903f4, 0x14000002, 0xaa0903f4});
    W[16] = 0x94000070;
    if (M == 1)
      W[6] = 0xaa0003f4; // another entry argument on one predecessor
    if (M == 2)
      W[6] = 0x2a0903f4; // partial-width self on one predecessor
    for (unsigned I = 0; I < W.size(); ++I)
      F.u32(F.Entry + I * 4, W[I]);
    F.Image.Symbols[0].Size = W.size() * 4;
    F.run();
    ASSERT_NE(F.low(), nullptr);
    auto H = buildObjCSourceCallHints(F.Image, *F.low());
    auto Count = [](const auto &Hints) {
      return llvm::count_if(Hints, [](const auto &V) {
        return bool(V.second.NativeSwiftReceiver);
      });
    };
    if (M == 1 || M == 2) {
      EXPECT_EQ(Count(H), 0);
      continue;
    }
    ASSERT_EQ(Count(H), 1);
    if (M == 3) {
      std::reverse(F.low()->Blocks.begin(), F.low()->Blocks.end());
      auto Reordered = buildObjCSourceCallHints(F.Image, *F.low());
      ASSERT_EQ(Count(Reordered), 1);
      EXPECT_EQ(Reordered.at(F.Entry + 64).Receiver,
                H.at(F.Entry + 64).Receiver);
    } else if (M == 4) {
      F.low()->Blocks.front().Succs.clear();
      EXPECT_EQ(Count(buildObjCSourceCallHints(F.Image, *F.low())), 0);
    } else {
      auto B = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
      auto C = F.call(B.Function);
      ASSERT_TRUE(C);
      EXPECT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(
          *C, F.Image, F.Result, B.Function, {}));
    }
  }
}

TEST(SwiftFieldReceiver, OpaqueExitsGrantNoReceiverPublication) {
  FieldFixture F;
  F.run();
  ASSERT_NE(F.high(), nullptr);
  auto B = sdk::bindObjCSourceReferences(*F.high(), F.Image, {});
  auto C = F.call(B.Function);
  ASSERT_TRUE(C);
  ASSERT_TRUE(sdk::objCNativeSwiftReceiverSourceCallBound(*C, F.Image, F.Result,
                                                          B.Function, {}));
  F.u32(F.Entry + 56,
        0xd4200000); // canonical frame replay excludes opaque exits
  F.run();
  ASSERT_NE(F.high(), nullptr);
  ASSERT_TRUE(F.high()->DoesNotReturn);
  const auto Current = buildObjCSourceCallHints(F.Image, *F.low());
  EXPECT_FALSE(llvm::any_of(Current, [](const auto &H) {
    return bool(H.second.NativeSwiftReceiver);
  }));
  EXPECT_FALSE(sdk::objCNativeSwiftReceiverSourceCallBound(
      *C, F.Image, F.Result, B.Function, {}));
}

namespace {
struct FixedRecordFixture {
  BinaryImage Image;
  static constexpr va_t Descriptor = 0x3000, Class = 0x3080;
  static constexpr va_t Metadata = 0x3600, Witnesses = 0x3700;
  FixedRecordFixture(Arch Architecture) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.MachOTwoLevelNamespace = true;
    Image.MachOHasChainedFixups = true;
    segment(0x1000, 0x100, "__text", true);
    segment(0x3000, 0x1000, "__const");
    segment(0x5000, 8, "__swift5_types");
    segment(0x6000, 0x100, "__got");
    Image.Sections.back().Type = llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS;
    for (unsigned I = 0; I != 9; ++I) {
      u32(0x1000 + I * 4, Architecture == Arch::AArch64 ? 0xd65f03c0 : 0xc3);
      Image.RuntimeFunctionAddrs.insert(0x1000 + I * 4);
    }
    u32(Descriptor, 0x51);
    relative(Descriptor + 4, 0x3140);
    relative(Descriptor + 8, 0x3100);
    relative(Descriptor + 12, 0x1020);
    relative(Descriptor + 16, 0x3200);
    u32(Descriptor + 20, 4);
    u32(Descriptor + 24, 2);
    text(0x3100, "Value");
    relative(0x3148, 0x3150);
    text(0x3150, "Demo");
    u32(Class, 0x50);
    relative(Class + 4, 0x3140);
    relative(Class + 8, 0x3160);
    text(0x3160, "Object");
    relative(0x5000, Descriptor);
    relative(0x5004, Class);
    Image.Symbols.push_back({"_$s4Demo5ValueVMn", Descriptor, 0, false});
    Image.Symbols.push_back({"_$s4Demo6ObjectCMn", Class, 0, false});
    Image.Symbols.push_back({"_$s4Demo5ValueVN", Metadata, 0, false});
    u64(Metadata, 0x200);
    pointer(Metadata + 8, Descriptor);
    pointer(Metadata - 8, Witnesses);
    relative(0x3200, 0x3260);
    u32(0x3208, 12u << 16);
    u32(0x320c, 4);
    symbolic(0x3260, Descriptor, 1);
    symbolic(0x3270, 0x6000, 2);
    text(0x3280, "Sd");
    symbolic(0x3290, Class, 1);
    const va_t Types[] = {0x3270, 0x3270, 0x3280, 0x3290};
    for (unsigned I = 0; I != 4; ++I) {
      u32(0x3210 + I * 12, 2);
      relative(0x3214 + I * 12, Types[I]);
      relative(0x3218 + I * 12, 0x3420 + I * 32);
      text(0x3420 + I * 32, "field" + std::to_string(I));
      u32(Metadata + 16 + I * 4, I * 8);
    }
    for (unsigned I = 0; I != 8; ++I)
      pointer(Witnesses + I * 8, 0x1000 + I * 4, true);
    u64(Witnesses + 64, 32);
    u64(Witnesses + 72, 32);
    u32(Witnesses + 80, 0x30007);
    u32(Witnesses + 84, 0x7fffffff);
    EXPECT_TRUE(Image.recordDyldBindSlot(
        0x6000, "_$s12CoreGraphics7CGFloatVMn", 0,
        "/usr/lib/swift/libswiftCoreFoundation.dylib", false));
  }
  void segment(va_t Address, uint64_t Size, const char *Name,
               bool Code = false) {
    Segment S;
    S.VA = S.FileOff = Address;
    S.Size = S.FileSz = Size;
    S.Flags = SegmentFlags::Readable;
    if (Code)
      S.Flags = S.Flags | SegmentFlags::Executable;
    S.Data.resize(Size);
    Image.Segments.push_back(S);
    Section R;
    R.VA = R.FileOff = Address;
    R.Size = R.FileSz = Size;
    R.Flags = S.Flags;
    R.Name = Name;
    R.Type = Code ? llvm::MachO::S_ATTR_PURE_INSTRUCTIONS : 0;
    Image.Sections.push_back(R);
  }
  uint8_t *bytes(va_t Address) {
    for (auto &S : Image.Segments)
      if (Address >= S.VA && Address - S.VA < S.Size)
        return S.Data.data() + Address - S.VA;
    return nullptr;
  }
  void u32(va_t Address, uint32_t Value) {
    llvm::support::endian::write32le(bytes(Address), Value);
  }
  void u64(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(bytes(Address), Value);
  }
  void relative(va_t Address, va_t Target) {
    u32(Address, uint32_t(Target - Address));
  }
  void text(va_t Address, const std::string &Text) {
    std::memcpy(bytes(Address), Text.c_str(), Text.size() + 1);
  }
  void symbolic(va_t Address, va_t Target, uint8_t Kind) {
    *bytes(Address) = Kind;
    relative(Address + 1, Target);
    *bytes(Address + 5) = 0;
  }
  void pointer(va_t Address, va_t Target, bool Code = false) {
    u64(Address, Target);
    Image.MachOResolvedChainedPointerSlots.insert(Address);
    (Code ? Image.CodePtrRelocSlots : Image.DataPtrRelocSlots).insert(Address);
    if (!Code)
      Image.DataPtrRelocTargetOwners[Address] = Image.getSectionFor(Target)->VA;
  }
  std::optional<SwiftFixedRecordStorage> prove() {
    return swiftFixedRecordStorage(Image, Descriptor);
  }
};
} // namespace

TEST(SwiftFixedRecordStorage, IndependentNominalFieldsAndWitnessesAgree) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    FixedRecordFixture F(A);
    ASSERT_EQ(swiftLocalRegisteredNominalType(F.Image, F.Descriptor),
              "4Demo5ValueV");
    ASSERT_EQ(swiftLocalRegisteredNominalType(F.Image, F.Class),
              "4Demo6ObjectC");
    const auto R = F.prove();
    ASSERT_TRUE(R);
    EXPECT_EQ(R->MangledType, "4Demo5ValueV");
    EXPECT_EQ(R->Metadata, F.Metadata);
    EXPECT_EQ(R->Size, 32u);
    EXPECT_EQ(R->Alignment, 8u);
    for (unsigned I = 0; I != 4; ++I) {
      EXPECT_EQ(R->Fields[I].Offset, I * 8);
      EXPECT_EQ(R->Fields[I].Name, "field" + std::to_string(I));
      EXPECT_TRUE(R->Fields[I].IsMutable);
      EXPECT_EQ(R->Fields[I].Storage,
                I == 3 ? SwiftFixedRecordField::Kind::StrongReference
                       : SwiftFixedRecordField::Kind::Float64);
    }
    EXPECT_EQ(R->Fields[0].MangledType, "12CoreGraphics7CGFloatV");
    EXPECT_EQ(R->Fields[2].MangledType, "Sd");
    EXPECT_EQ(R->Fields[3].MangledType, "4Demo6ObjectC");
    // This physical storage certificate cannot widen Swift source declarations.
    const auto Source = recoverSwiftTypes(F.Image);
    ASSERT_FALSE(Source.empty());
    EXPECT_NE(Source[0].Status, "recovered");
    // Every ordinary Double field and immutable stored property is also valid.
    for (unsigned I = 0; I != 3; ++I)
      F.relative(0x3214 + I * 12, 0x3280);
    for (unsigned I = 0; I != 4; ++I)
      F.u32(0x3210 + I * 12, 0);
    ASSERT_TRUE(F.prove());
  }
}

TEST(SwiftFixedRecordStorage, RejectsStaleOrIncompleteStorageEvidence) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 47; ++Mutation) {
      SCOPED_TRACE(Mutation);
      FixedRecordFixture F(A);
      ASSERT_TRUE(F.prove());
      switch (Mutation) {
      case 0:
        F.u32(F.Descriptor, 0xd1);
        break; // Generic instantiation required.
      case 1:
        F.u32(F.Descriptor, 0x151);
        break; // Unrecognized descriptor version.
      case 2:
        F.u32(F.Descriptor, 0x10051);
        break; // Dynamic metadata initialization.
      case 3:
        F.u32(F.Descriptor + 20, 3);
        break;
      case 4:
        F.u32(F.Descriptor + 24, 3);
        break;
      case 5:
        F.relative(F.Descriptor + 12, 0x1040);
        break; // No authenticated accessor entry.
      case 6:
        F.Image.Symbols[0].Name = "_$s4Demo5OtherVMn";
        break;
      case 7:
        F.Image.Symbols[1].Name = "_$s4Demo6ObjectVMn";
        break;
      case 8:
        F.relative(0x5004, F.Descriptor);
        break; // Reference class unregistered.
      case 9:
        F.u32(0x5000, 1);
        break;
      case 10:
        F.Image.Symbols.push_back(F.Image.Symbols[2]);
        break;
      case 11:
        F.Image.Exports.push_back({F.Image.Symbols[0].Name, 0, F.Descriptor});
        break;
      case 12:
        F.Image.Symbols[2].Name = "_$s4Demo5OtherVN";
        break;
      case 13:
        F.u64(F.Metadata, 0x201);
        break;
      case 14:
        F.pointer(F.Metadata + 8, F.Class);
        break;
      case 15:
        F.u32(F.Metadata + 24, 24);
        break; // Overlapping stored fields.
      case 16:
        F.u32(F.Metadata + 28, 32);
        break; // A gap cannot be hidden by stride.
      case 17:
        F.u64(F.Witnesses + 64, 24);
        break;
      case 18:
        F.u64(F.Witnesses + 72, 40);
        break;
      case 19:
        F.u32(F.Witnesses + 80, 0x400000 | 0x30007);
        break; // Incomplete layout.
      case 20:
        F.u32(F.Witnesses + 80, 0x20007);
        break; // POD contradicts strong reference.
      case 21:
        F.u32(F.Witnesses + 80, 0x130007);
        break; // Other lifetime representation.
      case 22:
        F.Image.CodePtrRelocSlots.erase(F.Witnesses + 16);
        break;
      case 23:
        F.Image.MachOResolvedChainedPointerSlots.erase(F.Metadata + 8);
        break;
      case 24:
        F.Image.DataPtrRelocSlots.insert(F.Metadata + 17);
        break;
      case 25:
        F.u32(0x3208, (16u << 16));
        break;
      case 26:
        F.u32(0x320c, 5);
        break;
      case 27:
        F.relative(0x3200, 0x3290);
        break; // Field descriptor belongs to other type.
      case 28:
        F.u32(0x3204, 4);
        break; // Struct cannot declare a superclass.
      case 29:
        F.u32(0x3210, 4);
        break;
      case 30:
        F.relative(0x323c, 0x3420);
        break; // Duplicate field name.
      case 31:
        F.text(0x3280, "Sf");
        break; // Float is not Float64.
      case 32:
        F.text(0x3280, "SdSg");
        break; // Optional has a distinct representation.
      case 33:
        F.text(0x3295, "Xw");
        break; // Weak reference storage is not a strong pointer.
      case 34:
        F.text(0x3295, "Xo");
        break; // Unowned reference.
      case 35:
        F.text(0x3295, "Sg");
        break;
      case 36:
        F.symbolic(0x3290, F.Descriptor, 1);
        break;
      case 37:
        F.u32(F.Class, 0xd0);
        break; // Generic class type lacks arguments.
      case 38:
        F.Image.DyldBindSlots[0x6000].Module =
            "/usr/lib/swift/libswiftCore.dylib";
        break;
      case 39:
        F.Image.DyldBindSlots[0x6000].WeakImport = true;
        break;
      case 40:
        F.Image.DyldBindSlots[0x6000].Addend = 8;
        break;
      case 41:
        F.Image.Sections.back().Type =
            llvm::MachO::S_THREAD_LOCAL_VARIABLE_POINTERS;
        break;
      case 42:
        F.Image.Sections.back().Type = llvm::MachO::S_LAZY_SYMBOL_POINTERS;
        break;
      case 43:
        F.Image.ConflictingImportStorageSlots.insert(0x6000);
        break;
      case 44:
        F.Image.Segments[1].Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 45:
        F.Image.RuntimeFunctionAddrs.insert(F.Metadata + 24);
        break;
      case 46:
        F.u64(0x3900, 0x200);
        F.pointer(0x3908, F.Descriptor);
        F.Image.Symbols.push_back({"different_metadata", 0x3900, 0, false});
        break;
      }
      EXPECT_FALSE(F.prove());
    }
}

TEST(SwiftFixedRecordStorage, CurrentPointersBoundsAndIndependentFieldTypes) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    FixedRecordFixture F(A);
    ASSERT_TRUE(F.prove());
    for (unsigned I = 0; I != 8; ++I) {
      auto C = F;
      C.pointer(C.Witnesses + I * 8, 0x1040, true);
      EXPECT_FALSE(C.prove());
    }
    for (unsigned I = 0; I != 4; ++I) {
      auto C = F;
      C.relative(0x3214 + I * 12, I == 3 ? 0x3280 : 0x3290);
      EXPECT_FALSE(C.prove());
    }
    for (unsigned I = 0; I != 3; ++I) {
      auto C = F;
      C.relative(0x3214 + I * 12, 0x3280);
      EXPECT_TRUE(C.prove());
    }
    for (uint32_t Kind : {llvm::MachO::S_THREAD_LOCAL_REGULAR,
                          llvm::MachO::S_THREAD_LOCAL_ZEROFILL,
                          llvm::MachO::S_THREAD_LOCAL_VARIABLES,
                          llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS}) {
      auto C = F;
      C.Image.Sections.back().Type = Kind;
      EXPECT_FALSE(C.prove());
    }
    auto Truncated = F;
    Truncated.Image.Sections[1].FileSz = F.Witnesses + 87 - 0x3000;
    EXPECT_FALSE(Truncated.prove());
    auto Alias = F;
    Alias.Image.Sections.push_back(Alias.Image.Sections[1]);
    EXPECT_FALSE(Alias.prove());
  }
}

TEST(SwiftFixedRecordStorage, RegistrationAndStorageCannotBeThreadLocal) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Section : {1u, 2u})
      for (uint32_t Kind :
           {llvm::MachO::S_THREAD_LOCAL_REGULAR,
            llvm::MachO::S_THREAD_LOCAL_ZEROFILL,
            llvm::MachO::S_THREAD_LOCAL_VARIABLES,
            llvm::MachO::S_THREAD_LOCAL_VARIABLE_POINTERS,
            llvm::MachO::S_THREAD_LOCAL_INIT_FUNCTION_POINTERS}) {
        SCOPED_TRACE(Section);
        SCOPED_TRACE(Kind);
        FixedRecordFixture F(A);
        ASSERT_TRUE(F.prove());
        F.Image.Sections[Section].Type = Kind;
        EXPECT_FALSE(swiftLocalRegisteredNominalType(F.Image, F.Descriptor));
        EXPECT_FALSE(F.prove());
      }
}

TEST(SwiftFixedRecordStorage, PointerRecordsCannotHideInThreadLocalSlices) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (va_t Slot :
         {FixedRecordFixture::Metadata - 8, FixedRecordFixture::Metadata + 8,
          FixedRecordFixture::Witnesses, FixedRecordFixture::Witnesses + 56}) {
      FixedRecordFixture F(A);
      auto Left = F.Image.Sections[1];
      auto Middle = Left, Right = Left;
      Left.Size = Left.FileSz = Slot - Left.VA;
      Middle.VA = Middle.FileOff = Slot;
      Middle.Size = Middle.FileSz = 8;
      Right.VA = Right.FileOff = Slot + 8;
      Right.Size = Right.FileSz = 0x4000 - Right.VA;
      F.Image.Sections[1] = Left;
      F.Image.Sections.push_back(Middle);
      F.Image.Sections.push_back(Right);
      for (auto &[Address, Owner] : F.Image.DataPtrRelocTargetOwners) {
        const auto Target = llvm::support::endian::read64le(F.bytes(Address));
        Owner = F.Image.getSectionFor(Target)->VA;
      }
      ASSERT_TRUE(F.prove());
      F.Image.Sections[F.Image.Sections.size() - 2].Type =
          llvm::MachO::S_THREAD_LOCAL_REGULAR;
      EXPECT_FALSE(F.prove()) << llvm::utohexstr(Slot);
    }
}

namespace {
struct FixedConstructorFixture : FixedRecordFixture {
  static constexpr va_t Entry = 0x1080;
  static constexpr const char *Name =
      "_$s4Demo5ValueV6field06field16field26field34Demo5ValueV"
      "12CoreGraphics7CGFloatV_12CoreGraphics7CGFloatVSdySbcSgtcfC";
  FixedConstructorFixture(Arch A) : FixedRecordFixture(A) {
    u32(Entry, A == Arch::AArch64 ? 0xd65f03c0 : 0x000000c3);
    Image.Symbols.push_back({Name, Entry, A == Arch::AArch64 ? 4u : 1u, true});
    Image.RuntimeFunctionAddrs.insert(Entry);
  }
  auto declaration() {
    return swiftMangledFixedRecordConstructorDeclaration(Image, Entry);
  }
};
} // namespace

TEST(SwiftFixedRecordConstructor, CompleteDeclarationKeepsAllCarriers) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    FixedConstructorFixture F(A);
    const auto D = F.declaration();
    ASSERT_TRUE(D);
    EXPECT_EQ(D->Storage.MangledType, "4Demo5ValueV");
    ASSERT_EQ(D->Signature.Parameters.size(), 5u);
    ASSERT_EQ(sourceABIParameters(D->Signature).size(), 5u);
    ASSERT_EQ(D->Signature.ReturnComponents.size(), 4u);
    EXPECT_EQ(D->Signature.Origin,
              SourceFunctionTypeHint::OriginKind::SwiftMangled);
    std::string Error;
    EXPECT_TRUE(validateSourceABI(D->Signature, Error)) << Error;
    for (unsigned I = 0; I != 3; ++I) {
      EXPECT_EQ(D->Signature.Parameters[I].Type->Kind, NdTypeKind::Float);
      EXPECT_EQ(D->Signature.Parameters[I].Type->Size, 8u);
      const auto FP = A == Arch::AArch64 ? a64reg::V(I) : x86reg::vectorReg(I);
      EXPECT_EQ(D->Signature.Parameters[I].Location.Kind,
                SourceABICarrierKind::FloatingRegister);
      EXPECT_EQ(D->Signature.Parameters[I].Location.RegisterOffset, FP);
      EXPECT_EQ(D->Signature.ReturnComponents[I].RegisterOffset, FP);
      EXPECT_EQ(D->Signature.ReturnComponents[I].ValueBytes, 8u);
    }
    EXPECT_EQ(D->Signature.Parameters[3].Location.RegisterOffset,
              A == Arch::AArch64 ? a64reg::X0 : x86reg::RDI);
    EXPECT_EQ(D->Signature.Parameters[4].Location.RegisterOffset,
              A == Arch::AArch64 ? a64reg::X1 : x86reg::RSI);
    EXPECT_EQ(D->Signature.ReturnComponents[3].RegisterOffset,
              A == Arch::AArch64 ? a64reg::X0 : x86reg::RAX);
    for (unsigned I = 3; I != 5; ++I)
      EXPECT_EQ(D->Signature.Parameters[I].TheRole,
                SourceParameterTypeHint::Role::Ordinary);
  }
}

TEST(SwiftFixedRecordConstructor,
     RejectsDifferentDeclarationsAndCurrentIdentity) {
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 43; ++Mutation) {
      SCOPED_TRACE(Mutation);
      FixedConstructorFixture F(A);
      ASSERT_TRUE(F.declaration());
      auto &Name = F.Image.Symbols.back().Name;
      auto Replace = [&](const char *From, const char *To) {
        const auto P = Name.find(From);
        ASSERT_NE(P, std::string::npos);
        Name.replace(P, std::strlen(From), To);
      };
      switch (Mutation) {
      case 0:
        F.Image.Format = BinaryFormat::ELF;
        break;
      case 1:
        F.Image.IsRelocatable = true;
        break;
      case 2:
        F.Image.Bits = Bitness::Bits32;
        break;
      case 3:
        F.Image.Arch = Arch::Unknown;
        break;
      case 4:
        F.Image.MachOTwoLevelNamespace = false;
        break;
      case 5:
        F.Image.Symbols.push_back(F.Image.Symbols.back());
        break;
      case 6: {
        auto Other = F.Image.Symbols.back();
        Other.Addr += 8;
        F.Image.Symbols.push_back(Other);
        break;
      }
      case 7:
        F.Image.Symbols.push_back({"data_alias", F.Entry, 8, false});
        break;
      case 8:
        F.Image.Symbols.back().IsFunc = false;
        break;
      case 9:
        F.Image.Segments[0].Flags =
            F.Image.Segments[0].Flags | SegmentFlags::Writable;
        break;
      case 10:
        F.Image.DataPtrRelocSlots.insert(F.Entry);
        break;
      case 11:
        Name.insert(0, "_");
        break;
      case 12:
        Name += "unknown";
        break;
      case 13:
        Replace("4Demo5ValueV", "4Demo5ValueC");
        break;
      case 14:
        Name += "Tg5";
        break;
      case 15:
        Replace("cfC", "cfc");
        break;
      case 16:
        Replace("tcfC", "tKcfC");
        break;
      case 17:
        Replace("tcfC", "tYacfC");
        break;
      case 18:
        Replace("ySbcSg", "SiSbcSg");
        break;
      case 19:
        Replace("ySbcSg", "ySucSg");
        break;
      case 20:
        Replace("ySbcSg", "ySbc");
        break;
      case 21:
        Replace("ySbcSg", "ySbXCSg");
        break;
      case 22:
        Replace("ySbcSg", "ySbKcSg");
        break;
      case 23:
        Replace("ySbcSg", "ySbYacSg");
        break;
      case 24:
        Replace("ySbcSg", "ySbcSgSg");
        break;
      case 25:
        Replace("VSdy", "VSfy");
        break;
      case 26:
        Replace("12CoreGraphics7CGFloatV", "Sd");
        break;
      case 27:
        Replace("_12CoreGraphics7CGFloatV", "_Sd");
        break;
      case 28:
        Replace("field34Demo5ValueV", "field34Demo5OtherV");
        break;
      case 29:
        Replace("6field1", "6other1");
        break;
      case 30:
        Replace("6field3", "6field4");
        break;
      case 31:
        F.Image.Symbols[2].Name = "other_metadata";
        break;
      case 32:
        F.u64(F.Witnesses + 64, 24);
        break;
      case 33:
        F.text(0x3295, "Sg");
        break;
      case 34:
        F.Image.Sections[2].Type = llvm::MachO::S_THREAD_LOCAL_REGULAR;
        break;
      case 35:
        F.Image.DyldBindSlots[0x6000].WeakImport = true;
        break;
      case 36:
        F.Image.DyldBindSlots[0x6000].Module = "/usr/lib/libUnknown.dylib";
        break;
      case 37:
        F.u32(0x3210, 4);
        break;
      case 38:
        F.Image.Symbols.push_back(F.Image.Symbols[2]);
        break;
      case 39: {
        auto Other = F.Image.Symbols[0];
        Other.Addr += 8;
        F.Image.Symbols.push_back(Other);
        break;
      }
      case 40:
        F.Image.Exports.push_back({"different_function", 0, F.Entry});
        break;
      case 41:
        F.Image.Exports.push_back({Name, 0, F.Entry + 8});
        break;
      case 42:
        Replace("ySbcSg", "ySbXBSg");
        break;
      }
      EXPECT_FALSE(F.declaration());
    }
}

TEST(SwiftFixedRecordConstructor,
     IndependentFieldTypesAndLiveMetadataStayRequired) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    FixedConstructorFixture F(A);
    ASSERT_TRUE(F.declaration());
    auto &Name = F.Image.Symbols.back().Name;
    for (unsigned I = 0; I != 2; ++I) {
      const auto P = Name.find("12CoreGraphics7CGFloatV");
      ASSERT_NE(P, std::string::npos);
      Name.replace(P, std::strlen("12CoreGraphics7CGFloatV"), "Sd");
      EXPECT_FALSE(F.declaration());
      F.relative(0x3214 + I * 12, 0x3280);
      ASSERT_TRUE(F.declaration());
    }
    const auto Original = F.declaration();
    ASSERT_TRUE(Original);
    F.u32(F.Metadata + 28, 32);
    EXPECT_FALSE(F.declaration());
    F.u32(F.Metadata + 28, 24);
    ASSERT_TRUE(F.declaration());
    F.Image.Symbols.back().Name += "TA";
    EXPECT_FALSE(F.declaration());
    EXPECT_EQ(Original->Signature.Parameters.size(), 5u);
  }
  FixedConstructorFixture F(Arch::AArch64);
  F.Image.Symbols.back().Addr = F.Entry + 1;
  F.Image.RuntimeFunctionAddrs.insert(F.Entry + 1);
  EXPECT_FALSE(
      swiftMangledFixedRecordConstructorDeclaration(F.Image, F.Entry + 1));
}

// A linked, independently named constructor whose machine body forwards all
// three FP lanes and the first opaque input. It exercises current re-lifting;
// its declaration alone cannot certify either a changed body or caller.
namespace {
PipelineResult constructorPipeline(FixedConstructorFixture &F,
                                   llvm::LLVMContext &Context,
                                   bool WithCaller = false) {
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Entry};
  if (WithCaller) {
    constexpr va_t Caller = 0x1100;
    F.segment(Caller, 0x100, "__text_caller", true);
    // An observable external store keeps this ordinary call in the pipeline;
    // the empty identity leaf is intentionally eligible for leaf elimination.
    F.u32(F.Entry, 0xf9000020); // str x0, [x1]
    F.u32(F.Entry + 4, 0xd65f03c0);
    for (auto &S : F.Image.Symbols)
      if (S.Addr == F.Entry)
        S.Size = 8;
    const uint32_t Words[] = {0xa9bf7bfd, 0x910003fd, 0x97ffffde, 0xa8c17bfd,
                              0xd65f03c0};
    for (unsigned I = 0; I != std::size(Words); ++I)
      F.u32(Caller + 4 * I, Words[I]);
    F.Image.Symbols.push_back({"caller", Caller, sizeof(Words), true});
    F.Image.RuntimeFunctionAddrs.insert(Caller);
    Options.OnlyFunctionEntries.insert(Caller);
  }
  auto R = Pipeline().run(F.Image, Context, Options);
  std::map<va_t, std::string> Diagnostics;
  neverd::sdk::inferObjCNativeDependencies(F.Image, R, Options, Diagnostics,
                                           {F.Entry});
  return Pipeline().run(F.Image, Context, Options);
}
} // namespace

TEST(SwiftFixedRecordConstructor, CurrentEntryAndSourceReplayKeepFourResults) {
  FixedConstructorFixture F(Arch::AArch64);
  llvm::LLVMContext Context;
  auto R = constructorPipeline(F, Context);
  ASSERT_TRUE(R.Success);
  const auto Current =
      neverd::sdk::native_source_detail::currentFunction(R, F.Entry);
  ASSERT_TRUE(Current);
  ASSERT_TRUE(swiftFixedRecordConstructorEntryABI(F.Image, *Current->Low));
  ASSERT_EQ(Current->Med->SourceTypeHint->Parameters.size(), 5u);
  ASSERT_EQ(Current->Med->SourceTypeHint->ReturnComponents.size(), 4u);
  const auto B = neverd::sdk::bindObjCSourceReferences(*Current->High, F.Image);
  ASSERT_TRUE(B.Limitation.empty()) << B.Limitation;
  EXPECT_TRUE(
      neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
          .valid(B.Function));
}

TEST(SwiftFixedRecordConstructor, OriginalCallRequiresCurrentCompleteCallee) {
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FixedConstructorFixture F(Arch::AArch64);
    llvm::LLVMContext Context;
    auto R = constructorPipeline(F, Context, true);
    ASSERT_TRUE(R.Success);
    MedFunc *Caller = nullptr;
    LowFunc *Low = nullptr;
    for (auto &M : R.MedFuncs)
      if (M.Entry == 0x1100)
        Caller = &M;
    for (auto &L : R.LowFuncs)
      if (L.Entry == 0x1100)
        Low = &L;
    ASSERT_TRUE(Caller && Low);
    MedOp *Call = nullptr;
    for (auto &B : Caller->Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::CALL && O.Addr == 0x1108)
          Call = &O;
    ASSERT_TRUE(Call && Call->SourceCallHint);
    ASSERT_TRUE(Call->SourceCallHint->SwiftValueConstructor);
    EXPECT_TRUE(Call->SourceCallHint->requiresUniqueSourceOccurrence());
    auto Contracts = nativeSourceCalleeContracts(F.Image, R);
    ASSERT_TRUE(validateSwiftValueConstructorBindings(F.Image, Low, *Caller,
                                                      &Contracts));
    auto ChangedHint =
        std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = ChangedHint;
    switch (Mutation) {
    case 0:
      ChangedHint->SwiftValueConstructor.reset();
      break;
    case 1:
      Call->Output.Size = 8;
      break;
    case 2:
      Call->NumInputs = 3;
      break;
    case 3:
      Call->Inputs[1].Size = 4;
      break;
    case 4:
      Call->OriginSeq += 1;
      break;
    case 5:
      ChangedHint->Signature.ReturnComponents.pop_back();
      break;
    case 6:
      Contracts.CurrentCallees.erase(F.Entry);
      break;
    case 7:
      Contracts.CurrentCallees.at(F.Entry).Audit = nullptr;
      break;
    case 8:
      F.u32(F.Entry, 0xaa0103e0);
      break;
    case 9:
      F.Image.Symbols.back().Size = 4;
      F.u32(0x1108, 0x94000001);
      break;
    case 10:
      Call->PreservesCallerSaved = true;
      break;
    case 11:
      for (auto &L : R.LowFuncs)
        if (L.Entry == F.Entry)
          L.Blocks[0].Ops[0].Addr += 4;
      break;
    case 12:
      Caller->Blocks[0].Ops.push_back(*Call);
      break;
    case 13:
      ChangedHint->SwiftValueConstructor.reset();
      F.u32(F.Metadata + 16, 8);
      break;
    }
    EXPECT_FALSE(validateSwiftValueConstructorBindings(F.Image, Low, *Caller,
                                                       &Contracts));
  }
}

TEST(SwiftFixedRecordConstructor, PublicationRebuildsCurrentEntryAndWholeBody) {
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FixedConstructorFixture F(Arch::AArch64);
    llvm::LLVMContext Context;
    auto R = constructorPipeline(F, Context);
    const auto C =
        neverd::sdk::native_source_detail::currentFunction(R, F.Entry);
    ASSERT_TRUE(C);
    auto B = neverd::sdk::bindObjCSourceReferences(*C->High, F.Image);
    ASSERT_TRUE(
        neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
            .valid(B.Function));
    switch (Mutation) {
    case 0:
      B.Function.SourceTypeHint->ReturnComponents.pop_back();
      break;
    case 1:
      B.Function.Params.pop_back();
      break;
    case 2:
      B.Function.Params[0].Type = NdType::makeFloat(4);
      break;
    case 3:
      B.Function.Body.clear();
      break;
    case 4:
      B.Function.FrameSize += 16;
      break;
    case 5:
      B.Function.Body.push_back(B.Function.Body.back());
      break;
    case 6:
      R.MedFuncs[0].Blocks[0].Ops.clear();
      break;
    case 7:
      R.FunctionAudits[0].MedIRVerified = false;
      break;
    case 8:
      F.u32(F.Entry, 0xaa0103e0);
      break;
    case 9:
      F.u32(F.Metadata + 16, 8);
      break;
    case 10:
      B.Function.SourceTypeHint->Parameters[3].Location.RegisterOffset += 8;
      break;
    case 11:
      B.Function.Body.back().RetVal = HighExpr::makeConst(0, 8);
      break;
    }
    EXPECT_FALSE(
        neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
            .valid(B.Function));
  }
}

TEST(SwiftFixedRecordConstructor,
     X64DeclarationDoesNotBypassARM64MachineReplay) {
  FixedConstructorFixture F(Arch::X64);
  F.u32(F.Entry, 0xc3f88948); // mov rdi, rax; ret
  for (auto &S : F.Image.Symbols)
    if (S.Addr == F.Entry)
      S.Size = 4;
  auto D = F.declaration();
  ASSERT_TRUE(D);
  ASSERT_EQ(D->Signature.ReturnComponents.size(), 4u);
  EXPECT_EQ(D->Signature.ReturnComponents[3].RegisterOffset, x86reg::RAX);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {F.Entry};
  Options.SourceTypeHints.emplace(F.Entry, D->Signature);
  auto R = Pipeline().run(F.Image, Context, Options);
  const auto C = neverd::sdk::native_source_detail::currentFunction(R, F.Entry);
  ASSERT_TRUE(C);
  EXPECT_FALSE(swiftFixedRecordConstructorEntryABI(F.Image, *C->Low));
  const auto B = neverd::sdk::bindObjCSourceReferences(*C->High, F.Image);
  ASSERT_TRUE(B.Limitation.empty()) << B.Limitation;
  EXPECT_FALSE(
      neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
          .valid(B.Function));
}

TEST(SwiftFixedRecordConstructor,
     CallerPublicationReplaysArgumentsAndOccurrence) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FixedConstructorFixture F(Arch::AArch64);
    F.segment(0x1100, 0x100, "__text_caller", true);
    F.u32(F.Entry, 0xf9000020);
    F.u32(F.Entry + 4, 0xd65f03c0);
    for (auto &S : F.Image.Symbols)
      if (S.Addr == F.Entry)
        S.Size = 8;
    const uint32_t Words[] = {0xa9bf7bfd, 0x910003fd, 0xaa0003e1, 0xaa1f03e0,
                              0x9e6703e0, 0x9e6703e1, 0x9e6703e2, 0x97ffffd9,
                              0xaa1f03e0, 0xa8c17bfd, 0xd65f03c0};
    for (unsigned I = 0; I != std::size(Words); ++I)
      F.u32(0x1100 + I * 4, Words[I]);
    F.Image.Symbols.push_back({"caller", 0x1100, sizeof(Words), true});
    F.Image.RuntimeFunctionAddrs.insert(0x1100);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {F.Entry, 0x1100};
    PipelineResult R;
    for (unsigned Round = 0; Round != 4; ++Round) {
      R = Pipeline().run(F.Image, Context, Options);
      std::map<va_t, std::string> D;
      if (!neverd::sdk::inferObjCNativeDependencies(
              F.Image, R, Options, D, Options.OnlyFunctionEntries))
        break;
    }
    const auto C =
        neverd::sdk::native_source_detail::currentFunction(R, 0x1100);
    ASSERT_TRUE(C);
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &H : R.HighFuncs)
      Functions.emplace(H.Entry, &H);
    auto B = neverd::sdk::bindObjCSourceReferences(*C->High, F.Image, nullptr,
                                                   &Functions);
    ASSERT_TRUE(B.Limitation.empty()) << B.Limitation;
    ASSERT_TRUE(
        neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
            .valid(B.Function));
    HighExpr *Call = nullptr;
    walkStmts(B.Function.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        std::vector<ExprPtr> Pending{E};
        while (!Pending.empty()) {
          auto V = Pending.back();
          Pending.pop_back();
          if (!V)
            continue;
          if (V->SourceCallHint && V->SourceCallHint->SwiftValueConstructor)
            Call = V.get();
          V->forEachChildExpr([&](const ExprPtr &C) { Pending.push_back(C); });
        }
      });
    });
    ASSERT_TRUE(Call);
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call->SourceCallHint);
    Call->SourceCallHint = Hint;
    switch (Mutation) {
    case 0:
      Hint->SwiftValueConstructor.reset();
      break;
    case 1:
      Call->Operands[0] = HighExpr::makeConst(7, 8);
      break;
    case 2:
      Call->Operands[4] = HighExpr::makeConst(0, 8);
      break;
    case 3:
      Call->Operands.pop_back();
      break;
    case 4:
      Hint->Signature.ReturnComponents.pop_back();
      break;
    case 5:
      Call->Type = NdType::makeInt(8);
      break;
    case 6:
      Hint->SwiftValueConstructor->Site.Sequence += 1;
      break;
    case 8:
      Hint->SwiftValueConstructor.reset();
      F.u32(F.Metadata + 16, 8);
      for (auto &M : R.MedFuncs)
        for (auto &Block : M.Blocks)
          for (auto &O : Block.Ops)
            if (O.SourceCallHint && O.SourceCallHint->SwiftValueConstructor) {
              auto H = std::make_shared<SourceCallTypeHint>(*O.SourceCallHint);
              H->SwiftValueConstructor.reset();
              O.SourceCallHint = H;
            }
      break;
    case 7: {
      HighStmt Extra;
      Extra.Kind = StmtKind::Call;
      Extra.CallExpr = std::make_shared<HighExpr>(*Call);
      B.Function.Body.insert(B.Function.Body.begin(), Extra);
      break;
    }
    }
    EXPECT_FALSE(
        neverd::sdk::SourceSwiftValueConstructorProjectionValidator(F.Image, R)
            .valid(B.Function));
  }
}
