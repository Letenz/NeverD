#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

namespace {
SourceFunctionTypeHint signature(Arch Architecture, unsigned IntegerCount = 1) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(4);
  Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                     {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
  for (unsigned I = 0; I < IntegerCount; ++I)
    Hint.Parameters.push_back({"arg" + std::to_string(I), NdType::makeInt(4)});
  std::string Diagnostic;
  EXPECT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Diagnostic))
      << Diagnostic;
  return Hint;
}

BinaryImage image(Arch Architecture = Arch::AArch64) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Segment;
  Segment.VA = 0x1000;
  Segment.Size = Segment.FileSz = 0x2000;
  Segment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Segment.Data.resize(0x2000);
  Image.Segments.push_back(Segment);
  Section Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.push_back(Text);
  Section Data;
  Data.VA = 0x2000;
  Data.FileOff = 0x1000;
  Data.Size = Data.FileSz = 0x1000;
  Data.Flags = SegmentFlags::Readable;
  Image.Sections.push_back(Data);
  Image.ImportPtrSlots[0x2180] = "_objc_msgSend";
  ObjCSourceReference Reference;
  Reference.Address = 0x2100;
  Reference.Name = "scale:";
  Image.ObjCSourceReferences[0x2100] = Reference;
  ObjCMethod Method;
  Method.ClassName = "First";
  Method.Selector = Reference.Name;
  Method.Implementation = 0x1400;
  Method.TypeHint = signature(Architecture);
  Image.ObjCMethods.push_back(Method);
  // ADRP x1,0x2000; LDR x1,[x1,#0x100]; ADRP x16,0x2000;
  // LDR x16,[x16,#0x180]; BR x16. No symbol table is supplied.
  const uint32_t Stub[] = {0xb0000001, 0xf9408021, 0xb0000010, 0xf940c210,
                           0xd61f0200};
  for (size_t I = 0; I < 5; ++I)
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + 0x100 + I * 4, Stub[I]);
  return Image;
}

LowOp operation(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
                va_t Address = 0x1200) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Address;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

LowFunc caller(Arch Architecture = Arch::AArch64) {
  LowFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "caller";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x1200;
  Block.EndAddr = 0x1208;
  const auto &TRI = getTargetRegInfo(Architecture);
  Block.Ops.push_back(operation(NdOp::CALL, NdVar::reg(TRI.IntReturnReg, 8),
                                {NdVar::cst(0x1100, 8)}));
  Block.Ops.push_back(
      operation(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 4)}, 0x1204));
  Function.Blocks.push_back(Block);
  return Function;
}

MedFunc convert(const BinaryImage &Image, const LowFunc &Low,
                bool Enable = true) {
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  Converter.setSourceCallHintsEnabled(Enable);
  auto Result = Converter.convert(Low, Image.Arch, BinaryFormat::MachO);
  recoverCallAbi(Result, Image.Arch, {}, &Image);
  return Result;
}

const HighExpr *sourceCall(const HighFunc &High) {
  std::vector<const HighExpr *> Work;
  walkStmts(High.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Expression) {
      Work.push_back(Expression.get());
    });
  });
  const HighExpr *SourceCall = nullptr;
  while (!Work.empty()) {
    const auto *Expression = Work.back();
    Work.pop_back();
    if (Expression->SourceCallHint) {
      SourceCall = Expression;
      break;
    }
    for (const auto &Operand : Expression->Operands)
      if (Operand)
        Work.push_back(Operand.get());
  }
  return SourceCall;
}

TEST(ObjCCallHints, RecognizesStrippedSelectorStubFromInstructionsAndSlots) {
  auto Image = image();
  auto Hints = buildObjCSourceCallHints(Image, caller());
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Hint = Hints.at(0x1200);
  EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::ObjCMessage);
  EXPECT_EQ(Hint.TargetName, "objc_msgSend");
  EXPECT_EQ(Hint.Selector, "scale:");
  EXPECT_EQ(Hint.SelectorReferenceAddress, 0x2100U);
  ASSERT_EQ(Hint.Signature.Parameters.size(), 3U);
  EXPECT_EQ(Hint.Signature.Parameters[2].Location.RegisterOffset, 16U);
}

TEST(ObjCCallHints, FrameworkDeclarationsSupplyAbsentScalarCallSignatures) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs.push_back(
      "/System/Library/Frameworks/Foundation.framework/Foundation");
  for (llvm::StringRef Selector : {"objectForKeyedSubscript:", "copy", "length",
                                   "addObject:", "doubleValue"}) {
    SCOPED_TRACE(Selector.str());
    Image.ObjCSourceReferences.at(0x2100).Name = Selector.str();
    const auto Hints = buildObjCSourceCallHints(Image, caller());
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Hint = Hints.at(0x1200);
    EXPECT_EQ(Hint.Selector, Selector);
    EXPECT_EQ(Hint.Signature.Parameters.size(),
              Selector.contains(':') ? 3U : 2U);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Hint.Signature, Diagnostic)) << Diagnostic;
  }
  Image.ObjCSourceReferences.at(0x2100).Name = "length";
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    auto Invalid = Image;
    Invalid.DyldBindSlots[0x2180] = {"_objc_msgSend", 0};
    if (Mutation == 0)
      Invalid.DyldBindSlots[0x2180].Addend = 4;
    if (Mutation == 1)
      Invalid.DyldBindSlots[0x2180].WeakImport = true;
    if (Mutation == 2)
      Invalid.DyldBindSlots[0x2180].Name = "_other";
    if (Mutation == 3)
      Invalid.ImportStorageSlots[0x2180] = {"_objc_msgSend", 8};
    EXPECT_TRUE(buildObjCSourceCallHints(Invalid, caller()).empty())
        << Mutation;
  }
}

TEST(ObjCCallHints, FrameworkDeclarationsKeepMissingAndConflictingEvidence) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "length";
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.DynInfo.NeededLibs = {"/tmp/Foundation.framework/Foundation"};
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"};
  ASSERT_EQ(buildObjCSourceCallHints(Image, caller()).size(), 1U);
  ObjCMethod Conflict;
  Conflict.Selector = "length";
  Conflict.TypeHint = signature(Image.Arch, 0); // SDK result is 64 bits.
  Image.ObjCMethods.push_back(Conflict);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.ObjCMethods.clear();
  ObjCProtocol Protocol;
  ObjCProtocolMethod Unsupported;
  Unsupported.Selector = "length";
  Protocol.Methods.push_back(Unsupported);
  Image.ObjCProtocols.push_back(Protocol);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  // A declaration absent from the SDK's common platform scope cannot veto
  // the independently observed contract of an application's own selector.
  Image.ObjCProtocols.clear();
  Image.ObjCSourceReferences.at(0x2100).Name = "sharedInstance";
  Conflict.Selector = "sharedInstance";
  Image.ObjCMethods.push_back(Conflict);
  EXPECT_EQ(buildObjCSourceCallHints(Image, caller()).size(), 1U);
}

TEST(ObjCCallHints, FrameworkVariadicAndAggregateCallsRemainUnbound) {
  auto Image = image();
  Image.ObjCMethods.clear();
  Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  for (llvm::StringRef Selector :
       {"stringWithFormat:", "rangeOfString:", "neverdUnknownSelector:"}) {
    SCOPED_TRACE(Selector.str());
    Image.ObjCSourceReferences.at(0x2100).Name = Selector.str();
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  }
  // Runtime encodings omit an ellipsis. A matching fixed prefix cannot
  // override the compiler's explicit variadic declaration.
  Image.ObjCSourceReferences.at(0x2100).Name = "stringWithFormat:";
  ObjCMethod Prefix;
  Prefix.Selector = "stringWithFormat:";
  Prefix.TypeHint = signature(Image.Arch);
  Image.ObjCMethods.push_back(Prefix);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
}

TEST(ObjCCallHints, FrameworkABIIsArchitectureSpecificAndRevalidated) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.ObjCMethods.clear();
    Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    const auto Hint = objcSelectorSourceTypeHint(Image, "length");
    ASSERT_TRUE(Hint);
    EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::ObjCSDK);
    EXPECT_EQ(Hint->Architecture, Architecture);
    ASSERT_EQ(Hint->ReturnType->Size, 8U);
    EXPECT_FALSE(Hint->ReturnType->IsSigned);
    const auto Boolean = objcSelectorSourceTypeHint(Image, "isEqualToString:");
    if (Architecture == Arch::AArch64) {
      ASSERT_TRUE(Boolean);
      EXPECT_EQ(Boolean->ReturnType->Size, 1U);
      EXPECT_FALSE(Boolean->ReturnType->IsSigned);
      EXPECT_TRUE(Boolean->ReturnLocation.ExtendTo32Bits);
    } else {
      // macOS x86-64 uses signed char, while iOS uses bool. No platform
      // identity is asserted by this catalog's common Darwin ABI facts.
      EXPECT_FALSE(Boolean);
    }
    auto Call = HighExpr::makeCall("objc_msgSend", 0, {});
    SourceCallTypeHint Binding;
    Binding.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Binding.TargetName = "objc_msgSend";
    Binding.Selector = "length";
    Binding.Signature = *Hint;
    auto Evidence = std::make_shared<SourceCallTypeHint>(Binding);
    Call->SourceCallHint = Evidence;
    Call->Type = Hint->ReturnType;
    for (const auto &Parameter : Hint->Parameters)
      Call->Operands.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    Evidence->Signature.ReturnType = NdType::makeInt(8);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
    *Evidence = Binding;
    Image.DynInfo.NeededLibs.clear();
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
    Image.Format = BinaryFormat::ELF;
    EXPECT_FALSE(objcSelectorSourceTypeHint(Image, "length"));
  }
}

TEST(ObjCCallHints, BoundCallsPreserveOnlyABIProvenRuntimeReferenceRegisters) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      auto Image = image(Architecture);
      Image.ImportPtrSlots[0x2180] = "_objc_retain";
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto Register =
          Mutation == 1 ? TRI.IntParamRegs.front() : TRI.CalleeSaveRegs.front();
      LowFunc F;
      F.Entry = 0x1200;
      LowBlock B;
      B.StartAddr = 0x1200;
      B.Ops = {
          operation(NdOp::LOAD, NdVar::reg(Register, 8),
                    {NdVar::cst(0x2180, 8)}, 0x1200),
          operation(NdOp::INDIR_CALL, {}, {NdVar::reg(Register, 8)}, 0x1204)};
      if (Mutation == 2)
        B.Ops.push_back(operation(NdOp::COPY, NdVar::reg(Register, 4),
                                  {NdVar::cst(0, 4)}, 0x1208));
      if (Mutation == 3)
        B.Ops.push_back(
            operation(NdOp::CALL, {}, {NdVar::cst(0x1900, 8)}, 0x1208));
      B.Ops.push_back(
          operation(NdOp::INDIR_CALL, {}, {NdVar::reg(Register, 8)}, 0x120c));
      F.Blocks.push_back(std::move(B));
      const auto Hints = buildObjCSourceCallHints(Image, F);
      EXPECT_TRUE(Hints.count(0x1204));
      EXPECT_EQ(Hints.count(0x120c), Mutation == 0)
          << static_cast<int>(Architecture) << ':' << Mutation;
    }
  }
}

namespace {
BinaryImage runtimeImage(llvm::StringRef Name,
                         Arch Architecture = Arch::AArch64) {
  auto Image = image(Architecture);
  Image.ImportPtrSlots[0x2180] = Name.str();
  auto *Bytes = Image.Segments[0].Data.data() + 0x100;
  if (Architecture == Arch::AArch64) {
    const uint32_t Stub[] = {0xb0000010, 0xf940c210, 0xd61f0200};
    for (size_t I = 0; I < 3; ++I)
      llvm::support::endian::write32le(Bytes + I * 4, Stub[I]);
  } else {
    Bytes[0] = 0xff;
    Bytes[1] = 0x25;
    llvm::support::endian::write32le(Bytes + 2, 0x2180 - 0x1106);
  }
  return Image;
}
} // namespace

TEST(ObjCCallHints, RuntimeImportsBindArgumentsBeforeSSAOnBothDarwinTargets) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image =
        runtimeImage("_objc_retainAutoreleasedReturnValue", Architecture);
    auto Low = caller(Architecture);
    const auto Med = convert(Image, Low);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    ASSERT_TRUE(Call.SourceCallHint);
    EXPECT_EQ(Call.SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::ObjCRuntimeCall);
    EXPECT_EQ(Call.SourceCallHint->TargetAddress, 0x2180U);
    EXPECT_EQ(Call.TargetName, "objc_retainAutoreleasedReturnValue");
    ASSERT_EQ(Call.Args.size(), 1U);
    EXPECT_EQ(Call.Args[0].RegOff,
              getTargetRegInfo(Architecture).IntParamRegs.front());
    EXPECT_EQ(Call.Args[0].Size, 8U);
    const auto &Op = Med.Blocks[Call.BlockId].Ops[Call.OpIdx];
    EXPECT_EQ(Op.Output.RegOff, getTargetRegInfo(Architecture).IntReturnReg);
    EXPECT_EQ(Op.Output.Size, 8U);
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Architecture);
    const auto *Expression = sourceCall(High);
    ASSERT_NE(Expression, nullptr);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
  }
}

TEST(ObjCCallHints, SwiftCRuntimeCallsKeepExactCarriersAndImportIdentity) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (const auto &[Name, Count, ReturnsPointer] :
         {std::tuple{"swift_unknownObjectWeakLoadStrong", 1U, true},
          std::tuple{"swift_unknownObjectWeakAssign", 2U, true},
          std::tuple{"swift_unknownObjectWeakDestroy", 1U, false},
          std::tuple{"swift_getObjectType", 1U, true},
          std::tuple{"swift_bridgeObjectRetain", 1U, true},
          std::tuple{"swift_bridgeObjectRelease", 1U, false},
          std::tuple{"swift_beginAccess", 4U, false},
          std::tuple{"swift_endAccess", 1U, false}}) {
      SCOPED_TRACE(Name);
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      const auto &Hint = *Call.SourceCallHint;
      EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::SwiftRuntimeCall);
      EXPECT_EQ(Hint.TargetAddress, 0x2180U);
      EXPECT_EQ(Hint.TargetName, Name);
      EXPECT_EQ(Hint.Signature.Origin,
                SourceFunctionTypeHint::OriginKind::SwiftRuntime);
      EXPECT_EQ(Hint.Signature.ReturnType->Kind,
                ReturnsPointer ? NdTypeKind::Ptr : NdTypeKind::Void);
      ASSERT_EQ(Call.Args.size(), Count);
      for (size_t I = 0; I < Count; ++I) {
        EXPECT_EQ(Call.Args[I].RegOff,
                  getTargetRegInfo(Architecture).IntParamRegs[I]);
        EXPECT_EQ(Call.Args[I].Size, 8U);
        EXPECT_EQ(Hint.Signature.Parameters[I].Type->Kind,
                  std::string(Name) == "swift_beginAccess" && I == 2
                      ? NdTypeKind::Int
                      : NdTypeKind::Ptr);
      }
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      auto Changed = *Expression;
      auto WrongHint = std::make_shared<SourceCallTypeHint>(Hint);
      WrongHint->TargetName = "swift_release";
      Changed.SourceCallHint = std::move(WrongHint);
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {}));
      Image.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
      EXPECT_TRUE(
          buildObjCSourceCallHints(Image, caller(Architecture)).empty());
    }
  }
}

TEST(ObjCCallHints, SwiftOnceKeepsCallbackContextAndVoidResult) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("_swift_once", Architecture);
    const auto Med = convert(Image, caller(Architecture));
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    ASSERT_TRUE(Call.SourceCallHint);
    const auto &Hint = *Call.SourceCallHint;
    ASSERT_EQ(Hint.Signature.Parameters.size(), 3U);
    ASSERT_EQ(Call.Args.size(), 3U);
    EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
    EXPECT_EQ(Hint.Signature.ReturnLocation.Kind, SourceABICarrierKind::None);
    const auto Callback = Hint.Signature.Parameters[1].Type;
    ASSERT_EQ(Callback->Kind, NdTypeKind::Ptr);
    ASSERT_TRUE(Callback->Pointee);
    EXPECT_EQ(Callback->Pointee->Kind, NdTypeKind::Func);
    EXPECT_EQ(Callback->Pointee->RetType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Callback->Pointee->ParamTypes.size(), 1U);
    EXPECT_TRUE(equalSourceTypes(Callback->Pointee->ParamTypes[0],
                                 Hint.Signature.Parameters[2].Type));
    for (size_t I = 0; I < 3; ++I) {
      EXPECT_EQ(Call.Args[I].RegOff,
                getTargetRegInfo(Architecture).IntParamRegs[I]);
      EXPECT_EQ(Call.Args[I].Size, 8U);
    }
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Architecture);
    const auto *Expression = sourceCall(High);
    ASSERT_NE(Expression, nullptr);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    for (const auto &WrongType :
         {NdType::makePtr(NdType::makeVoid()),
          NdType::makePtr(NdType::makeFunc(NdType::makeInt(8))),
          NdType::makePtr(NdType::makeFunc(NdType::makeVoid()))}) {
      auto Changed = *Expression;
      auto Wrong = std::make_shared<SourceCallTypeHint>(Hint);
      Wrong->Signature.Parameters[1].Type = WrongType;
      Changed.SourceCallHint = Wrong;
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {}));
    }
    Image.ConflictingImportStorageSlots.insert(0x2180);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
  }
}

TEST(ObjCCallHints, DiagnosticRuntimeUsesCABIAndKeepsItsLinkerSpelling) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &[Name, Count] :
         {std::pair{"_swift_stdlib_reportUnimplementedInitializer", 5U},
          std::pair{"_swift_stdlib_reportFatalError", 5U},
          std::pair{"_swift_stdlib_reportUnimplementedInitializerInFile", 9U},
          std::pair{"_swift_stdlib_reportFatalErrorInFile", 8U}}) {
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
      ASSERT_TRUE(Hint);
      ASSERT_EQ(Hint->Signature.Parameters.size(), Count);
      EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Hint->BorrowedByteInputs.size(), Count == 5 ? 2U : 3U);
      EXPECT_EQ(Hint->Signature.Parameters[1].Type->Size, 4U);
      EXPECT_TRUE(Hint->Signature.Parameters[1].Type->IsSigned);
      EXPECT_EQ(Hint->Signature.Parameters.back().Type->Size, 4U);
      EXPECT_FALSE(Hint->Signature.Parameters.back().Type->IsSigned);
      std::string Error;
      EXPECT_TRUE(validateSourceABI(Hint->Signature, Error)) << Error;
      std::vector<ExprPtr> Args;
      for (const auto &Parameter : Hint->Signature.Parameters)
        Args.push_back(HighExpr::makeConst(0, Parameter.Type->Size));
      auto Call = HighExpr::makeCall("unrelated_veneer", 0x1100, Args);
      Call->Type = NdType::makeVoid();
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      HighStmt Statement;
      Statement.Kind = StmtKind::Call;
      Statement.CallExpr = Call;
      HighFunc Function;
      Function.Entry = 0x1200;
      Function.Name = "diagnostic_wrapper";
      Function.ReturnType = NdType::makeVoid();
      Function.Body = {Statement};
      std::string C;
      llvm::raw_string_ostream OS(C);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      Options.Format = BinaryFormat::MachO;
      ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
      EXPECT_NE(C.find("__asm__(\"_" + std::string(Name) + "\")"),
                std::string::npos)
          << C;
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      auto Forged = std::make_shared<SourceCallTypeHint>(*Hint);
      Forged->BorrowedByteInputs = {{1, 0}};
      Call->SourceCallHint = Forged;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
    }
  }
}

TEST(ObjCCallHints, ImmutableBytesRejectAliasingWritableAndRelocatedStorage) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    auto Image = runtimeImage("__swift_stdlib_reportUnimplementedInitializer");
    auto &Data = Image.Sections[1];
    // Linked Mach-O data inherits RX segment permissions; instruction
    // attributes, rather than those coarse permissions, identify code.
    Image.Segments[0].Flags =
        Image.Segments[0].Flags | SegmentFlags::Executable;
    Data.Flags = Data.Flags | SegmentFlags::Executable;
    ASSERT_TRUE(readImmutableImageBytes(Image, 0x2200, 5));
    if (Mutation == 0)
      Data.Flags = Data.Flags | SegmentFlags::Writable;
    if (Mutation == 1)
      Image.Segments[0].Flags =
          Image.Segments[0].Flags | SegmentFlags::Writable;
    if (Mutation == 2)
      Image.Sections.push_back(Data);
    if (Mutation == 3)
      Image.Segments.push_back(Image.Segments[0]);
    if (Mutation == 4)
      Image.DataPtrRelocSlots.insert(0x21ff);
    if (Mutation == 5)
      Image.CodePtrRelocSlots.insert(0x2204);
    if (Mutation == 6)
      Image.DyldBindSlots[0x2201] = {};
    if (Mutation == 7)
      Image.ConflictingImportStorageSlots.insert(0x21fa);
    if (Mutation == 8)
      Data.FileSz = 0x201;
    if (Mutation == 9)
      Image.MachOChainedFixupsAmbiguous = true;
    if (Mutation == 10)
      Data.Type |= llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    if (Mutation == 11)
      Image.ImportPtrSlots[0x2200] = "_other";
    EXPECT_FALSE(readImmutableImageBytes(Image, 0x2200, 5)) << Mutation;
  }
  const auto Image =
      runtimeImage("__swift_stdlib_reportUnimplementedInitializer");
  EXPECT_FALSE(readImmutableImageBytes(Image, UINT64_MAX - 2, 5));
  EXPECT_FALSE(readImmutableImageBytes(Image, 0x2200, 1024 * 1024 + 1));
}

TEST(ObjCCallHints, BorrowedBytesRequireTheBoundedConsumerOccurrence) {
  auto Image = runtimeImage("__swift_stdlib_reportUnimplementedInitializer");
  const uint8_t Bytes[] = {65, 0, 255, 34, 92};
  std::copy(std::begin(Bytes), std::end(Bytes),
            Image.Segments[0].Data.begin() + 0x1200);
  const auto Hint = swiftRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(Hint);
  auto Address = HighExpr::makeConst(0x2200, 8);
  auto Count = HighExpr::makeConst(5, 4);
  auto Call = HighExpr::makeCall(
      {}, 0x1100, {Address, Count, Address, Count, HighExpr::makeConst(0, 4)});
  Call->Type = NdType::makeVoid();
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = Call;
  HighFunc Function;
  Function.ReturnType = NdType::makeVoid();
  Function.Body = {Statement};
  auto Result = sdk::bindObjCSourceReferences(Function, Image);
  ASSERT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  ASSERT_EQ(Result.BorrowedBytes.size(), 1U);
  std::set<std::string> Helpers;
  auto Source =
      sdk::renderBorrowedByteHelpers(Image, Result.BorrowedBytes, Helpers);
  EXPECT_NE(Source.find("65, 0, 255, 34, 92, 0"), std::string::npos);
  EXPECT_EQ(Call->Operands[0], Address)
      << "Binding must not mutate the original DAG";
  auto BoundCall = Result.Function.Body[0].CallExpr;
  EXPECT_TRUE(sdk::objcSourceCallBound(*BoundCall, Image, {}));
  EXPECT_TRUE(sdk::objcSourceCallBound(*BoundCall->Operands[0], Image, {}));
  Count->ConstVal =
      UINT32_MAX; // Negative precision would make printf unbounded.
  EXPECT_FALSE(
      sdk::bindObjCSourceReferences(Function, Image).Limitation.empty());
  Count->ConstVal = 5;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = Address;
  Function.ReturnType = NdType::makePtr(NdType::makeVoid());
  Function.Body.push_back(Return);
  Result = sdk::bindObjCSourceReferences(Function, Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_EQ(Result.Function.Body.back().RetVal->Kind, ExprKind::Const);
}

TEST(ObjCCallHints, SwiftRuntimeRejectsSpecialConventionsAndUnprovenTargets) {
  for (const char *Name :
       {"_swift_retainDirect", "_swift_releaseDirect", "_swift_retain_x20",
        "_swift_getTypeByMangledName",
        "_swift_unknownObjectWeakLoadStrong_suffix", "swift_retain"}) {
    auto Image = runtimeImage(Name);
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty()) << Name;
  }
  auto Image = runtimeImage("_swift_retain");
  Image.ImportPtrSlots.clear();
  Image.Symbols.push_back({"_swift_retain", 0x1100});
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image = runtimeImage("_swift_retain");
  Image.Segments[0].Data[0x108] ^= 1;
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
}

TEST(ObjCCallHints,
     SwiftStringBridgePreservesSwiftConventionAndScalarCarriers) {
  const std::string Name =
      "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF";
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    auto Image = runtimeImage(Name, Architecture);
    const auto Med = convert(Image, caller(Architecture));
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    ASSERT_TRUE(Call.SourceCallHint);
    const auto &Hint = *Call.SourceCallHint;
    EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::SwiftStringBridge);
    EXPECT_EQ(Hint.Signature.Origin,
              SourceFunctionTypeHint::OriginKind::SwiftStringBridge);
    EXPECT_EQ(Hint.TargetName, Name);
    EXPECT_EQ(Hint.TargetAddress, 0x2180U);
    EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Ptr);
    ASSERT_EQ(Call.Args.size(), 2U);
    for (unsigned I = 0; I < 2; ++I) {
      EXPECT_EQ(Call.Args[I].RegOff,
                getTargetRegInfo(Architecture).IntParamRegs[I]);
      EXPECT_EQ(Call.Args[I].Size, 8U);
      EXPECT_EQ(Hint.Signature.Parameters[I].Type->Kind,
                I ? NdTypeKind::Ptr : NdTypeKind::Int);
    }
    EXPECT_FALSE(Hint.Signature.Parameters[0].Type->IsSigned);
    EXPECT_FALSE(swiftRuntimeSourceCallHint(Image, 0x2180));
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Architecture);
    const auto *Expression = sourceCall(High);
    ASSERT_NE(Expression, nullptr);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    std::string C;
    llvm::raw_string_ostream OS(C);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    EXPECT_NE(
        C.find("extern void *neverd_swift_string_to_nsstring(uint64_t, void *) "
               "__asm__(\"" +
               Name + "\") __attribute__((swiftcall));"),
        std::string::npos)
        << C;
    EXPECT_NE(C.find("neverd_swift_string_to_nsstring((uint64_t)"),
              std::string::npos)
        << C;
    EXPECT_EQ(C.find("extern int _sSS"), std::string::npos) << C;
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto Changed = *Expression;
      auto Wrong = std::make_shared<SourceCallTypeHint>(Hint);
      if (Mutation == 0)
        Wrong->CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
      else if (Mutation == 1)
        Wrong->Signature.Origin =
            SourceFunctionTypeHint::OriginKind::SwiftRuntime;
      else if (Mutation == 2)
        Wrong->Signature.Parameters[0].Type =
            NdType::makePtr(NdType::makeVoid());
      else if (Mutation == 3)
        Wrong->Signature.Parameters[1].Location.RegisterOffset =
            getTargetRegInfo(Architecture).IntParamRegs[2];
      else
        Wrong->TargetName += "_suffix";
      Changed.SourceCallHint = std::move(Wrong);
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {})) << Mutation;
    }
    Image.ConflictingImportStorageSlots.insert(0x2180);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
  }
}

TEST(ObjCCallHints, NSStringBridgePreservesBothResultWordsAndExactImport) {
  const std::string Name =
      "_$sSS10FoundationE36_"
      "unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ";
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    auto Image = runtimeImage(Name, Architecture);
    auto Low = caller(Architecture);
    Low.Blocks[0].Ops.insert(
        Low.Blocks[0].Ops.begin() + 1,
        operation(NdOp::COPY, NdVar::reg(TRI.IntReturnReg, 8),
                  {NdVar::reg(TRI.IntReturnRegs[1], 8)}, 0x1204));
    auto Med = convert(Image, Low);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    ASSERT_TRUE(Med.CallInfos[0].SourceCallHint);
    const auto &Hint = *Med.CallInfos[0].SourceCallHint;
    EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::SwiftStringFromNSString);
    ASSERT_EQ(Hint.Signature.ReturnComponents.size(), 2U);
    ASSERT_EQ(Hint.Signature.Parameters.size(), 1U);
    EXPECT_EQ(Hint.Signature.ReturnType->Size, 16U);
    EXPECT_EQ(Hint.Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
    const auto High = MedToHighConverter().convert(Med, Architecture);
    const auto *Expression = sourceCall(High);
    ASSERT_NE(Expression, nullptr);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    EXPECT_NE(Source.find("extern unsigned __int128 "
                          "neverd_nsstring_to_swift_string(void *) __asm__(\"" +
                          Name + "\") __attribute__((swiftcall));"),
              std::string::npos)
        << Source;
    EXPECT_EQ(Source.find("caller-saved register clobbered"), std::string::npos)
        << Source;
    EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      auto Changed = *Expression;
      auto Wrong = std::make_shared<SourceCallTypeHint>(Hint);
      if (Mutation == 0)
        Wrong->Signature.ReturnComponents.pop_back();
      else if (Mutation == 1)
        std::swap(Wrong->Signature.ReturnComponents[0],
                  Wrong->Signature.ReturnComponents[1]);
      else if (Mutation == 2)
        Wrong->CallKind = SourceCallTypeHint::Kind::SwiftStringBridge;
      else
        Wrong->TargetName += "_suffix";
      Changed.SourceCallHint = std::move(Wrong);
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {})) << Mutation;
    }
    Image.DyldBindSlots[0x2180].Name = Name;
    Image.DyldBindSlots[0x2180].WeakImport = true;
    EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
  }
}

TEST(ObjCCallHints, SwiftStringBridgeRejectsOtherABIsAndUnprovenImports) {
  const std::string Name =
      "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF";
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &Other :
         {Name.substr(1), Name + "_suffix",
          std::string("_$sSS10FoundationE36_"
                      "unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgF"),
          std::string("_swift_retainDirect")}) {
      const auto Image = runtimeImage(Other, Architecture);
      EXPECT_TRUE(buildObjCSourceCallHints(Image, caller(Architecture)).empty())
          << Other;
    }
    for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
      auto Image = runtimeImage(Name, Architecture);
      if (Mutation == 0) {
        Image.ImportPtrSlots.clear();
        Image.Symbols.push_back({Name, 0x1100});
      } else if (Mutation == 1)
        Image.IsRelocatable = true;
      else if (Mutation == 2)
        Image.Format = BinaryFormat::ELF;
      else if (Mutation == 3)
        Image.Bits = Bitness::Bits32;
      else if (Mutation == 4)
        Image.Arch = Arch::ARM;
      else if (Mutation == 5)
        Image.ConflictingImportStorageSlots.insert(0x2180);
      else if (Mutation == 6 || Mutation == 7) {
        auto &Binding = Image.ImportStorageSlots[0x2180];
        Binding.Name = Mutation == 6 ? "_different" : Name;
        Binding.Addend = Mutation == 7;
      } else {
        auto &Binding = Image.DyldBindSlots[0x2180];
        Binding.Name = Mutation == 8 ? "_different" : Name;
        Binding.Addend = Mutation == 9;
        Binding.WeakImport = Mutation == 10;
      }
      EXPECT_TRUE(buildObjCSourceCallHints(Image, caller(Architecture)).empty())
          << Mutation;
    }
  }
}

TEST(ObjCCallHints,
     DarwinLocksKeepPointerAndBooleanCarriersWithSDKDeclarations) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Name :
         {"os_unfair_lock_lock", "os_unfair_lock_unlock",
          "os_unfair_lock_trylock", "os_unfair_lock_assert_owner",
          "os_unfair_lock_assert_not_owner"}) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(static_cast<int>(Architecture));
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      const auto &Hint = *Call.SourceCallHint;
      EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
      EXPECT_EQ(Hint.TargetAddress, 0x2180U);
      EXPECT_EQ(Hint.TargetName, Name);
      EXPECT_EQ(Hint.Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinRuntime);
      const bool TryLock = std::string(Name) == "os_unfair_lock_trylock";
      EXPECT_EQ(Hint.Signature.ReturnType->Kind,
                TryLock ? NdTypeKind::Int : NdTypeKind::Void);
      ASSERT_EQ(Call.Args.size(), 1U);
      EXPECT_EQ(Call.Args[0].RegOff,
                getTargetRegInfo(Architecture).IntParamRegs.front());
      EXPECT_EQ(Call.Args[0].Size, 8U);
      EXPECT_EQ(Hint.Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
      const auto &Op = Med.Blocks[Call.BlockId].Ops[Call.OpIdx];
      EXPECT_EQ(Op.Output.Size, TryLock ? 1U : 0U);
      EXPECT_EQ(Hint.Signature.ReturnLocation.ExtendTo32Bits,
                TryLock && Architecture == Arch::AArch64);
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      ASSERT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      std::string C;
      llvm::raw_string_ostream OS(C);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      EXPECT_NE(C.find("#include <os/lock.h>"), std::string::npos) << C;
      EXPECT_NE(C.find(std::string(Name) + "("), std::string::npos) << C;
      EXPECT_EQ(C.find("extern void os_unfair_lock"), std::string::npos) << C;
      EXPECT_EQ(C.find("extern int os_unfair_lock"), std::string::npos) << C;
      auto Changed = *Expression;
      auto WrongHint = std::make_shared<SourceCallTypeHint>(Hint);
      if (TryLock && Architecture == Arch::AArch64) {
        WrongHint->Signature.ReturnLocation.ExtendTo32Bits = false;
        Changed.SourceCallHint = WrongHint;
        EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {}));
        WrongHint = std::make_shared<SourceCallTypeHint>(Hint);
      }
      WrongHint->TargetName = std::string(Name) + "_unproved";
      Changed.SourceCallHint = std::move(WrongHint);
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, Image, {}));
      Image.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
  }
}

TEST(ObjCCallHints, DarwinLocksRejectUnprovenImportsAndNonzeroAddends) {
  for (const char *Name :
       {"os_unfair_lock_lock", "_os_unfair_lock_lock_suffix",
        "_os_unfair_lock_lock_with_options", "_os_unfair_lock_lock_with_flags",
        "_OSSpinLockLock"}) {
    auto Image = runtimeImage(Name);
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty()) << Name;
  }
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    auto Image = runtimeImage("_os_unfair_lock_lock");
    if (Mutation == 0) {
      Image.ImportPtrSlots.clear();
      Image.Symbols.push_back({"_os_unfair_lock_lock", 0x1100});
    } else if (Mutation == 1)
      Image.Segments[0].Data[0x108] ^= 1;
    else if (Mutation == 2)
      Image.IsRelocatable = true;
    else if (Mutation == 3)
      Image.Format = BinaryFormat::ELF;
    else if (Mutation == 4)
      Image.Bits = Bitness::Bits32;
    else if (Mutation == 5)
      Image.Arch = Arch::ARM;
    else if (Mutation == 6 || Mutation == 7) {
      auto &Binding = Image.ImportStorageSlots[0x2180];
      Binding.Name = Mutation == 6 ? "_different" : "_os_unfair_lock_lock";
      Binding.Addend = Mutation == 7;
    } else {
      auto &Binding = Image.DyldBindSlots[0x2180];
      Binding.Name = "_os_unfair_lock_lock";
      Binding.Addend = Mutation == 8;
      Binding.WeakImport = Mutation == 9;
    }
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty()) << Mutation;
  }
}

TEST(ObjCCallHints,
     BlockRuntimeKeepsExactCNamesPointerCarriersAndOwnershipFlags) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (llvm::StringRef Name :
         {"_Block_copy", "_Block_release", "_Block_object_assign",
          "_Block_object_dispose"}) {
      SCOPED_TRACE(Name.str());
      SCOPED_TRACE(static_cast<int>(Architecture));
      auto Image = runtimeImage(("_" + Name).str(), Architecture);
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      const auto &Hint = *Call.SourceCallHint;
      EXPECT_EQ(Hint.TargetName, Name.str());
      EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
      EXPECT_FALSE(Hint.DoesNotReturn);
      const bool Assign = Name == "_Block_object_assign";
      const bool Flags = Assign || Name == "_Block_object_dispose";
      ASSERT_EQ(Hint.Signature.Parameters.size(), Assign  ? 3U
                                                  : Flags ? 2U
                                                          : 1U);
      EXPECT_EQ(Hint.Signature.ReturnType->Kind,
                Name == "_Block_copy" ? NdTypeKind::Ptr : NdTypeKind::Void);
      for (size_t I = 0; I < Hint.Signature.Parameters.size(); ++I) {
        const auto &P = Hint.Signature.Parameters[I];
        const bool IsFlags = Flags && I + 1 == Hint.Signature.Parameters.size();
        EXPECT_EQ(P.Type->Size, IsFlags ? 4U : 8U);
        EXPECT_EQ(P.Type->Kind, IsFlags ? NdTypeKind::Int : NdTypeKind::Ptr);
        EXPECT_EQ(P.Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[I]);
        if (IsFlags)
          EXPECT_TRUE(P.Type->IsSigned);
      }
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      std::string C;
      llvm::raw_string_ostream OS(C);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      EXPECT_NE(C.find("#include <Block.h>"), std::string::npos) << C;
      EXPECT_EQ(C.find("#include <os/lock.h>"), std::string::npos) << C;
      EXPECT_NE(C.find(Name.str() + "("), std::string::npos) << C;
      EXPECT_EQ(C.find("nd__Block_"), std::string::npos) << C;
      EXPECT_EQ(C.find("extern int Block_"), std::string::npos) << C;
      auto Wrong = *Expression;
      auto Binding = std::make_shared<SourceCallTypeHint>(Hint);
      Binding->TargetName = Name.drop_front().str();
      Wrong.SourceCallHint = Binding;
      EXPECT_FALSE(sdk::objcSourceCallBound(Wrong, Image, {}));
      Image.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
  }
}

TEST(ObjCCallHints, StackFailureRetainsItsTerminalRuntimeCall) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool ThroughSlot : {false, true}) {
      auto Image = runtimeImage("___stack_chk_fail", Architecture);
      auto Low = caller(Architecture);
      if (ThroughSlot) {
        Low.Blocks[0].Ops.front().Opcode = NdOp::INDIR_CALL;
        Low.Blocks[0].Ops.front().Inputs[0] = NdVar::cst(0x2180, 8);
      }
      const auto Med = convert(Image, Low);
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      ASSERT_TRUE(Med.CallInfos.front().SourceCallHint);
      const auto &Hint = *Med.CallInfos.front().SourceCallHint;
      EXPECT_EQ(Hint.TargetName, "__stack_chk_fail");
      EXPECT_TRUE(Hint.DoesNotReturn);
      EXPECT_TRUE(Hint.Signature.Parameters.empty());
      EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      auto High = Converter.convert(Med, Architecture);
      const auto *Call = sourceCall(High);
      ASSERT_NE(Call, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      EXPECT_NE(Source.find("__stack_chk_fail();"), std::string::npos)
          << Source;
      EXPECT_NE(Source.find("__attribute__((noreturn))"), std::string::npos);
      EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
      auto Forged = *Call;
      auto ChangedHint = std::make_shared<SourceCallTypeHint>(Hint);
      ChangedHint->DoesNotReturn = false;
      Forged.SourceCallHint = ChangedHint;
      EXPECT_FALSE(sdk::objcSourceCallBound(Forged, Image, {}));
      Image.DyldBindSlots[0x2180].Name = "___stack_chk_fail";
      Image.DyldBindSlots[0x2180].WeakImport = true;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
    }
  }
}

TEST(ObjCCallHints, StackGuardAddressRequiresExactRuntimeIdentity) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("___stack_chk_guard", Architecture);
    HighFunc Function;
    Function.Name = "guard_address";
    Function.ReturnType = NdType::makePtr(NdType::makeVoid());
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.Addr = 0x1200;
    Return.RetVal =
        HighExpr::makeLoad(HighExpr::makeConst(0x2180, 8), NdType::makeInt(8));
    Function.Body = {Return};
    auto Bound = sdk::bindObjCSourceReferences(Function, Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    const auto *Address = sourceCall(Bound.Function);
    ASSERT_NE(Address, nullptr);
    EXPECT_EQ(Address->SourceCallHint->TargetName, "__stack_chk_guard");
    EXPECT_TRUE(sdk::objcSourceCallBound(*Address, Image, {}));
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
    EXPECT_NE(Source.find("extern long __stack_chk_guard[8];"),
              std::string::npos)
        << Source;
    EXPECT_EQ(Source.find("0x2180"), std::string::npos);
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      auto Changed = Image;
      if (Mutation == 0)
        Changed.ImportPtrSlots[0x2180] = "___stack_chk_guard_suffix";
      else if (Mutation == 1)
        Changed.ImportPtrSlots.clear();
      else if (Mutation == 2)
        Changed.IsRelocatable = true;
      else if (Mutation == 3)
        Changed.ConflictingImportStorageSlots.insert(0x2180);
      else {
        auto &Binding = Changed.DyldBindSlots[0x2180];
        Binding.Name = Mutation == 4 ? "_different" : "___stack_chk_guard";
        Binding.Addend = Mutation == 5;
        Binding.WeakImport = Mutation == 6;
        if (Mutation == 7)
          Changed.Format = BinaryFormat::ELF;
      }
      EXPECT_FALSE(sdk::objcSourceCallBound(*Address, Changed, {}));
      EXPECT_FALSE(
          sdk::bindObjCSourceReferences(Function, Changed).Limitation.empty());
    }
    for (bool Ordered : {false, true}) {
      auto Copy = Function;
      Copy.Body.front().RetVal = HighExpr::makeLoad(
          HighExpr::makeConst(0x2180, 8), NdType::makeInt(Ordered ? 8 : 4),
          Ordered ? NdMemoryOrdering::Acquire : NdMemoryOrdering::None);
      EXPECT_FALSE(
          sdk::bindObjCSourceReferences(Copy, Image).Limitation.empty());
    }
  }
}

TEST(ObjCCallHints, AssociatedObjectImportsPreserveKeyValueAndPolicyCarriers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (const auto &[Name, Count] :
         {std::pair{"objc_getAssociatedObject", 2U},
          std::pair{"objc_setAssociatedObject", 4U},
          std::pair{"objc_removeAssociatedObjects", 1U}}) {
      SCOPED_TRACE(Name);
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      ASSERT_EQ(Call.Args.size(), Count);
      const auto &Hint = Call.SourceCallHint->Signature;
      const auto &TRI = getTargetRegInfo(Architecture);
      for (size_t I = 0; I < Count; ++I) {
        EXPECT_EQ(Call.Args[I].RegOff, TRI.IntParamRegs[I]);
        EXPECT_EQ(Call.Args[I].Size, 8U);
        EXPECT_EQ(Hint.Parameters[I].Type->Kind,
                  I == 3 ? NdTypeKind::Int : NdTypeKind::Ptr);
      }
      if (Count == 4)
        EXPECT_FALSE(Hint.Parameters[3].Type->IsSigned);
      EXPECT_EQ(Hint.ReturnType->Kind,
                Count == 2 ? NdTypeKind::Ptr : NdTypeKind::Void);
      const auto &Op = Med.Blocks[Call.BlockId].Ops[Call.OpIdx];
      EXPECT_EQ(Op.Output.Size, Count == 2 ? 8U : 0U);
      Image.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_FALSE(objcRuntimeSourceCallHint(Image, 0x2180));
    }
  }
}

TEST(ObjCCallHints, RegisterSpecificRuntimeCallsReadTheNamedRegister) {
  for (unsigned Register : {0, 1, 8, 15, 19, 20, 28}) {
    for (llvm::StringRef Operation : {"retain", "release"}) {
      const auto Name =
          "_objc_" + Operation.str() + "_x" + std::to_string(Register);
      auto Image = runtimeImage(Name);
      const auto Med = convert(Image, caller());
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      const auto &Call = Med.CallInfos.front();
      ASSERT_TRUE(Call.SourceCallHint);
      ASSERT_EQ(Call.Args.size(), 1U);
      EXPECT_EQ(Call.Args[0].RegOff, Register * 8U);
      EXPECT_EQ(Call.TargetName, "objc_" + Operation.str());
      const auto &Op = Med.Blocks[Call.BlockId].Ops[Call.OpIdx];
      EXPECT_EQ(Op.Output.Size, Operation == "retain" ? 8 : 0);
    }
  }
}

TEST(ObjCCallHints, RuntimeBindingsRequireExactImportedIdentityAndABI) {
  for (llvm::StringRef Name :
       {"_objc_retain_x16", "_objc_retain_x18", "_objc_retain_x29",
        "_objc_retain_x01", "_objc_release_x20_extra", "_objc_retainFake"}) {
    auto Image = runtimeImage(Name);
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty())
        << Name.str();
  }
  auto Image = runtimeImage("_objc_retain_x19", Arch::X64);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller(Arch::X64)).empty());
  Image = runtimeImage("_objc_retain");
  Image.ConflictingImportStorageSlots.insert(0x2180);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.ConflictingImportStorageSlots.clear();
  Image.ImportPtrSlots.clear();
  Symbol Symbol;
  Symbol.Name = "_objc_retain";
  Symbol.Addr = 0x1100;
  Symbol.IsFunc = true;
  Image.Symbols.push_back(Symbol);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
}

TEST(ObjCCallHints, RuntimeIndirectCallUsesTheLoadedImportSlot) {
  auto Image = runtimeImage("_objc_retain_x19");
  auto Low = caller();
  auto &Ops = Low.Blocks[0].Ops;
  Ops[0].Opcode = NdOp::INDIR_CALL;
  Ops[0].Inputs[0] = NdVar::reg(16 * 8, 8);
  Ops.insert(Ops.begin(), operation(NdOp::LOAD, NdVar::reg(16 * 8, 8),
                                    {NdVar::cst(0x2180, 8)}, 0x11fc));
  const auto Hints = buildObjCSourceCallHints(Image, Low);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.begin()->second.TargetAddress, 0x2180U);
  EXPECT_EQ(
      Hints.begin()->second.Signature.Parameters[0].Location.RegisterOffset,
      19 * 8U);
  Ops.insert(Ops.begin() + 1, operation(NdOp::COPY, NdVar::reg(16 * 8, 4),
                                        {NdVar::cst(0, 4)}, 0x11fe));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Low).empty());
}

TEST(ObjCCallHints, InlinedColdArgumentsKeepTheirBranchEntryAndRuntimeCall) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    auto Image = runtimeImage("_objc_release", Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto First = NdVar::reg(TRI.IntParamRegs[0], 8);
    const auto Second = NdVar::reg(TRI.IntParamRegs[1], 8);
    const auto Result = NdVar::reg(TRI.IntReturnReg, 8);
    LowFunc Low;
    Low.Entry = 0x1200;
    Low.Name = "cold_release";
    Low.Blocks.resize(3);
    auto &Entry = Low.Blocks[0];
    Entry.Id = 0;
    Entry.StartAddr = 0x1200;
    Entry.EndAddr = 0x1204;
    Entry.Succs = {1, 2};
    Entry.Ops = {
        operation(NdOp::COND_BR, {}, {NdVar::cst(0x1220, 8), First}, 0x1200)};
    auto &Join = Low.Blocks[1];
    Join.Id = 1;
    Join.StartAddr = 0x1204;
    Join.EndAddr = 0x120c;
    Join.Preds = {0, 2};
    Join.Ops = {operation(NdOp::COPY, Result, {NdVar::cst(42, 8)}, 0x1204),
                operation(NdOp::RETURN, {}, {Result}, 0x1208)};
    auto &Cold = Low.Blocks[2];
    Cold.Id = 2;
    Cold.StartAddr = 0x1220;
    Cold.EndAddr = 0x122c;
    Cold.Preds = {0};
    Cold.Succs = {1};
    Cold.Ops = {operation(NdOp::COPY, First, {Second}, 0x1220),
                operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1224),
                operation(NdOp::BRANCH, {}, {NdVar::cst(0x1204, 8)}, 0x1228)};
    const auto Med = convert(Image, Low);
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Image.Arch);
    const auto *Call = sourceCall(High);
    ASSERT_NE(Call, nullptr) << "The reachable release must survive DCE";
    EXPECT_EQ(Call->SourceCallHint->TargetName, "objc_release");
    std::set<va_t> Targets;
    std::map<va_t, unsigned> Entries;
    walkStmts(High.Body, [&](const HighStmt &Statement) {
      if (Statement.Kind == StmtKind::Goto)
        Targets.insert(Statement.GotoTarget);
      if (Statement.Addr)
        ++Entries[Statement.Addr];
    });
    for (va_t Target : Targets)
      EXPECT_EQ(Entries[Target], 1U)
          << "Missing or duplicated source branch entry";
  }
}

TEST(ObjCCallHints, RuntimeVoidAndWeakSignaturesDoNotInventResults) {
  auto Image = runtimeImage("_objc_storeStrong");
  const auto Hints = buildObjCSourceCallHints(Image, caller());
  ASSERT_EQ(Hints.size(), 1U);
  const auto &Signature = Hints.begin()->second.Signature;
  EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Signature.Parameters.size(), 2U);
  EXPECT_EQ(Signature.Parameters[0].Type->Pointee->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Signature.Parameters[1].Type->Pointee->Kind, NdTypeKind::Void);
  Image.ImportPtrSlots[0x2180] = "_objc_autoreleasePoolPush";
  const auto Pool = buildObjCSourceCallHints(Image, caller());
  ASSERT_EQ(Pool.size(), 1U);
  EXPECT_TRUE(Pool.begin()->second.Signature.Parameters.empty());
  EXPECT_EQ(Pool.begin()->second.Signature.ReturnType->Kind, NdTypeKind::Ptr);
}

TEST(ObjCCallHints, EnumerationMutationKeepsItsObjectAndReturningContinuation) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("_objc_enumerationMutation", Architecture);
    auto Low = caller(Architecture);
    auto &Block = Low.Blocks[0];
    Block.Ops.insert(
        Block.Ops.end() - 1,
        operation(NdOp::COPY,
                  NdVar::reg(getTargetRegInfo(Architecture).IntReturnReg, 4),
                  {NdVar::cst(73, 4)}, 0x1204));
    Block.Ops.back().Addr = 0x1208;
    Block.EndAddr = 0x120c;
    const auto Hints = buildObjCSourceCallHints(Image, Low);
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Signature = Hints.begin()->second.Signature;
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    ASSERT_EQ(Signature.Parameters.size(), 1U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    const auto Med = convert(Image, Low);
    ASSERT_EQ(Med.CallInfos.size(), 1U);
    const auto &Call = Med.CallInfos.front();
    EXPECT_EQ(Med.Blocks[Call.BlockId].Ops[Call.OpIdx].Output.Size, 0U);
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto High = Converter.convert(Med, Architecture);
    ASSERT_NE(sourceCall(High), nullptr);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    EXPECT_NE(Source.find("#include <objc/runtime.h>"), std::string::npos);
    EXPECT_NE(Source.find("objc_enumerationMutation("), std::string::npos);
    bool ReturnsMarker = false;
    walkStmts(High.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Return)
        return;
      std::vector<ExprPtr> Work{S.RetVal};
      while (!Work.empty()) {
        auto E = Work.back();
        Work.pop_back();
        if (!E)
          continue;
        ReturnsMarker |= E->Kind == ExprKind::Const && E->ConstVal == 73;
        Work.insert(Work.end(), E->Operands.begin(), E->Operands.end());
      }
    });
    EXPECT_TRUE(ReturnsMarker);
  }
}

TEST(ObjCCallHints,
     RuntimeImportsRejectContradictoryBindingsAndWeakIdentities) {
  for (const auto *Name : {"_objc_enumerationMutation", "_objc_retain"})
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      auto Image = runtimeImage(Name);
      if (Mutation < 2) {
        auto &Binding = Image.ImportStorageSlots[0x2180];
        Binding.Name = Mutation == 0 ? "_different" : Name;
        Binding.Addend = Mutation == 1;
      } else {
        auto &Binding = Image.DyldBindSlots[0x2180];
        Binding.Name = Mutation == 2 ? "_different" : Name;
        Binding.Addend = Mutation == 3;
        Binding.WeakImport = Mutation == 4;
      }
      EXPECT_FALSE(objcRuntimeSourceCallHint(Image, 0x2180))
          << Name << ':' << Mutation;
    }
}

TEST(ObjCCallHints, PropertyRuntimeKeepsValueBeforeSignedOffset) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = runtimeImage("_objc_setProperty_nonatomic_copy", Architecture);
    const auto Hints = buildObjCSourceCallHints(Image, caller(Architecture));
    ASSERT_EQ(Hints.size(), 1U);
    const auto &Signature = Hints.begin()->second.Signature;
    ASSERT_EQ(Signature.Parameters.size(), 4U);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
    EXPECT_EQ(Signature.Parameters[2].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[3].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.Parameters[3].Type->Size, 8U);
    EXPECT_TRUE(Signature.Parameters[3].Type->IsSigned);
    EXPECT_EQ(Signature.Parameters[3].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[3]);
  }
}

TEST(ObjCCallHints, RejectsAmbiguousAndUnsupportedSelectorSignatures) {
  auto Image = image();
  auto Other = Image.ObjCMethods[0];
  Other.ClassName = "Other";
  Other.TypeHint->ReturnType = NdType::makeInt(8);
  Image.ObjCMethods.push_back(Other);
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.ObjCMethods.back().TypeHint.reset();
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.ObjCMethods.back().TypeHint = Image.ObjCMethods[0].TypeHint;
  EXPECT_EQ(buildObjCSourceCallHints(Image, caller()).size(), 1U);
}

TEST(ObjCCallHints, RejectsSymbolOnlyWrongBranchAndWrongImport) {
  auto Image = image();
  Symbol S;
  S.Name = "_objc_msgSend$scale:";
  S.Addr = 0x1100;
  S.IsFunc = true;
  Image.Symbols.push_back(S);
  Image.ImportPtrSlots[0x2180] = "_unrelated_runtime";
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
  Image.ImportPtrSlots[0x2180] = "_objc_msgSend";
  llvm::support::endian::write32le(Image.Segments[0].Data.data() + 0x110,
                                   0xd61f0220); // BR x17, not loaded x16
  EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
}

TEST(ObjCCallHints, PreservesReceiverArgumentAndReturnBeforeSSA) {
  auto Image = image();
  Image.Sections[0].Name = "__objc_stubs";
  auto Low = caller();
  auto &Ops = Low.Blocks[0].Ops;
  Ops.insert(Ops.begin(), operation(NdOp::COPY, NdVar::reg(16, 4),
                                    {NdVar::cst(0x80000001, 4)}, 0x11fc));
  const auto Med = convert(Image, Low);
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  const auto &Call = Med.CallInfos[0];
  ASSERT_TRUE(Call.SourceCallHint);
  ASSERT_EQ(Call.Args.size(), 3U);
  EXPECT_EQ(Call.Args[0].Kind, MedVar::Reg);
  EXPECT_EQ(Call.Args[0].RegOff, 0U);
  EXPECT_EQ(Call.Args[2].Kind, MedVar::Reg);
  EXPECT_EQ(Call.Args[2].RegOff, 16U);
  EXPECT_EQ(Call.Args[2].Size, 4U);
  const auto &Op = Med.Blocks[Call.BlockId].Ops[Call.OpIdx];
  EXPECT_EQ(Op.Output.RegOff, 0U);
  EXPECT_EQ(Op.Output.Size, 4U);
  EXPECT_EQ(Med.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
  MedToHighConverter HighConverter;
  HighConverter.setBinaryImage(&Image);
  const auto High = HighConverter.convert(Med, Image.Arch);
  const auto *SourceCall = sourceCall(High);
  ASSERT_NE(SourceCall, nullptr);
  ASSERT_EQ(SourceCall->Operands.size(), 3U);
  EXPECT_EQ(SourceCall->Operands[0]->Kind, ExprKind::Var);
  EXPECT_EQ(SourceCall->Operands[0]->Var.Kind, MedVar::Param);
  EXPECT_EQ(SourceCall->Operands[0]->Var.RegOff, 0U);
  EXPECT_FALSE(SourceCall->Operands[1]->Kind == ExprKind::Const &&
               SourceCall->Operands[1]->ConstVal == 0);
  EXPECT_EQ(SourceCall->Operands[2]->Kind, ExprKind::Const);
  EXPECT_EQ(SourceCall->Operands[2]->ConstVal, 0x80000001U);
  const auto Disabled = convert(Image, Low, false);
  ASSERT_EQ(Disabled.CallInfos.size(), 1U);
  EXPECT_FALSE(Disabled.CallInfos[0].SourceCallHint);
}

BinaryImage selectorStubImage() {
  auto Image = image();
  Image.Sections[0].Name = "__objc_stubs";
  return Image;
}

MedFunc callerWithStaleCommand(bool Clobbered) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  auto Register = [&](int Id, int Version, unsigned Index) {
    MedVar Value;
    Value.Kind = MedVar::Reg;
    Value.Id = Id;
    Value.SSAVer = Version;
    Value.Size = 8;
    Value.RegOff = TRI.IntParamRegs[Index];
    Value.TheArch = Arch::AArch64;
    return Value;
  };
  MedFunc Function;
  Function.Entry = 0x1200;
  Function.Name = "stale_selector_caller";
  Function.Blocks.resize(1);
  auto &Block = Function.Blocks[0];
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  auto Append = [&](NdOp Opcode, MedVar Output, MedVar Input,
                    unsigned CallSite = 0) {
    MedOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = 0x1200 + Block.Ops.size() * 4;
    Op.CallSiteId = CallSite;
    Op.addInput(Input);
    Block.Ops.push_back(Op);
  };
  MedVar Command = MedVar::makeConst(0x777777, 8);
  if (Clobbered) {
    Append(NdOp::CALL, Register(10, 1, 0), MedVar::makeConst(0x1500, 8), 1);
    Command = Register(20, 1, 1);
    Function.CallClobbers.push_back({Command, 1});
  }
  Append(NdOp::COPY, Register(11, 2, 0), MedVar::makeConst(0x2222, 8));
  Append(NdOp::COPY, Register(21, 2, 1), Command);
  Append(NdOp::COPY, Register(30, 1, 2), MedVar::makeConst(0x123456, 8));
  Append(NdOp::CALL, Register(12, 3, 0), MedVar::makeConst(0x1100, 8), 2);
  Append(NdOp::RETURN, {}, Register(12, 3, 0));
  Block.EndAddr = Block.Ops.back().Addr + 4;
  return Function;
}

void expectNativeSelectorCommand(const MedFunc &Function,
                                 const BinaryImage *Image, uint64_t Command) {
  MedToHighConverter Converter;
  Converter.setBinaryImage(Image);
  const auto High = Converter.convert(Function, Arch::AArch64);
  std::vector<const HighExpr *> Work;
  walkStmts(High.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Expression) {
      Work.push_back(Expression.get());
    });
  });
  std::vector<const HighExpr *> Calls;
  while (!Work.empty()) {
    const auto *Expression = Work.back();
    Work.pop_back();
    if (Expression->Kind == ExprKind::Call && Expression->CallAddr == 0x1100)
      Calls.push_back(Expression);
    for (const auto &Operand : Expression->Operands)
      if (Operand)
        Work.push_back(Operand.get());
  }
  ASSERT_EQ(Calls.size(), 1U);
  const auto &Call = *Calls.front();
  EXPECT_FALSE(Call.SourceCallHint);
  EXPECT_FALSE(Call.IsIndirectCall);
  ASSERT_EQ(Call.Operands.size(), 3U);
  const uint64_t Expected[] = {0x2222, Command, 0x123456};
  for (size_t I = 0; I < 3; ++I) {
    ASSERT_NE(Call.Operands[I], nullptr);
    ASSERT_EQ(Call.Operands[I]->Kind, ExprKind::Const);
    EXPECT_EQ(Call.Operands[I]->ConstVal, Expected[I]);
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.EmitComments = false;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  const std::string ExpectedCall = Command == 0
                                       ? "sub_1100(0x2222, 0, 0x123456)"
                                       : "sub_1100(0x2222, 0x777777, 0x123456)";
  EXPECT_NE(Source.find(ExpectedCall), std::string::npos) << Source;
}

TEST(ObjCCallHints, SelectorStubCommandProofDoesNotRequireMethodSignature) {
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Image = selectorStubImage();
    if (Mode == 0)
      Image.ObjCMethods.clear();
    else if (Mode == 1)
      Image.ObjCMethods[0].TypeHint.reset();
    else {
      auto Conflicting = Image.ObjCMethods[0];
      Conflicting.TypeHint->ReturnType = NdType::makeInt(8);
      Image.ObjCMethods.push_back(std::move(Conflicting));
    }
    EXPECT_TRUE(objcSelectorStubOverwritesCommand(Image, 0x1100));
    EXPECT_TRUE(buildObjCSourceCallHints(Image, caller()).empty());
    expectNativeSelectorCommand(callerWithStaleCommand(false), &Image, 0);
  }
}

TEST(ObjCCallHints, SelectorStubCommandProofRejectsUnverifiedCodeAndSlots) {
  for (unsigned Mutation = 0; Mutation < 16; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = selectorStubImage();
    switch (Mutation) {
    case 0:
      Image.Format = BinaryFormat::ELF;
      break;
    case 1:
      Image.IsRelocatable = true;
      break;
    case 2:
      Image.Arch = Arch::X64;
      break;
    case 3:
      Image.Bits = Bitness::Bits32;
      break;
    case 4:
      Image.Sections[0].Name = "__text";
      break;
    case 5:
      Image.Sections[0].Flags = SegmentFlags::Readable;
      break;
    case 6:
      Image.Segments[0].Flags = SegmentFlags::Readable;
      break;
    case 7:
      Image.Sections[0].FileSz = 0x110;
      break;
    case 8:
      Image.Segments[0].FileSz = 0x110;
      break;
    case 9:
      Image.ObjCSourceReferences.clear();
      break;
    case 10:
      Image.ObjCSourceReferences[0x2100].TheKind =
          ObjCSourceReference::Kind::Class;
      break;
    case 11:
      Image.ObjCSourceReferences[0x2100].Size = 4;
      break;
    case 12:
      Image.ObjCSourceReferences[0x2100].Name.clear();
      break;
    case 13:
      Image.ImportPtrSlots[0x2180] = "_unrelated_runtime";
      break;
    case 14:
      llvm::support::endian::write32le(Image.Segments[0].Data.data() + 0x104,
                                       0xf9408000); // LDR x0
      break;
    case 15:
      llvm::support::endian::write32le(Image.Segments[0].Data.data() + 0x110,
                                       0xd61f0220); // BR x17
      break;
    }
    EXPECT_FALSE(objcSelectorStubOverwritesCommand(Image, 0x1100));
    expectNativeSelectorCommand(callerWithStaleCommand(false), &Image,
                                0x777777);
  }
}

TEST(ObjCCallHints, VerifiedSelectorStubDiscardsStaleCallerCommand) {
  for (bool Clobbered : {false, true}) {
    SCOPED_TRACE(Clobbered);
    auto Image = selectorStubImage();
    Image.ObjCMethods.clear();
    auto Function = callerWithStaleCommand(Clobbered);
    // The ordinary source pipeline does not run recoverCallAbi. Exercise
    // that route before also checking the independently recovered ABI record.
    expectNativeSelectorCommand(Function, &Image, 0);
    if (!Clobbered)
      expectNativeSelectorCommand(Function, nullptr, 0x777777);
    ASSERT_TRUE(verifyMedFunc(Function, "before-selector-stub-abi"));
    const auto ClobberCount = Function.CallClobbers.size();
    recoverCallAbi(Function, Arch::AArch64, {}, &Image);
    ASSERT_EQ(Function.CallInfos.size(), Clobbered ? 2U : 1U);
    const auto &Call = Function.CallInfos.back();
    EXPECT_EQ(Call.TargetAddr, 0x1100U);
    EXPECT_FALSE(Call.SourceCallHint);
    ASSERT_EQ(Call.Args.size(), 3U);
    ASSERT_TRUE(Call.Args[0].isConst());
    EXPECT_EQ(Call.Args[0].ConstVal, 0x2222U);
    ASSERT_TRUE(Call.Args[1].isConst());
    EXPECT_EQ(Call.Args[1].ConstVal, 0U);
    EXPECT_EQ(Call.Args[1].Size, 8U);
    ASSERT_TRUE(Call.Args[2].isConst());
    EXPECT_EQ(Call.Args[2].ConstVal, 0x123456U);
    EXPECT_EQ(Function.CallClobbers.size(), ClobberCount);
    EXPECT_TRUE(verifyMedFunc(Function, "after-selector-stub-abi"));
    expectNativeSelectorCommand(Function, &Image, 0);
  }
}

TEST(ObjCCallHints, UnverifiedSelectorStubKeepsObservedCallerCommand) {
  for (bool NamedSection : {false, true}) {
    SCOPED_TRACE(NamedSection);
    auto Image = selectorStubImage();
    if (NamedSection)
      Image.ImportPtrSlots[0x2180] = "_unrelated_runtime";
    else
      Image.Sections[0].Name = "__text";
    auto Function = callerWithStaleCommand(false);
    recoverCallAbi(Function, Arch::AArch64, {}, &Image);
    ASSERT_EQ(Function.CallInfos.size(), 1U);
    const auto &Call = Function.CallInfos.front();
    EXPECT_FALSE(Call.SourceCallHint);
    ASSERT_EQ(Call.Args.size(), 3U);
    ASSERT_TRUE(Call.Args[1].isConst());
    EXPECT_EQ(Call.Args[1].ConstVal, 0x777777U);
    EXPECT_TRUE(verifyMedFunc(Function, "unverified-selector-stub-abi"));
    expectNativeSelectorCommand(Function, &Image, 0x777777);
  }
}

TEST(ObjCCallHints, ResolvesX64CallsiteSelectorAndRejectsStaleAlias) {
  auto Image = image(Arch::X64);
  auto Low = caller(Arch::X64);
  auto &Ops = Low.Blocks[0].Ops;
  Ops[0].Opcode = NdOp::INDIR_CALL;
  Ops[0].Inputs[0] = NdVar::cst(0x2180, 8);
  const auto SelectorReg = getTargetRegInfo(Arch::X64).IntParamRegs[1];
  Ops.insert(Ops.begin(), operation(NdOp::LOAD, NdVar::reg(SelectorReg, 8),
                                    {NdVar::cst(0x2100, 8)}, 0x11f0));
  EXPECT_EQ(buildObjCSourceCallHints(Image, Low).size(), 1U);
  Ops.insert(Ops.begin() + 1, operation(NdOp::COPY, NdVar::reg(SelectorReg, 1),
                                        {NdVar::cst(7, 1)}, 0x11f8));
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Low).empty());
}

TEST(ObjCCallHints, KeepsFullRegisterArgumentListBeyondFormerInputCapacity) {
  auto Image = image();
  Image.ObjCMethods[0].TypeHint = signature(Arch::AArch64, 6);
  Image.ObjCMethods[0].Selector = "a:b:c:d:e:f:";
  Image.ObjCSourceReferences[0x2100].Name = Image.ObjCMethods[0].Selector;
  auto Low = caller();
  const auto Med = convert(Image, Low);
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  const auto &Call = Med.CallInfos[0];
  ASSERT_TRUE(Call.SourceCallHint);
  ASSERT_EQ(Call.Args.size(), 8U);
  for (size_t I = 2; I < 8; ++I) {
    EXPECT_EQ(Call.Args[I].Kind, MedVar::Reg) << I;
    EXPECT_EQ(Call.Args[I].RegOff, I * 8) << I;
  }
  const auto High = MedToHighConverter().convert(Med, Image.Arch);
  const auto *Expression = sourceCall(High);
  ASSERT_NE(Expression, nullptr);
  ASSERT_EQ(Expression->Operands.size(), 8U);
  for (size_t I = 2; I < 8; ++I) {
    ASSERT_TRUE(Expression->Operands[I]);
    EXPECT_EQ(Expression->Operands[I]->Kind, ExprKind::Var) << I;
    EXPECT_EQ(Expression->Operands[I]->Var.Kind, MedVar::Param) << I;
    EXPECT_EQ(Expression->Operands[I]->Var.RegOff, I * 8) << I;
  }
}

TEST(ObjCCallHints, NativeHintsPreserveIndependentFloatingRegisterBank) {
  auto Image = image();
  auto Hint = signature(Arch::AArch64, 0);
  Hint.ReturnType = NdType::makeFloat(8);
  Hint.Parameters.push_back({"floating", NdType::makeFloat(8)});
  Hint.Parameters.push_back({"integer", NdType::makeInt(4)});
  std::string Diagnostic;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Diagnostic));
  std::map<va_t, SourceFunctionTypeHint> Hints{{0x1600, Hint}};
  auto Low = caller();
  Low.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(0x1600, 8);
  Low.Blocks[0].Ops.back().Inputs[0] =
      NdVar::reg(getTargetRegInfo(Arch::AArch64).FPReturnReg, 8);
  LowToMedConverter Converter;
  Converter.setSourceCalleeTypeHints(&Hints);
  Converter.setSourceCallHintsEnabled(true);
  auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  recoverCallAbi(Med, Arch::AArch64, {});
  ASSERT_EQ(Med.CallInfos.size(), 1U);
  const auto &Call = Med.CallInfos[0];
  ASSERT_TRUE(Call.SourceCallHint);
  EXPECT_EQ(Call.SourceCallHint->CallKind, SourceCallTypeHint::Kind::Native);
  ASSERT_EQ(Call.Args.size(), 4U);
  EXPECT_EQ(Call.Args[2].RegOff,
            getTargetRegInfo(Arch::AArch64).FPParamRegs[0]);
  EXPECT_EQ(Call.Args[3].RegOff,
            getTargetRegInfo(Arch::AArch64).IntParamRegs[2]);
  EXPECT_EQ(Med.Blocks[Call.BlockId].Ops[Call.OpIdx].Output.RegOff,
            getTargetRegInfo(Arch::AArch64).FPReturnReg);
}

TEST(ObjCCallHints, SuperDispatchUsesVerifiedRuntimeVeneerAndLoadedSelector) {
  auto Image = image();
  Image.ImportPtrSlots[0x2180] = "_objc_msgSendSuper2";
  auto Low = caller();
  Low.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(0x1108, 8);
  Low.Blocks[0].Ops.insert(
      Low.Blocks[0].Ops.begin(),
      operation(NdOp::LOAD, NdVar::reg(8, 8), {NdVar::cst(0x2100, 8)}, 0x11fc));
  auto Hints = buildObjCSourceCallHints(Image, Low);
  ASSERT_EQ(Hints.size(), 1U);
  EXPECT_EQ(Hints.at(0x1200).CallKind, SourceCallTypeHint::Kind::ObjCSuper2);
  EXPECT_EQ(Hints.at(0x1200).SelectorReferenceAddress, 0U);
  Low.Blocks[0].Ops[1].Inputs[0] = NdVar::cst(0x2180, 8);
  // The address of a bound pointer slot is not the function stored in it.
  EXPECT_TRUE(buildObjCSourceCallHints(Image, Low).empty());
}

TEST(ObjCCallHints, ExplicitNativeStackHintsRequireKnownX64CallInstruction) {
  auto Image = image(Arch::X64);
  auto Hint = signature(Arch::X64, 5); // seven values, one stack argument
  ASSERT_EQ(Hint.Parameters.back().Location.Kind, SourceABICarrierKind::Stack);
  std::map<va_t, SourceFunctionTypeHint> Hints{{0x1600, Hint}};
  auto Low = caller(Arch::X64);
  Low.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(0x1600, 8);
  auto Recover = [&] {
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    Converter.setSourceCalleeTypeHints(&Hints);
    Converter.setSourceCallHintsEnabled(true);
    auto Med = Converter.convert(Low, Arch::X64, BinaryFormat::MachO);
    recoverCallAbi(Med, Arch::X64, {});
    return Med;
  };
  auto Unknown = Recover();
  ASSERT_EQ(Unknown.CallInfos.size(), 1U);
  EXPECT_FALSE(Unknown.CallInfos[0].SourceCallHint);
  Image.Segments[0].Data[0x200] =
      0xe8; // actual near CALL pushes return address
  auto Bound = Recover();
  ASSERT_EQ(Bound.CallInfos.size(), 1U);
  ASSERT_TRUE(Bound.CallInfos[0].SourceCallHint);
  EXPECT_EQ(Bound.CallInfos[0].Args.size(), 7U);
  Image.IsRelocatable = true;
  auto Relocatable = Recover();
  ASSERT_EQ(Relocatable.CallInfos.size(), 1U);
  EXPECT_FALSE(Relocatable.CallInfos[0].SourceCallHint);
}

TEST(ObjCCallHints,
     OperandStorageRetainsSixtyFourArgumentsWithoutSilentTruncation) {
  MedOp Op;
  Op.Opcode = NdOp::CALL;
  Op.addInput(MedVar::makeConst(0x1400, 8));
  for (unsigned I = 0; I < 64; ++I)
    Op.addInput(MedVar::makeConst(I + 100, 8));
  ASSERT_EQ(Op.NumInputs, 65U);
  auto Copy = Op;
  ASSERT_EQ(Copy.Inputs.size(), 65U);
  for (unsigned I = 0; I < 64; ++I)
    EXPECT_EQ(Copy.Inputs[I + 1].ConstVal, I + 100);
}
} // namespace

TEST(ObjCCallHints, SDKCDeclarationsPreservePointerIntegerAndFloatCarriers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto &[Name, Module, Count, ResultKind, ResultBytes] :
         {std::tuple{
              "NSStringFromClass",
              "/System/Library/Frameworks/Foundation.framework/Foundation", 1U,
              NdTypeKind::Ptr, 8U},
          std::tuple{"CFStringCompare",
                     "/System/Library/Frameworks/CoreFoundation.framework/"
                     "CoreFoundation",
                     3U, NdTypeKind::Int, 8U},
          std::tuple{"pthread_mutex_lock", "/usr/lib/libSystem.B.dylib", 1U,
                     NdTypeKind::Int, 4U},
          std::tuple{"dispatch_time", "/usr/lib/system/libdispatch.dylib", 2U,
                     NdTypeKind::Int, 8U},
          std::tuple{"fmod", "/usr/lib/libSystem.B.dylib", 2U,
                     NdTypeKind::Float, 8U},
          std::tuple{"__error", "/usr/lib/libSystem.B.dylib", 0U,
                     NdTypeKind::Ptr, 8U}}) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(static_cast<int>(Architecture));
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      Image.DyldBindSlots[0x2180] = {"_" + std::string(Name), 0, Module, false};
      const auto Med = convert(Image, caller(Architecture));
      ASSERT_EQ(Med.CallInfos.size(), 1U);
      ASSERT_TRUE(Med.CallInfos.front().SourceCallHint);
      const auto &Hint = *Med.CallInfos.front().SourceCallHint;
      EXPECT_EQ(Hint.Signature.Origin,
                SourceFunctionTypeHint::OriginKind::DarwinSDK);
      EXPECT_EQ(Hint.TargetName, Name);
      EXPECT_EQ(Hint.Signature.Parameters.size(), Count);
      EXPECT_EQ(Hint.Signature.ReturnType->Kind, ResultKind);
      EXPECT_EQ(Hint.Signature.ReturnType->Size, ResultBytes);
      if (ResultKind == NdTypeKind::Float) {
        EXPECT_EQ(Hint.Signature.ReturnLocation.Kind,
                  SourceABICarrierKind::FloatingRegister);
        for (const auto &Parameter : Hint.Signature.Parameters)
          EXPECT_EQ(Parameter.Location.Kind,
                    SourceABICarrierKind::FloatingRegister);
      }
      MedToHighConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto High = Converter.convert(Med, Architecture);
      const auto *Expression = sourceCall(High);
      ASSERT_NE(Expression, nullptr);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      std::string C;
      llvm::raw_string_ostream OS(C);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      EXPECT_NE(C.find("neverd_darwin_" + std::string(Name) + "("),
                std::string::npos)
          << C;
      EXPECT_NE(C.find("__asm__(\"_" + std::string(Name) + "\")"),
                std::string::npos)
          << C;
      EXPECT_EQ(C.find("#include <os/lock.h>"), std::string::npos) << C;
      Image.DyldBindSlots[0x2180].Module = "/tmp/impostor.dylib";
      EXPECT_FALSE(sdk::objcSourceCallBound(*Expression, Image, {}));
    }
  }
}

TEST(ObjCCallHints, SDKCDeclarationsRequireExactExportsAndFixedPrototypes) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      auto Image = runtimeImage("_NSStringFromClass", Architecture);
      const std::string Module = "/System/Library/Frameworks/"
                                 "Foundation.framework/Versions/C/Foundation";
      Image.DyldBindSlots[0x2180] = {"_NSStringFromClass", 0, Module, false};
      if (Mutation == 1)
        Image.DyldBindSlots.clear();
      if (Mutation == 2)
        Image.DyldBindSlots[0x2180].Module.clear();
      if (Mutation == 3)
        Image.DyldBindSlots[0x2180].Module = "/usr/lib/libSystem.B.dylib";
      if (Mutation == 4)
        Image.DyldBindSlots[0x2180].Module =
            "/tmp/Foundation.framework/Foundation";
      if (Mutation == 5)
        Image.DyldBindSlots[0x2180].WeakImport = true;
      if (Mutation == 6)
        Image.DyldBindSlots[0x2180].Addend = 4;
      if (Mutation == 7)
        Image.ConflictingImportStorageSlots.insert(0x2180);
      EXPECT_EQ(bool(darwinRuntimeSourceCallHint(Image, 0x2180)), Mutation == 0)
          << Mutation;
    }
    // Exported names cannot turn a variadic prefix, a by-value aggregate or
    // an unknown callback prototype into a complete scalar declaration.
    for (const char *Name : {"NSLog", "CFStringCreateWithFormat", "sigsetjmp",
                             "vfork", "dispatch_async_f"}) {
      auto Image = runtimeImage("_" + std::string(Name), Architecture);
      Image.DyldBindSlots[0x2180] = {
          "_" + std::string(Name), 0,
          std::string(Name) == "NSLog"
              ? "/System/Library/Frameworks/Foundation.framework/Foundation"
          : std::string(Name) == "CFStringCreateWithFormat"
              ? "/System/Library/Frameworks/CoreFoundation.framework/"
                "CoreFoundation"
              : "/usr/lib/libSystem.B.dylib",
          false};
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Image, 0x2180)) << Name;
    }
  }
}
