//===- ObjCSourceProjectionTests.cpp - Objective-C projection boundaries --===//

#include "../../../lib/sdk/capi/ObjCSourceProjection.h"
#include "../../../lib/sdk/capi/ObjCSynchronizedSource.h"
#include "../../../lib/sdk/capi/SourceProjectionEvidenceJSON.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::sdk;

struct Projection {
  SourceFunctionTypeHint Hint;
  HighFunc Func;
  PipelineFunctionAudit Audit;

  Projection() {
    Hint.ReturnType = NdType::makeInt(4, true);
    Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                       {"objc_cmd", NdType::makePtr(NdType::makeVoid())},
                       {"arg0", NdType::makeInt(4, true)}};
    Func.Entry = 0x1000;
    Func.Name = "neverd_objc_imp_1000";
    Func.ReturnType = Hint.ReturnType;
    Func.SourceTypeHint = Hint;
    for (const auto &Parameter : Hint.Parameters)
      Func.Params.push_back({Parameter.Name, Parameter.Type});
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeConst(42, 4);
    Func.Body.push_back(Return);
    Audit.Entry = Func.Entry;
    Audit.Disposition = PipelineFunctionDisposition::Accepted;
    Audit.DecodedInstructions = Audit.LiftedInstructions = 2;
    Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  }

  std::string limitation() const {
    return objcSourceBodyLimitation(Func, Hint, &Audit);
  }
};

std::string emit(const HighFunc &Func) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  return Source;
}

void executeSynchronizedSource(const std::string &Source,
                               llvm::StringRef Harness) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Found = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(Found);
  const std::string Compiler = *Found;
#endif
  const auto CXX = llvm::sys::findProgramByName("clang++");
  ASSERT_TRUE(CXX);
  llvm::SmallString<128> SourcePath, HarnessPath, ObjectPath, BinaryPath,
      ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-sync", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-sync", "cpp", HarnessPath));
  llvm::FileRemover RemoveHarness(HarnessPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-sync", "o", ObjectPath));
  llvm::FileRemover RemoveObject(ObjectPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-sync", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-sync", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code FileError;
  {
    llvm::raw_fd_ostream Out(SourcePath, FileError);
    ASSERT_FALSE(FileError);
    Out << Source;
  }
  {
    llvm::raw_fd_ostream Out(HarnessPath, FileError);
    ASSERT_FALSE(FileError);
    Out << Harness;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef> CompileArgs{Compiler,
                                                         "-x",
                                                         "c",
                                                         "-std=gnu11",
                                                         "-fexceptions",
                                                         "-Werror",
                                                         "-Wno-unused-value",
                                                         Optimization,
                                                         "-c",
                                                         SourcePath,
                                                         "-o",
                                                         ObjectPath};
    std::string Error;
    const auto Compile = llvm::sys::ExecuteAndWait(
        Compiler, CompileArgs, std::nullopt, Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Compile, 0) << Error
                          << (Errors ? (*Errors)->getBuffer().str() : "")
                          << Source;
    const llvm::SmallVector<llvm::StringRef> LinkArgs{
        *CXX,        "-std=gnu++17", "-fexceptions", "-Werror", Optimization,
        HarnessPath, ObjectPath,     "-o",           BinaryPath};
    const auto Link = llvm::sys::ExecuteAndWait(*CXX, LinkArgs, std::nullopt,
                                                Redirects, 30, 0, &Error);
    Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Link, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "")
                       << Harness.str();
    EXPECT_EQ(llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                        Redirects, 30, 0, &Error),
              0)
        << Error << Source << Harness.str();
  }
}

TEST(ObjCSourceProjection,
     CompleteConstantBodyAndUnusedParametersAreSupported) {
  Projection P;
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  // C projection does not require an LLVM definition or invent one as proof.
  EXPECT_FALSE(P.Audit.HasLLVMDefinition);
  const std::string Source = emit(P.Func);
  EXPECT_NE(Source.find("return 42;"), std::string::npos) << Source;
  EXPECT_TRUE(objcSourceTextLimitation(Source).empty());
}

TEST(ObjCSourceProjection, SignatureChangesCannotInheritRecoveryStatus) {
  using Mutation = std::function<void(Projection &)>;
  const std::vector<std::pair<const char *, Mutation>> Mutations = {
      {"missing hint", [](Projection &P) { P.Func.SourceTypeHint.reset(); }},
      {"return width",
       [](Projection &P) { P.Func.ReturnType = NdType::makeInt(8); }},
      {"return signedness",
       [](Projection &P) { P.Func.ReturnType = NdType::makeInt(4, false); }},
      {"parameter type",
       [](Projection &P) {
         P.Func.Params[2].Type = NdType::makeInt(4, false);
       }},
      {"parameter pointee",
       [](Projection &P) {
         P.Func.Params[0].Type = NdType::makePtr(NdType::makeInt(4));
       }},
      {"parameter order",
       [](Projection &P) { std::swap(P.Func.Params[0], P.Func.Params[1]); }},
      {"missing parameter", [](Projection &P) { P.Func.Params.pop_back(); }},
      {"additional parameter",
       [](Projection &P) {
         P.Func.Params.push_back({"extra", NdType::makeInt(4)});
       }},
      {"bound hint drift",
       [](Projection &P) {
         P.Func.SourceTypeHint->Parameters[2].Name = "wrong";
       }},
  };
  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    Projection P;
    Mutate(P);
    EXPECT_FALSE(P.limitation().empty());
  }
}

TEST(ObjCSourceProjection, RequiresEveryInstructionAndCompleteMatchingAudit) {
  Projection Complete;
  EXPECT_FALSE(
      objcSourceBodyLimitation(Complete.Func, Complete.Hint, nullptr).empty());
  using Mutation = std::function<void(PipelineFunctionAudit &)>;
  const std::vector<std::pair<const char *, Mutation>> Mutations = {
      {"wrong function", [](auto &A) { ++A.Entry; }},
      {"empty decode",
       [](auto &A) { A.DecodedInstructions = A.LiftedInstructions = 0; }},
      {"incomplete lift", [](auto &A) { --A.LiftedInstructions; }},
      {"limited",
       [](auto &A) {
         A.Disposition = PipelineFunctionDisposition::SkippedLimit;
       }},
      {"missing low IR", [](auto &A) { A.HasLowIR = false; }},
      {"missing med IR", [](auto &A) { A.HasMedIR = false; }},
      {"unverified med IR", [](auto &A) { A.MedIRVerified = false; }},
      {"decode failure", [](auto &A) { A.DecodeFailures.push_back(0x1004); }},
      {"unsupported instruction",
       [](auto &A) { A.UnsupportedInstructions.push_back(0x1004); }},
      {"truncated path", [](auto &A) { A.TruncatedPaths.push_back(0x1008); }},
  };
  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    Projection P;
    Mutate(P.Audit);
    EXPECT_FALSE(P.limitation().empty());
  }
}

TEST(ObjCSourceProjection, UnboundCallDiagnosticIdentifiesAndClearsTheFailure) {
  Projection P;
  auto Call = HighExpr::makeCall("external", 0x2000, {});
  P.Func.Body[0].RetVal = Call;
  const HighExpr *Failure = nullptr;
  EXPECT_FALSE(
      objcSourceBodyLimitation(P.Func, P.Hint, &P.Audit, {}, &Failure).empty());
  EXPECT_EQ(Failure, Call.get());
  EXPECT_TRUE(objcSourceBodyLimitation(
                  P.Func, P.Hint, &P.Audit,
                  [](const HighExpr &) { return true; }, &Failure)
                  .empty());
  EXPECT_EQ(Failure, nullptr);
  Failure = Call.get();
  auto WrongOrigin = P.Hint;
  WrongOrigin.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  EXPECT_FALSE(
      objcSourceBodyLimitation(P.Func, WrongOrigin, &P.Audit, {}, &Failure)
          .empty());
  EXPECT_EQ(Failure, nullptr);
}

TEST(ObjCSourceProjection,
     InventoriesIndependentCallsAndTheirStatementAddresses) {
  Projection P;
  auto SharedCall = HighExpr::makeCall("first", 0x2000, {});
  HighStmt First;
  First.Kind = StmtKind::ExprStmt;
  First.Addr = 0x1000;
  First.Val = SharedCall;
  HighStmt Second = First;
  Second.Addr = 0x1004;
  P.Func.Body.insert(P.Func.Body.begin(), {First, Second});
  P.Func.Body.back().Addr = 0x1008;
  P.Func.Body.back().RetVal =
      HighExpr::makeCall("last", 0x3000, {HighExpr::makeUndef(8)});

  const auto Report = sourceBodyDiagnostics(P.Func, P.Hint, &P.Audit);
  EXPECT_TRUE(Report.Complete);
  std::set<va_t> Calls;
  bool Unresolved = false;
  for (const auto &Item : Report.Items) {
    if (Item.Issue == SourceProjectionIssue::CallBinding) {
      ASSERT_NE(Item.Expression, nullptr);
      Calls.insert(Item.StatementAddress);
    }
    Unresolved |= Item.Issue == SourceProjectionIssue::UnresolvedValue;
  }
  EXPECT_EQ(Calls, (std::set<va_t>{0x1000, 0x1004, 0x1008}));
  EXPECT_TRUE(Unresolved);
  const HighExpr *FirstRejected = nullptr;
  EXPECT_EQ(sourceBodyLimitation(P.Func, P.Hint, &P.Audit, {}, &FirstRejected),
            Report.limitation());
  EXPECT_EQ(FirstRejected, SharedCall.get());
}

TEST(ObjCSourceProjection,
     UnknownEdgesLeaveFlowIncompleteButStillInventoryCalls) {
  Projection P;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.Addr = 0x1004;
  Jump.GotoTarget = 0x9000;
  P.Func.Body.insert(P.Func.Body.begin(), Jump);
  P.Func.Body.back().RetVal = HighExpr::makeCall("external", 0x2000, {});
  const auto Report = sourceBodyDiagnostics(P.Func, P.Hint, &P.Audit);
  EXPECT_FALSE(Report.Complete);
  ASSERT_EQ(Report.Items.size(), 2u);
  EXPECT_EQ(Report.Items[0].Issue, SourceProjectionIssue::ControlFlow);
  EXPECT_EQ(Report.Items[0].StatementAddress, 0x1004u);
  EXPECT_EQ(Report.Items[0].RelatedAddress, 0x9000u);
  EXPECT_EQ(Report.Items[1].Issue, SourceProjectionIssue::CallBinding);
  EXPECT_EQ(P.limitation(), Report.limitation());
}

TEST(ObjCSourceProjection, InventoriesFallthroughAndEachUninitializedLocal) {
  Projection P;
  MedVar Left;
  Left.Kind = MedVar::Reg;
  Left.Id = 11;
  Left.SSAVer = 2;
  MedVar Right = Left;
  Right.Id = 12;
  P.Func.Body[0].Kind = StmtKind::ExprStmt;
  P.Func.Body[0].Addr = 0x1000;
  P.Func.Body[0].RetVal.reset();
  P.Func.Body[0].Val = HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeVar(Left), HighExpr::makeVar(Right));
  const auto Report = sourceBodyDiagnostics(P.Func, P.Hint, &P.Audit);
  EXPECT_TRUE(Report.Complete);
  EXPECT_EQ(P.limitation(), Report.limitation());
  std::set<int> MissingDefinitions;
  for (const auto &Item : Report.Items)
    if (Item.Issue == SourceProjectionIssue::DefiniteAssignment) {
      ASSERT_NE(Item.Expression, nullptr);
      EXPECT_EQ(Item.StatementAddress, 0x1000u);
      MissingDefinitions.insert(Item.Expression->Var.Id);
    }
  EXPECT_EQ(MissingDefinitions, (std::set<int>{11, 12}));
}

TEST(ObjCSourceProjection, InvalidSignatureDoesNotHideIndependentBodyEvidence) {
  Projection P;
  P.Hint.ReturnType.reset();
  P.Hint.Parameters[0].Type.reset();
  MedVar InvalidParameter;
  InvalidParameter.Kind = MedVar::Param;
  InvalidParameter.Id = 999;
  P.Func.Body[0].RetVal = HighExpr::makeCall(
      "external", 0x2000, {HighExpr::makeVar(InvalidParameter)});
  const auto Report = sourceBodyDiagnostics(P.Func, P.Hint, &P.Audit);
  EXPECT_FALSE(Report.Complete);
  std::set<SourceProjectionIssue> Issues;
  for (const auto &Item : Report.Items)
    Issues.insert(Item.Issue);
  EXPECT_TRUE(Issues.count(SourceProjectionIssue::Signature));
  EXPECT_TRUE(Issues.count(SourceProjectionIssue::CallBinding));
  EXPECT_TRUE(Issues.count(SourceProjectionIssue::ParameterBinding));
  EXPECT_EQ(P.limitation(), Report.limitation());
}

TEST(ObjCSourceProjection,
     EvidenceBudgetCannotTurnAnIncompleteGraphIntoSuccess) {
  Projection P;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x9000;
  P.Func.Body.insert(P.Func.Body.begin(), Jump);
  for (size_t Index = 0; Index < SourceProjectionDiagnostics::MaxDiagnostics;
       ++Index) {
    HighStmt Call;
    Call.Kind = StmtKind::ExprStmt;
    Call.Val = HighExpr::makeCall("external", 0x2000, {});
    P.Func.Body.push_back(Call);
  }
  const auto Report = sourceBodyDiagnostics(P.Func, P.Hint, &P.Audit);
  EXPECT_FALSE(Report.Complete);
  ASSERT_EQ(Report.Items.size(),
            SourceProjectionDiagnostics::MaxDiagnostics + 1);
  EXPECT_EQ(Report.Items.back().Issue, SourceProjectionIssue::Budget);
  EXPECT_EQ(P.limitation(), Report.limitation());
}

TEST(ObjCSourceProjection, RejectsUnresolvedBodiesCallsAndExceptionState) {
  using Mutation = std::function<void(Projection &)>;
  const std::vector<std::pair<const char *, Mutation>> Mutations = {
      {"empty body", [](Projection &P) { P.Func.Body.clear(); }},
      {"missing return",
       [](Projection &P) { P.Func.Body[0].Kind = StmtKind::Nop; }},
      {"missing return value",
       [](Projection &P) { P.Func.Body[0].RetVal.reset(); }},
      {"undef",
       [](Projection &P) { P.Func.Body[0].RetVal = HighExpr::makeUndef(4); }},
      {"native call",
       [](Projection &P) {
         P.Func.Body[0].RetVal = HighExpr::makeCall("external", 0x2000, {});
       }},
      {"exception metadata",
       [](Projection &P) { P.Func.ExceptionMetadata.emplace(); }},
      {"exception region",
       [](Projection &P) { P.Func.Body[0].Kind = StmtKind::ItaniumTry; }},
  };
  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    Projection P;
    Mutate(P);
    EXPECT_FALSE(P.limitation().empty());
  }
}

TEST(ObjCSourceProjection, OrdinaryDarwinUnwindDoesNotImplyExceptionCode) {
  for (const auto Encoding :
       {ExceptionEncoding::CompactUnwind, ExceptionEncoding::DwarfFDE}) {
    Projection P;
    auto &Metadata = P.Func.ExceptionMetadata.emplace();
    Metadata.Encoding = Encoding;
    if (Encoding == ExceptionEncoding::CompactUnwind)
      Metadata.Compact.emplace();
    else
      Metadata.Dwarf.emplace();
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
    Metadata.ObjC.emplace();
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  }
}

TEST(ObjCSourceProjection, UnhandledObjCThrowHasSourceFlowWithoutDispatch) {
  Projection P;
  auto &Metadata = P.Func.ExceptionMetadata.emplace();
  Metadata.CodeRange = {0x1000, 0x1100};
  Metadata.Encoding = ExceptionEncoding::CompactUnwind;
  Metadata.Compact.emplace();
  Metadata.ObjC.emplace().RuntimeCalls = {
      {0x1004, 0x2000, "objc_exception_throw", ObjCRuntimeCallKind::Throw}};
  EXPECT_FALSE(P.limitation().empty());

  HighStmt Throw;
  Throw.Kind = StmtKind::ExprStmt;
  Throw.Addr = 0x1004;
  Throw.Val = HighExpr::makeCall("objc_exception_throw", 0x2000,
                                 {HighExpr::makeConst(1, 8)});
  auto ThrowHint = std::make_shared<SourceCallTypeHint>();
  ThrowHint->CallKind = SourceCallTypeHint::Kind::ObjCRuntimeCall;
  ThrowHint->TargetName = "objc_exception_throw";
  ThrowHint->DoesNotReturn = true;
  Throw.Val->SourceCallHint = ThrowHint;
  P.Func.Body.insert(P.Func.Body.begin(), Throw);
  const auto CallAllowed = [](const HighExpr &Expression) {
    return Expression.SourceCallHint &&
           Expression.SourceCallHint->TargetName == "objc_exception_throw";
  };
  auto Limitation = [&] {
    return objcSourceBodyLimitation(P.Func, P.Hint, &P.Audit, CallAllowed);
  };
  EXPECT_TRUE(Limitation().empty()) << Limitation();
  Metadata.ObjC->RuntimeCalls[0].TargetName = "_objc_exception_throw";
  EXPECT_TRUE(Limitation().empty()) << Limitation();
  Throw.Val->CallAddr = 0x2004;
  EXPECT_FALSE(Limitation().empty());
  Throw.Val->CallAddr = 0x2000;
  ThrowHint->DoesNotReturn = false;
  EXPECT_FALSE(Limitation().empty());
  ThrowHint->DoesNotReturn = true;

  Metadata.Compact->HasLSDA = true;
  EXPECT_FALSE(Limitation().empty());
  Metadata.Compact->HasLSDA = false;
  Metadata.ObjC->RuntimeCalls[0].TargetName = "objc_exception_rethrow";
  EXPECT_FALSE(Limitation().empty());
}

TEST(ObjCSourceProjection,
     EnclosingUnwindRuntimeCallsBelongOnlyToDecodedSubentries) {
  Projection P;
  P.Func.Entry = P.Audit.Entry = 0x1100;
  ExceptionFunction Metadata;
  Metadata.CodeRange = {0x1000, 0x1200};
  Metadata.Encoding = ExceptionEncoding::CompactUnwind;
  Metadata.Compact.emplace();
  Metadata.ObjC.emplace().RuntimeCalls = {
      {0x1004, 0x2000, "objc_exception_throw", ObjCRuntimeCallKind::Throw}};

  P.Func.ExceptionMetadata =
      sourceUnwindForDecodedSubentry(Metadata, P.Func.Entry, {0x1100, 0x1104});
  ASSERT_TRUE(P.Func.ExceptionMetadata);
  EXPECT_FALSE(P.Func.ExceptionMetadata->ObjC);
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();

  Metadata.ObjC->RuntimeCalls.push_back(
      {0x1104, 0x2000, "objc_exception_throw", ObjCRuntimeCallKind::Throw});
  const auto LocalThrow =
      sourceUnwindForDecodedSubentry(Metadata, P.Func.Entry, {0x1100, 0x1104});
  ASSERT_TRUE(LocalThrow.ObjC);
  ASSERT_EQ(LocalThrow.ObjC->RuntimeCalls.size(), 1U);
  EXPECT_EQ(LocalThrow.ObjC->RuntimeCalls[0].CallVA, 0x1104U);
  EXPECT_FALSE(isPlainSourceUnwind(LocalThrow));

  // The first method can end before the record does: the later throw belongs
  // to an adjacent entry, even though this method starts at CodeRange.Begin.
  const auto First =
      sourceUnwindForDecodedSubentry(Metadata, 0x1000, {0x1000, 0x1008});
  EXPECT_FALSE(First.ObjC);
  EXPECT_TRUE(isPlainSourceUnwind(First));
  const auto FirstWithThrow =
      sourceUnwindForDecodedSubentry(Metadata, 0x1000, {0x1000, 0x1004});
  ASSERT_TRUE(FirstWithThrow.ObjC);
  ASSERT_EQ(FirstWithThrow.ObjC->RuntimeCalls.size(), 1U);
  EXPECT_EQ(FirstWithThrow.ObjC->RuntimeCalls[0].CallVA, 0x1004U);
  EXPECT_FALSE(isPlainSourceUnwind(FirstWithThrow));
  EXPECT_EQ(sourceUnwindForDecodedSubentry(Metadata, 0x1100, {0x1104})
                .ObjC->RuntimeCalls.size(),
            2U);
  Metadata.Compact->HasLSDA = true;
  EXPECT_EQ(sourceUnwindForDecodedSubentry(Metadata, 0x1100, {0x1100, 0x1104})
                .ObjC->RuntimeCalls.size(),
            2U);
}

TEST(ObjCSourceProjection,
     UnwindWithLanguageDispatchOrPartialMetadataIsRejected) {
  using Mutation = std::function<void(ExceptionFunction &)>;
  const std::vector<std::pair<const char *, Mutation>> Mutations = {
      {"partial decode",
       [](auto &M) { M.ParseStatus = ExceptionParseStatus::Partial; }},
      {"personality kind",
       [](auto &M) {
         M.Personality = ExceptionPersonality::ObjCPersonalityV0;
       }},
      {"personality address", [](auto &M) { M.PersonalityVA = 0x2000; }},
      {"unresolved personality",
       [](auto &M) { M.PersonalityName = "__objc_personality_v0"; }},
      {"handler data", [](auto &M) { M.HandlerDataVA = 0x2000; }},
      {"language table", [](auto &M) { M.Itanium.emplace(); }},
      {"objc dispatch",
       [](auto &M) { M.ObjC.emplace().LandingPads.emplace_back(); }},
      {"objc runtime throw",
       [](auto &M) {
         M.ObjC.emplace().RuntimeCalls.push_back({0x1010, 0x2000,
                                                  "objc_exception_throw",
                                                  ObjCRuntimeCallKind::Throw});
       }},
      {"compact personality",
       [](auto &M) { M.Compact->PersonalityVA = 0x2000; }},
      {"compact LSDA flag", [](auto &M) { M.Compact->HasLSDA = true; }},
      {"compact LSDA address", [](auto &M) { M.Compact->LSDAVA = 0x2000; }},
      {"partial compact frame",
       [](auto &M) {
         M.Compact->SemanticStatus = CompactUnwindSemanticStatus::Partial;
       }},
      {"dwarf LSDA", [](auto &M) { M.Dwarf.emplace().LSDAVA = 0x2000; }},
      {"missing compact record", [](auto &M) { M.Compact.reset(); }},
  };
  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    Projection P;
    auto &Metadata = P.Func.ExceptionMetadata.emplace();
    Metadata.Encoding = ExceptionEncoding::CompactUnwind;
    Metadata.Compact.emplace();
    Mutate(Metadata);
    EXPECT_FALSE(P.limitation().empty());
  }
}

TEST(ObjCSourceProjection, ParameterReferencesMustOccupyTheDeclaredABISlot) {
  for (const auto Architecture : {Arch::X64, Arch::AArch64}) {
    Projection P;
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Architecture;
    Parameter.Id = 2;
    Parameter.Size = 4;
    Parameter.RegOff = getTargetRegInfo(Architecture).IntParamRegs[2];
    P.Func.Body[0].RetVal = HighExpr::makeVar(Parameter);
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
    P.Func.Body[0].RetVal->Var.RegOff =
        getTargetRegInfo(Architecture).IntParamRegs[3];
    EXPECT_FALSE(P.limitation().empty());
    P.Func.Body[0].RetVal->Var = Parameter;
    P.Func.Body[0].RetVal->Var.Id = 3;
    EXPECT_FALSE(P.limitation().empty());
    P.Func.Body[0].RetVal->Var = Parameter;
    P.Func.Body[0].RetVal->Var.Kind = MedVar::Reg;
    EXPECT_FALSE(P.limitation().empty());
  }
}

TEST(ObjCSourceProjection,
     SyntheticFrameBaseIsExplainedButIncomingFlagsAreNot) {
  Projection P;
  MedVar Register;
  Register.Kind = MedVar::Reg;
  Register.TheArch = Arch::X64;
  Register.Id = 9;
  Register.Size = 8;
  Register.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  P.Func.Body[0].RetVal = HighExpr::makeVar(Register);
  EXPECT_FALSE(P.limitation().empty());
  P.Func.FrameSize = 16;
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  P.Func.Body[0].RetVal->Var.Kind = MedVar::Flag;
  EXPECT_FALSE(P.limitation().empty());
}

TEST(ObjCSourceProjection, MissingLocalDefinitionsCannotBeRecovered) {
  for (const auto Kind : {MedVar::Temp, MedVar::Reg, MedVar::Stack}) {
    for (bool Renamed : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(Kind));
      SCOPED_TRACE(Renamed);
      Projection P;
      MedVar Local;
      Local.Kind = Kind;
      Local.TheArch = Arch::X64;
      Local.Id = 99;
      Local.SSAVer = 1;
      Local.Size = 4;
      Local.StackOff = -8;
      Local.RenameTag = Renamed ? 7 : -1;
      P.Func.Body[0].RetVal = HighExpr::makeVar(Local);
      EXPECT_FALSE(P.limitation().empty());

      HighStmt Definition;
      Definition.Kind = StmtKind::Assign;
      Definition.Dst = HighExpr::makeVar(Local);
      Definition.Val = HighExpr::makeConst(42, 4);
      P.Func.Body.insert(P.Func.Body.begin(), Definition);
      EXPECT_TRUE(P.limitation().empty()) << P.limitation();
    }
  }
}

MedVar flowLocal() {
  MedVar Local;
  Local.Kind = MedVar::Temp;
  Local.TheArch = Arch::X64;
  Local.Id = 99;
  Local.SSAVer = 1;
  Local.Size = 4;
  return Local;
}

ExprPtr flowCondition() {
  MedVar Parameter;
  Parameter.Kind = MedVar::Param;
  Parameter.TheArch = Arch::X64;
  Parameter.Id = 2;
  Parameter.Size = 4;
  Parameter.RegOff = getTargetRegInfo(Arch::X64).IntParamRegs[2];
  return HighExpr::makeVar(Parameter);
}

HighStmt flowAssignment(uint64_t Value = 42) {
  HighStmt Statement;
  Statement.Kind = StmtKind::Assign;
  Statement.Dst = HighExpr::makeVar(flowLocal());
  Statement.Val = HighExpr::makeConst(Value, 4);
  return Statement;
}

HighStmt flowReturn(ExprPtr Value = HighExpr::makeVar(flowLocal())) {
  HighStmt Statement;
  Statement.Kind = StmtKind::Return;
  Statement.RetVal = std::move(Value);
  return Statement;
}

TEST(ObjCSourceProjection, OneBranchDefinitionDoesNotCoverTheOtherPath) {
  Projection P;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = flowCondition();
  Branch.Body = {flowAssignment()};
  P.Func.Body = {Branch, flowReturn()};
  EXPECT_FALSE(P.limitation().empty())
      << "A reachable return reads a local undefined on the false branch";

  Branch.Kind = StmtKind::IfElse;
  Branch.ElseBody = {flowAssignment(7)};
  P.Func.Body = {Branch, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, RepeatedStableGuardProvesConditionalDefinition) {
  Projection P;
  HighStmt Define;
  Define.Kind = StmtKind::If;
  Define.Cond = flowCondition();
  Define.Body = {flowAssignment()};
  HighStmt Use;
  Use.Kind = StmtKind::If;
  Use.Cond = flowCondition();
  Use.Body = {flowReturn()};
  P.Func.Body = {Define, Use, flowReturn(HighExpr::makeConst(7, 4))};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();

  HighStmt Change;
  Change.Kind = StmtKind::Assign;
  Change.Dst = flowCondition();
  Change.Val = HighExpr::makeConst(1, 4);
  P.Func.Body.insert(P.Func.Body.begin() + 1, Change);
  EXPECT_FALSE(P.limitation().empty())
      << "Overwriting the guard must invalidate the earlier branch fact";
}

TEST(ObjCSourceProjection, GuardedPhiCleanupRemovesTheUndefinedReadItself) {
  Projection P;
  HighStmt Define;
  Define.Kind = StmtKind::IfElse;
  Define.Cond = flowCondition();
  Define.Body = {flowAssignment()};
  auto Copy = flowAssignment();
  auto Incoming = flowLocal();
  Incoming.Id = 100;
  Copy.Val = HighExpr::makeVar(Incoming);
  Copy.IsPhiCopy = true;
  Copy.Addr = 0x2000;
  Define.ElseBody = {Copy};
  HighStmt Use;
  Use.Kind = StmtKind::If;
  Use.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, flowCondition(),
                                 HighExpr::makeConst(0, 4));
  Use.Body = {flowReturn()};
  P.Func.Body = {Define, Use, flowReturn(HighExpr::makeConst(7, 4))};
  EXPECT_FALSE(P.limitation().empty());
  ASSERT_TRUE(eliminateHighDeadPhiCopies(P.Func));
  EXPECT_EQ(P.Func.Body[0].ElseBody[0].Kind, StmtKind::Nop);
  EXPECT_EQ(P.Func.Body[0].ElseBody[0].Addr, 0x2000u);
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  EXPECT_FALSE(eliminateHighDeadPhiCopies(P.Func));
}

TEST(ObjCSourceProjection, GuardFactsDoNotCrossDifferentLocalsOrWidths) {
  for (bool DifferentWidth : {false, true}) {
    Projection P;
    HighStmt Define;
    Define.Kind = StmtKind::If;
    Define.Cond = flowCondition();
    Define.Body = {flowAssignment()};
    HighStmt Use = Define;
    Use.Cond = flowCondition();
    Use.Body = {flowReturn()};
    if (DifferentWidth) {
      Use.Cond->Var.Size = 1;
      Use.Cond->Type = NdType::makeInt(1);
    } else {
      Use.Cond->Var.Id = 3;
    }
    P.Func.Body = {Define, Use, flowReturn(HighExpr::makeConst(7, 4))};
    const auto Report = analyzeHighSourceFlow(P.Func, true);
    ASSERT_TRUE(Report.Complete);
    EXPECT_FALSE(Report.Items.empty());
  }
}

TEST(ObjCSourceProjection, EscapedGuardAndIntrinsicWritesInvalidateFacts) {
  for (bool AddressEscape : {false, true}) {
    Projection P;
    HighStmt Define;
    Define.Kind = StmtKind::If;
    Define.Cond = flowCondition();
    Define.Body = {flowAssignment()};
    HighStmt Use = Define;
    Use.Cond = flowCondition();
    Use.Body = {flowReturn()};
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall("may_change_guard", 0x3000, {});
    if (AddressEscape) {
      auto Address = std::make_shared<HighExpr>();
      Address->Kind = ExprKind::Addr;
      Address->Type = NdType::makePtr();
      Address->Operands = {flowCondition()};
      Call.CallExpr->Operands = {Address};
    } else {
      Call.CallExpr->IntrinsicOutputs = {flowCondition()->Var};
    }
    P.Func.Body = {Define, Call, Use, flowReturn(HighExpr::makeConst(7, 4))};
    const auto Report = analyzeHighSourceFlow(P.Func, true);
    ASSERT_TRUE(Report.Complete);
    EXPECT_FALSE(Report.Items.empty());
  }
}

TEST(ObjCSourceProjection, GotoIntoGuardedArmCannotBorrowItsCondition) {
  Projection P;
  HighStmt Define;
  Define.Kind = StmtKind::If;
  Define.Cond = flowCondition();
  Define.Body = {flowAssignment()};
  HighStmt Use = Define;
  Use.Body = {flowReturn()};
  Use.Body[0].Addr = 0x2000;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x2000;
  P.Func.Body = {Define, Jump, Use, flowReturn(HighExpr::makeConst(7, 4))};
  EXPECT_FALSE(P.limitation().empty());
}

TEST(ObjCSourceProjection, PredicatePartitionBudgetFallsBackConservatively) {
  Projection P;
  P.Func.Body.clear();
  // Repeated independent tests produce more than 32 contexts at a join.
  // An unreachable return must never be inferred from a partial expansion.
  for (int Pass = 0; Pass < 2; ++Pass)
    for (int Id = 2; Id < 10; ++Id) {
      HighStmt Branch;
      Branch.Kind = StmtKind::If;
      Branch.Cond = flowCondition();
      Branch.Cond->Var.Id = Id;
      P.Func.Body.push_back(Branch);
    }
  P.Func.Body.push_back(flowReturn());
  auto Report = analyzeHighSourceFlow(P.Func, true);
  ASSERT_TRUE(Report.Complete);
  ASSERT_FALSE(Report.Items.empty());
  EXPECT_EQ(Report.Items[0].Issue, HighSourceFlowIssue::DefiniteAssignment);
}

TEST(ObjCSourceProjection, AReadCannotBorrowALaterOrUnreachableDefinition) {
  Projection P;
  P.Func.Body = {flowReturn(), flowAssignment()};
  EXPECT_FALSE(P.limitation().empty())
      << "A definition after return cannot initialize its operand";

  P.Func.Body = {flowAssignment(), flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, EveryReachableNonVoidExitNeedsAReturn) {
  Projection P;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = flowCondition();
  Branch.Body = {flowReturn(HighExpr::makeConst(42, 4))};
  P.Func.Body = {Branch};
  EXPECT_FALSE(P.limitation().empty())
      << "A return in one arm does not cover the fallthrough exit";

  P.Func.Body.push_back(flowReturn(HighExpr::makeConst(7, 4)));
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  Branch.Kind = StmtKind::IfElse;
  Branch.ElseBody = {flowReturn(HighExpr::makeConst(7, 4))};
  P.Func.Body = {Branch};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, EarlyReturnDoesNotPolluteTheContinuingPath) {
  Projection P;
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = flowCondition();
  Branch.Body = {flowReturn(HighExpr::makeConst(7, 4))};
  Branch.ElseBody = {flowAssignment()};
  P.Func.Body = {Branch, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, LoopDefinitionsRespectWhetherTheBodyExecutes) {
  for (const auto Kind : {StmtKind::While, StmtKind::For}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    Projection P;
    HighStmt Loop;
    Loop.Kind = Kind;
    Loop.Cond = flowCondition();
    Loop.Body = {flowAssignment()};
    P.Func.Body = {Loop, flowReturn()};
    EXPECT_FALSE(P.limitation().empty())
        << "A possibly zero-iteration loop cannot initialize an exit value";

    P.Func.Body.insert(P.Func.Body.begin(), flowAssignment(7));
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
    Loop.Kind = StmtKind::DoWhile;
    P.Func.Body = {Loop, flowReturn()};
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  }
}

TEST(ObjCSourceProjection, ContinueCannotSkipARequiredLoopDefinition) {
  Projection P;
  HighStmt Continue;
  Continue.Kind = StmtKind::Continue;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = flowCondition();
  Branch.Body = {Continue};
  HighStmt Loop;
  Loop.Kind = StmtKind::DoWhile;
  // continue reaches a false test and exits before the assignment.
  Loop.Cond = HighExpr::makeConst(0, 1);
  Loop.Body = {Branch, flowAssignment()};
  P.Func.Body = {Loop, flowReturn()};
  EXPECT_FALSE(P.limitation().empty())
      << "continue reaches the loop test without defining the exit value";

  P.Func.Body.insert(P.Func.Body.begin(), flowAssignment(7));
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, RepeatedLoopTestDoesNotInventAnExitingPath) {
  Projection P;
  HighStmt Continue;
  Continue.Kind = StmtKind::Continue;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = flowCondition();
  Branch.Body = {Continue};
  HighStmt Loop;
  Loop.Kind = StmtKind::DoWhile;
  Loop.Cond = flowCondition();
  Loop.Body = {Branch, flowAssignment()};
  P.Func.Body = {Loop, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();

  HighStmt Change;
  Change.Kind = StmtKind::Assign;
  Change.Dst = flowCondition();
  Change.Val = HighExpr::makeConst(0, 4);
  P.Func.Body[0].Body[0].Body.insert(P.Func.Body[0].Body[0].Body.begin(),
                                     Change);
  EXPECT_FALSE(P.limitation().empty())
      << "Clearing the guard before continue creates an undefined exit";
}

TEST(ObjCSourceProjection, BreakPreservesDefinitionsFromAnEnteredLoop) {
  Projection P;
  HighStmt Break;
  Break.Kind = StmtKind::Break;
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = HighExpr::makeConst(1, 1);
  Loop.Body = {flowAssignment(), Break};
  P.Func.Body = {Loop, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, SwitchDefinitionsNeedToCoverTheDefaultPath) {
  Projection P;
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.SwitchExpr = flowCondition();
  Switch.Cases.push_back({0, {flowAssignment()}});
  P.Func.Body = {Switch, flowReturn()};
  EXPECT_FALSE(P.limitation().empty())
      << "An unmatched case reaches return without defining its value";

  Switch.DefaultBody = {flowAssignment(7)};
  P.Func.Body = {Switch, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, GotoCannotSkipARequiredDefinition) {
  Projection P;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x2000;
  auto Return = flowReturn();
  Return.Addr = Jump.GotoTarget;
  P.Func.Body = {Jump, flowAssignment(), Return};
  EXPECT_FALSE(P.limitation().empty())
      << "The only path jumps over the local definition";

  auto Definition = flowAssignment();
  Definition.Addr = Jump.GotoTarget;
  Return.Addr = 0;
  P.Func.Body = {Jump, Definition, Return};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, BackwardGotoRetainsAReachingDefinition) {
  Projection P;
  HighStmt Label;
  Label.Kind = StmtKind::Nop;
  Label.Addr = 0x2000;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = Label.Addr;
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = flowCondition();
  Branch.Body = {Jump};
  P.Func.Body = {flowAssignment(), Label, Branch, flowReturn()};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, GotoRequiresOneEmittableTarget) {
  Projection P;
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x2000;
  P.Func.Body.insert(P.Func.Body.begin(), Jump);
  EXPECT_FALSE(P.limitation().empty())
      << "A goto with no emitted target cannot be recovered";

  P.Func.Body.back().Addr = Jump.GotoTarget;
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  auto Duplicate = flowAssignment();
  Duplicate.Addr = Jump.GotoTarget;
  P.Func.Body.insert(P.Func.Body.begin(), Duplicate);
  EXPECT_FALSE(P.limitation().empty())
      << "Distinct statements cannot emit the same goto label twice";
}

TEST(ObjCSourceProjection, NoReturnMetadataCannotHideSourceFallthrough) {
  Projection P;
  P.Func.DoesNotReturn = true;
  P.Func.Body = {flowAssignment()};
  EXPECT_FALSE(P.limitation().empty())
      << "_Noreturn is a declaration, not an actual terminating operation";

  P.Func.Body = {flowReturn(HighExpr::makeConst(42, 4))};
  EXPECT_FALSE(P.limitation().empty())
      << "A reachable return contradicts the emitted _Noreturn declaration";
}

TEST(ObjCSourceProjection, ActualInfiniteControlFlowDoesNotNeedAReturnValue) {
  Projection P;
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = HighExpr::makeConst(1, 1);
  P.Func.Body = {Loop};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  P.Func.DoesNotReturn = true;
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();

  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.Addr = Jump.GotoTarget = 0x2000;
  P.Func.Body = {Jump};
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
}

TEST(ObjCSourceProjection, OnlyAnActualUnconditionalTrapTerminatesThePath) {
  Projection P;
  P.Func.DoesNotReturn = true;
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall("trap", 0, {});
  for (auto Id : {Intrinsic::Ud2, Intrinsic::ArmHlt, Intrinsic::Brk,
                  Intrinsic::Hlt_A64}) {
    Call.CallExpr->IntrinsicId = Id;
    P.Func.Body = {Call};
    EXPECT_TRUE(P.limitation().empty()) << P.limitation();
    EXPECT_TRUE(isUnconditionalTrapIntrinsic(Id));
  }
  EXPECT_STREQ(intrinsicCName(Intrinsic::Brk), "__builtin_trap");

  // A debugger can resume after a breakpoint. Its spelling is not proof of
  // a terminating path, even when the containing native function is flagged.
  Call.CallExpr->IntrinsicId = Intrinsic::Int3;
  P.Func.Body = {Call};
  EXPECT_FALSE(P.limitation().empty());

  Call.CallExpr->IntrinsicId = Intrinsic::Ud2;
  Call.CallExpr->SourceCallHint = std::make_shared<SourceCallTypeHint>();
  P.Func.Body = {Call};
  EXPECT_FALSE(P.limitation().empty())
      << "The emitter's explicit source binding takes precedence over the "
         "intrinsic tag; it is not proof that the emitted call traps";
}

TEST(ObjCSourceProjection, ReachingDefinitionsAlsoCoverCallAndStoreOperands) {
  for (const auto Kind :
       {StmtKind::ExprStmt, StmtKind::Store, StmtKind::Call}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    Projection P;
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = flowCondition();
    Branch.Body = {flowAssignment()};
    HighStmt Use;
    Use.Kind = Kind;
    if (Kind == StmtKind::ExprStmt)
      Use.Val = HighExpr::makeVar(flowLocal());
    else if (Kind == StmtKind::Store) {
      Use.StoreAddr = HighExpr::makeConst(0x3000, 8);
      Use.StoreVal = HighExpr::makeVar(flowLocal());
    } else {
      Use.CallExpr = HighExpr::makeCall("bound_helper", 0x4000,
                                        {HighExpr::makeVar(flowLocal())});
    }
    P.Func.Body = {Branch, Use, flowReturn(HighExpr::makeConst(42, 4))};
    auto Check = [&] {
      return sourceBodyLimitation(P.Func, P.Hint, &P.Audit,
                                  [](const HighExpr &) { return true; });
    };
    EXPECT_FALSE(Check().empty());
    HighStmt Block;
    Block.Kind = StmtKind::Block;
    Block.Body = {flowAssignment()};
    P.Func.Body.insert(P.Func.Body.begin(), Block);
    EXPECT_TRUE(Check().empty()) << Check();
  }
}

TEST(ObjCSourceProjection, SourceFlowGraphRejectsExcessiveStatements) {
  Projection P;
  P.Func.Body.assign(100001, HighStmt{});
  P.Func.Body.push_back(flowReturn(HighExpr::makeConst(42, 4)));
  EXPECT_NE(P.limitation().find("limit"), std::string::npos);
}

TEST(ObjCSourceProjection, EmptySwitchArmsStillHaveABoundedFanout) {
  Projection P;
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.SwitchExpr = flowCondition();
  for (unsigned Value = 0; Value < 4097; ++Value)
    Switch.Cases.push_back({Value, {}});
  P.Func.Body = {Switch, flowReturn(HighExpr::makeConst(42, 4))};
  EXPECT_NE(P.limitation().find("switch-case limit"), std::string::npos);
}

TEST(ObjCSourceProjection, LocalInventoryIsBoundedBeforeDataflowAllocation) {
  Projection P;
  P.Func.Body.clear();
  for (unsigned Index = 0; Index < 8193; ++Index) {
    auto Definition = flowAssignment();
    Definition.Dst->Var.Id = Index;
    P.Func.Body.push_back(std::move(Definition));
  }
  P.Func.Body.push_back(flowReturn(HighExpr::makeConst(42, 4)));
  EXPECT_NE(P.limitation().find("local-value limit"), std::string::npos);
}

TEST(ObjCSourceProjection, RenamedPhiUsesItsEmittedLocalIdentity) {
  Projection P;
  MedVar Destination;
  Destination.Kind = MedVar::Temp;
  Destination.TheArch = Arch::AArch64;
  Destination.Id = 17;
  Destination.SSAVer = 2;
  Destination.Size = 4;
  Destination.RenameTag = 3;
  HighStmt Definition;
  Definition.Kind = StmtKind::Assign;
  Definition.IsPhiCopy = true;
  Definition.Dst = HighExpr::makeVar(Destination);
  Definition.Val = HighExpr::makeConst(42, 4);
  P.Func.Body.insert(P.Func.Body.begin(), Definition);
  MedVar Use = Destination;
  Use.Kind = MedVar::Reg;
  Use.Id = 31;
  Use.SSAVer = 5;
  auto Phi = HighExpr::makeVar(Use);
  Phi->Kind = ExprKind::Phi;
  P.Func.Body.back().RetVal = Phi;
  EXPECT_TRUE(P.limitation().empty()) << P.limitation();
  P.Func.Body.back().RetVal->Var.RenameTag = 4;
  EXPECT_FALSE(P.limitation().empty());
}

TEST(ObjCSourceProjection, DetectsTheRealHighCExpressionDepthFallback) {
  Projection P;
  auto Expression = HighExpr::makeConst(42, 4);
  for (unsigned Index = 0; Index < 210; ++Index)
    Expression = HighExpr::makeBinop(NdOp::INT_ADD, Expression,
                                     HighExpr::makeConst(1, 4));
  P.Func.Body[0].RetVal = Expression;
  EXPECT_FALSE(P.limitation().empty());
  const std::string Source = emit(P.Func);
  ASSERT_NE(Source.find("/* truncated: expr too deep */"), std::string::npos)
      << Source;
  EXPECT_FALSE(objcSourceTextLimitation(Source).empty());
}

TEST(ObjCSourceProjection, TextGuardRejectsEmitterPlaceholdersButNotCLiterals) {
  for (const char *Comment :
       {"truncated: expr too deep", "bad unary", "bad binop", "bad load",
        "bad store", "bad cast", "bad addr", "bad field", "unknown_op(0)",
        "unknown expr", "unary 999",
        "caller-saved register clobbered by call: unknown"}) {
    SCOPED_TRACE(Comment);
    const std::string Body =
        std::string("int method(void) { return 0 /* ") + Comment + " */; }";
    EXPECT_FALSE(objcSourceTextLimitation(Body).empty());
    const std::string Literal =
        std::string("const char *value = \"/* ") + Comment + " */\";";
    EXPECT_TRUE(objcSourceTextLimitation(Literal).empty());
  }
  EXPECT_TRUE(objcSourceTextLimitation(
                  "// /* bad load */\nint method(void) { return 42; }")
                  .empty());
  EXPECT_FALSE(objcSourceTextLimitation("\n ").empty());
  EXPECT_FALSE(
      objcSourceTextLimitation("int method(void) { /* incomplete").empty());
}

TEST(ObjCSourceProjection, SerializesEvidenceWithoutBorrowedIdentities) {
  const std::string InvalidName("helper\xff", 7);
  auto Call = HighExpr::makeCall(InvalidName, 0x8000, {});
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->TargetName = InvalidName;
  Call->SourceCallHint = std::move(Hint);
  MedVar Variable;
  Variable.Kind = MedVar::Temp;
  Variable.Id = 7;
  Variable.SSAVer = 3;
  Variable.RenameTag = 2;
  Variable.StackOff = -16;
  auto Value = HighExpr::makeVar(Variable);
  SourceProjectionDiagnostics Diagnostics;
  Diagnostics.add(SourceProjectionIssue::CallBinding, InvalidName, 0x1004,
                  Call.get());
  Diagnostics.add(SourceProjectionIssue::DefiniteAssignment, "missing write",
                  0x1008, Value.get());
  SourceProjectionDiagnostics Dependencies;
  Dependencies.Complete = false;
  Dependencies.add(SourceProjectionIssue::Dependency, "missing dependency", 0,
                   nullptr, 0x9000);
  Diagnostics.append(Dependencies);
  auto Object = sourceProjectionEvidenceJSON(Diagnostics);
  EXPECT_EQ(Object.getBoolean("checks_complete"), false);
  auto *Items = Object.getArray("items");
  ASSERT_TRUE(Items);
  ASSERT_EQ(Items->size(), 3U);
  auto *First = (*Items)[0].getAsObject();
  ASSERT_TRUE(First);
  EXPECT_EQ(First->getString("statement_address"), "0x1004");
  auto *CallObject = First->getObject("call");
  ASSERT_TRUE(CallObject);
  EXPECT_EQ(CallObject->getString("target_address"), "0x8000");
  EXPECT_EQ(CallObject->getString("binding_name"), jsonSafeText(InvalidName));
  auto *Local = (*Items)[1].getAsObject()->getObject("value");
  ASSERT_TRUE(Local);
  EXPECT_EQ(Local->getInteger("ssa_version"), 3);
  EXPECT_EQ(Local->getInteger("rename_tag"), 2);
  EXPECT_EQ(Local->getInteger("stack_offset"), -16);
  EXPECT_EQ((*Items)[2].getAsObject()->getString("related_address"), "0x9000");
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(std::move(Object));
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Call->CallTarget, InvalidName);
}

struct NativeDependencyFixture {
  BinaryImage Image;
  PipelineResult Result;
  NativeDependencyFixture() {
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x5000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    Image.Segments.push_back(std::move(Text));
    Result.SourceImage = &Image;
    for (va_t Entry : {0x1000, 0x2000}) {
      ObjCMethod Method;
      Method.Implementation = Entry;
      Method.Status = "supported";
      Method.TypeHint = SourceFunctionTypeHint{};
      Image.ObjCMethods.push_back(std::move(Method));
    }
    for (va_t Entry : {0x1000, 0x2000, 0x3000}) {
      LowFunc Function;
      Function.Entry = Entry;
      Function.Blocks.emplace_back();
      Function.Blocks[0].StartAddr = Entry;
      Result.LowFuncs.push_back(std::move(Function));
    }
  }
  void call(size_t Function, va_t Target, bool Indirect = false) {
    auto &Block = Result.LowFuncs[Function].Blocks[0];
    LowOp Op;
    Op.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
    Op.Addr = Block.StartAddr + Block.Ops.size() * 4;
    Op.addInput(NdVar::cst(Target, 8));
    Block.Ops.push_back(Op);
  }
};

TEST(ObjCSourceProjection, IntegerPairDemandOnlyFollowsExactDirectTail) {
  NativeDependencyFixture F;
  F.Image.Arch = Arch::AArch64;
  F.call(0, 0x3000);
  auto &Function = F.Result.LowFuncs[0];
  auto &Ops = Function.Blocks[0].Ops;
  const auto ReturnRegister = getTargetRegInfo(F.Image.Arch).IntReturnReg;
  Ops[0].Output = NdVar::reg(ReturnRegister, 8);
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Ops[0].Addr;
  Return.addInput(NdVar::reg(ReturnRegister, 8));
  Ops.push_back(Return);
  EXPECT_EQ(forwardedNativeIntegerPairTarget(F.Image, Function), 0x3000U);

  Ops.back().Addr += 4;
  EXPECT_FALSE(forwardedNativeIntegerPairTarget(F.Image, Function));
  Ops.back().Addr = Ops[0].Addr;
  Ops[0].Opcode = NdOp::INDIR_CALL;
  EXPECT_FALSE(forwardedNativeIntegerPairTarget(F.Image, Function));
  Ops[0].Opcode = NdOp::CALL;
  Function.Blocks.emplace_back();
  EXPECT_FALSE(forwardedNativeIntegerPairTarget(F.Image, Function));
}

TEST(ObjCSourceProjection, NativeDependencyGraphKeepsSharedCallsAndCycles) {
  NativeDependencyFixture F;
  F.call(0, 0x3000);
  F.call(1, 0x3000);
  F.call(2, 0x1000);
  // An indirect operand is not an authenticated edge, even if constant.
  F.call(2, 0x4000, true);
  NativeSourceDependencyEvidence Evidence;
  const auto Targets = walkObjCNativeDependencies(F.Image, F.Result, &Evidence);
  EXPECT_EQ(Targets, (std::set<va_t>{0x1000, 0x3000}));
  EXPECT_EQ(Evidence.Roots, (std::set<va_t>{0x1000, 0x2000}));
  EXPECT_TRUE(Evidence.InventoryComplete);
  EXPECT_FALSE(Evidence.TargetsComplete);
  ASSERT_EQ(Evidence.Calls.size(), 4U);
  size_t SharedCallers = 0;
  for (const auto &Call : Evidence.Calls) {
    if (Call.Target == 0x3000)
      ++SharedCallers;
    if (Call.Indirect)
      EXPECT_EQ(Call.Target, 0U);
  }
  EXPECT_EQ(SharedCallers, 2U);
  PipelineOptions Options;
  std::map<va_t, std::string> Diagnostics;
  EXPECT_EQ(
      inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics), 0U);
  EXPECT_TRUE(Options.SourceTypeHints.empty());
  ASSERT_EQ(Diagnostics.size(), Targets.size());
  for (const auto &[Entry, Reason] : Diagnostics) {
    EXPECT_TRUE(Targets.count(Entry));
    EXPECT_FALSE(Reason.empty());
  }
  auto JSON = nativeSourceDependencyEvidenceJSON(Evidence);
  EXPECT_EQ(JSON.getBoolean("inventory_complete"), true);
  EXPECT_EQ(JSON.getBoolean("targets_complete"), false);
  ASSERT_EQ(JSON.getArray("calls")->size(), 4U);
  for (const auto &Call : *JSON.getArray("calls"))
    if (*Call.getAsObject()->getBoolean("indirect"))
      EXPECT_EQ(*Call.getAsObject()->get("target_address"),
                llvm::json::Value(nullptr));
}

TEST(ObjCSourceProjection,
     AmbiguousSelectorBodiesRemainSeparateNativeDependencyRoots) {
  NativeDependencyFixture F;
  F.Image.ObjCMethods[0].Status = "ambiguous_dispatch";
  F.Image.ObjCMethods[1].Status = "conflicting_encoding";
  NativeSourceDependencyEvidence Evidence;
  walkObjCNativeDependencies(F.Image, F.Result, &Evidence);
  EXPECT_EQ(Evidence.Roots, (std::set<va_t>{0x1000}));
  F.Image.ObjCMethods[0].TypeHint.reset();
  walkObjCNativeDependencies(F.Image, F.Result, &Evidence);
  EXPECT_TRUE(Evidence.Roots.empty());
}

TEST(ObjCSourceProjection, NativeInferenceSkipsCallOnlyThunkTargets) {
  NativeDependencyFixture F;
  F.call(0, 0x3000);
  PipelineOptions Options;
  std::map<va_t, std::string> Diagnostics;
  const std::set<va_t> CallOnlyTargets{0x3000};
  EXPECT_EQ(inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics,
                                        {}, CallOnlyTargets),
            0U);
  EXPECT_TRUE(Options.SourceTypeHints.empty());
  EXPECT_FALSE(Diagnostics.count(0x3000));
}

TEST(ObjCSourceProjection, SpecializedArrayCastInferenceRequiresCompleteAudit) {
  NativeDependencyFixture F;
  F.Image.Arch = Arch::AArch64;
  F.Image.Format = BinaryFormat::MachO;
  F.Image.Bits = Bitness::Bits64;
  F.Image.Symbols.push_back(
      {"_$ss15_arrayForceCastySayq_GSayxGr0_lF10Foundation3URLV_AFSgTg5",
       0x3000, 0, true});
  F.call(0, 0x3000);
  MedFunc Med;
  Med.Entry = 0x3000;
  F.Result.MedFuncs.push_back(Med);
  HighFunc High;
  High.Entry = 0x3000;
  F.Result.HighFuncs.push_back(High);
  PipelineFunctionAudit Audit;
  Audit.Entry = 0x3000;
  Audit.Disposition = PipelineFunctionDisposition::Accepted;
  Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  Audit.DecodedInstructions = Audit.LiftedInstructions = 1;
  F.Result.FunctionAudits.push_back(Audit);

  PipelineOptions Options;
  std::map<va_t, std::string> Diagnostics;
  EXPECT_EQ(
      inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics), 1U);
  ASSERT_TRUE(Options.SourceTypeHints.count(0x3000));
  const auto &Hint = Options.SourceTypeHints.at(0x3000);
  ASSERT_EQ(Hint.Parameters.size(), 1U);
  EXPECT_EQ(Hint.Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint.ReturnLocation.RegisterOffset, a64reg::X0);
  // Inference schedules re-lifting. It does not publish the old untyped body.
  EXPECT_FALSE(F.Result.HighFuncs[0].SourceTypeHint);
  EXPECT_EQ(
      inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics), 0U);

  for (unsigned Case = 0; Case < 6; ++Case) {
    SCOPED_TRACE(Case);
    F.Result.FunctionAudits[0] = Audit;
    auto &Broken = F.Result.FunctionAudits[0];
    switch (Case) {
    case 0:
      Broken.Disposition = PipelineFunctionDisposition::RejectedLowIR;
      break;
    case 1:
      Broken.HasLowIR = false;
      break;
    case 2:
      Broken.HasMedIR = false;
      break;
    case 3:
      Broken.MedIRVerified = false;
      break;
    case 4:
      Broken.LiftedInstructions = 0;
      break;
    case 5:
      Broken.DecodedInstructions = 0;
      break;
    }
    PipelineOptions Invalid;
    Diagnostics.clear();
    EXPECT_EQ(
        inferObjCNativeDependencies(F.Image, F.Result, Invalid, Diagnostics),
        0U);
    EXPECT_TRUE(Invalid.SourceTypeHints.empty());
  }
}

TEST(ObjCSourceProjection, NativeInferenceUsesSourceBoundRefinementBodies) {
  NativeDependencyFixture F;
  F.Image.Arch = Arch::AArch64;
  F.call(0, 0x3000);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.ReturnType = NdType::makeInt(4, false);
  Hint.Parameters = {{"native_arg0", NdType::makeInt(8, false)},
                     {"native_arg1", NdType::makeInt(8, false)}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Arch::AArch64, Error)) << Error;
  HighFunc Function;
  Function.Entry = 0x3000;
  Function.ReturnType = Hint.ReturnType;
  Function.SourceTypeHint = Hint;
  for (const auto &Parameter : Hint.Parameters)
    Function.Params.push_back({Parameter.Name, Parameter.Type});
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(0, 4);
  Function.Body.push_back(Return);
  F.Result.HighFuncs.push_back(Function);
  PipelineFunctionAudit Audit;
  Audit.Entry = Function.Entry;
  Audit.Disposition = PipelineFunctionDisposition::Accepted;
  Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  Audit.DecodedInstructions = Audit.LiftedInstructions = 1;
  F.Result.FunctionAudits.push_back(Audit);

  auto Bound = Function;
  MedVar ParameterValue;
  ParameterValue.Kind = MedVar::Param;
  ParameterValue.TheArch = Arch::AArch64;
  ParameterValue.Id = 1;
  ParameterValue.Size = 8;
  auto Parameter = HighExpr::makeVar(ParameterValue, NdType::makeInt(8, false));
  auto Prefix =
      HighExpr::makeBinop(NdOp::SUBBYTES, Parameter, HighExpr::makeConst(0, 4));
  Prefix->Type = NdType::makeInt(4, false);
  HighStmt Use;
  Use.Kind = StmtKind::Assign;
  MedVar Local;
  Local.Kind = MedVar::Temp;
  Local.Id = 9;
  Local.Size = 4;
  Local.TheArch = Arch::AArch64;
  Use.Dst = HighExpr::makeVar(Local, NdType::makeInt(4, false));
  Use.Val = Prefix;
  Bound.Body.insert(Bound.Body.begin(), std::move(Use));

  PipelineOptions Options;
  Options.SourceTypeHints.emplace(Function.Entry, Hint);
  std::map<va_t, HighFunc> Refinements{{Function.Entry, std::move(Bound)}};
  std::map<va_t, std::string> Diagnostics;
  EXPECT_EQ(inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics,
                                        {}, {}, &Refinements),
            1U);
  ASSERT_EQ(Options.SourceTypeHints.at(Function.Entry).Parameters.size(), 2U);
  EXPECT_EQ(Options.SourceTypeHints.at(Function.Entry).Parameters[1].Type->Size,
            4U);
  EXPECT_EQ(inferObjCNativeDependencies(F.Image, F.Result, Options, Diagnostics,
                                        {}, {}, &Refinements),
            0U);
}

TEST(ObjCSourceProjection, NativeDependencyGraphTracksMissingAndFinalEvidence) {
  NativeDependencyFixture F;
  F.call(0, 0x3000);
  F.call(2, 0x4000);
  NativeSourceDependencyEvidence Evidence;
  walkObjCNativeDependencies(F.Image, F.Result, &Evidence);
  EXPECT_FALSE(Evidence.InventoryComplete);
  EXPECT_FALSE(Evidence.TargetsComplete);
  EXPECT_EQ(Evidence.MissingFunctions, (std::set<va_t>{0x4000}));
  LowFunc Added;
  Added.Entry = 0x4000;
  F.Result.LowFuncs.push_back(std::move(Added));
  walkObjCNativeDependencies(F.Image, F.Result, &Evidence);
  EXPECT_TRUE(Evidence.InventoryComplete);
  EXPECT_TRUE(Evidence.TargetsComplete);
  EXPECT_TRUE(Evidence.MissingFunctions.empty());
  EXPECT_EQ(Evidence.Calls.size(), 2U);
  BinaryImage Other;
  EXPECT_THROW(walkObjCNativeDependencies(Other, F.Result, &Evidence),
               std::invalid_argument);
}

} // namespace

TEST(ObjCSourceProjection,
     OrdinarySynchronizationAnnotationsDoNotInventHandlers) {
  for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
    Projection P;
    auto &Metadata = P.Func.ExceptionMetadata.emplace();
    Metadata.Encoding = ExceptionEncoding::CompactUnwind;
    Metadata.Compact.emplace();
    auto &ObjC = Metadata.ObjC.emplace();
    ObjC.RuntimeCalls = {
        {0x1000, 0x2000, "objc_sync_enter", ObjCRuntimeCallKind::SyncEnter},
        {0x1004, 0x2004, "objc_sync_exit", ObjCRuntimeCallKind::SyncExit},
        {0x1008, 0x2008, "objc_release", ObjCRuntimeCallKind::ARCCleanup}};
    if (Mutation == 1)
      ObjC.LandingPads.emplace_back();
    if (Mutation == 2)
      ObjC.UsesFragileSetjmp = true;
    if (Mutation == 3)
      ObjC.UsesMSVCTables = true;
    if (Mutation == 4)
      ObjC.Runtime = ObjCRuntimeKind::GNU;
    if (Mutation == 5)
      ObjC.RuntimeCalls[0].Kind = ObjCRuntimeCallKind::Throw;
    if (Mutation == 6)
      ObjC.RuntimeCalls[0].Kind = ObjCRuntimeCallKind::BeginCatch;
    if (Mutation == 7)
      ObjC.RuntimeCalls[0].Kind = ObjCRuntimeCallKind::EndCatch;
    if (Mutation == 8)
      Metadata.Compact->HasLSDA = true;
    if (Mutation == 9)
      Metadata.PersonalityVA = 0x2000;
    EXPECT_EQ(P.limitation().empty(), Mutation == 0)
        << Mutation << ':' << P.limitation();
  }
}

TEST(ObjCSourceProjection, SynchronizedCleanupRequiresExactUnwindPad) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = 0x70;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.assign(Code.Size, 0);
  auto Put = [&](va_t Address, uint32_t Word) {
    llvm::support::endian::write32le(Code.Data.data() + Address - Code.VA,
                                     Word);
  };
  auto BL = [&](va_t From, va_t To) {
    Put(From, 0x94000000U | ((To - From) / 4));
  };
  Put(0x1010, 0xf90007e0U); // str x0, [sp, #8]
  Put(0x1018, 0xf94007e0U); // ldr x0, [sp, #8]
  BL(0x101c, 0x2000);       // objc_sync_enter
  Put(0x1034, 0xf94007e0U);
  Put(0x1038, 0xf9400400U);
  BL(0x103c, 0x2030); // unprotected dispatch_group_enter
  Put(0x1040, 0xf94007e0U);
  BL(0x1044, 0x2010); // normal objc_sync_exit
  Put(0x105c, 0xaa0003f3U);
  Put(0x1060, 0xf94007e0U);
  BL(0x1064, 0x2010); // exceptional objc_sync_exit
  Put(0x1068, 0xaa1303e0U);
  BL(0x106c, 0x2020); // _Unwind_Resume
  Image.Segments.push_back(std::move(Code));
  for (const auto &[Address, Name] :
       {std::pair<va_t, const char *>{0x2000, "_objc_sync_enter"},
        {0x2010, "_objc_sync_exit"},
        {0x2020, "__Unwind_Resume"},
        {0x2030, "_dispatch_group_enter"}}) {
    auto Symbol = Symbol::makeFunc(Address);
    Symbol.Name = Name;
    Image.Symbols.push_back(std::move(Symbol));
  }
  HighFunc Function;
  Function.Entry = 0x1000;
  Function.Params.push_back({"objc_self", NdType::makePtr(NdType::makeVoid())});
  auto &EH = Function.ExceptionMetadata.emplace();
  EH.CodeRange = {0x1000, 0x1070};
  EH.Personality = ExceptionPersonality::ObjCPersonalityV0;
  EH.PersonalityName = "__objc_personality_v0";
  auto &LSDA = EH.Itanium.emplace();
  LSDA.CallSites.resize(3);
  LSDA.CallSites[0].GuardedRange = {0x1000, 0x1020};
  LSDA.CallSites[1].GuardedRange = {0x1020, 0x1034};
  LSDA.CallSites[1].LandingPadVA = 0x105c;
  LSDA.CallSites[2].GuardedRange = {0x1034, 0x1070};
  auto &ObjC = EH.ObjC.emplace();
  ObjC.LandingPads.push_back(
      {{0x1020, 0x1034}, 0x105c, ObjCPadKind::SynchronizedExit, {}});

  auto Proof = proveObjCSynchronizedReceiverCleanup(Image, Function);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->EnterCall, 0x101cU);
  EXPECT_EQ(Proof->GuardStopCall, 0x103cU);
  EXPECT_EQ(Proof->ExitCall, 0x1044U);
  LSDA.CallSites[1].FirstActionOffset = 0;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(Image, Function));
  LSDA.CallSites[1].FirstActionOffset.reset();
  Image.Segments[0].Data[0x1060 - 0x1000] ^= 1;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(Image, Function));
  Image.Segments[0].Data[0x1060 - 0x1000] ^= 1;
  Image.Segments[0].Data[0x103c - 0x1000] ^= 1;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(Image, Function));
}

TEST(ObjCSourceProjection, SynchronizedCleanupSourceRequiresExceptionFlags) {
  const char *Source =
      "#include <stdint.h>\n"
      "extern int32_t neverd_darwin_objc_sync_enter(void*);\n"
      "extern void neverd_darwin_dispatch_group_enter(void*);\n"
      "extern int32_t neverd_darwin_objc_sync_exit(void*);\n"
      "void neverd_objc_imp_1000(void* objc_self) {\n"
      "    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));\n"
      "    work();\n"
      "    neverd_darwin_dispatch_group_enter(objc_self);\n"
      "    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));\n"
      "}\n";
  const auto Result = addObjCSynchronizedReceiverCleanup(
      Source, ObjCSynchronizedSourceProof{0x101c, 0x103c, 0x1044});
  ASSERT_TRUE(Result);
  EXPECT_NE(Result->find("#ifndef __EXCEPTIONS"), std::string::npos);
  EXPECT_NE(Result->find("cleanup(neverd_objc_sync_cleanup)"),
            std::string::npos);
  EXPECT_NE(Result->find("neverd_objc_sync_guard = objc_self;"),
            std::string::npos);
  EXPECT_NE(Result->find("neverd_objc_sync_guard = 0;"), std::string::npos);
  EXPECT_LT(Result->find("neverd_objc_sync_guard = 0;"),
            Result->find("neverd_darwin_dispatch_group_enter(objc_self);"));
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(
      "void f(void) {}", ObjCSynchronizedSourceProof{}));
}

TEST(ObjCSourceProjection, ProvenSynchronizedPadIsOnlyRemovedAfterReturn) {
  const ObjCSynchronizedSourceProof Proof{0x101c, 0x103c, 0x1044, 0x105c,
                                          0x2020};
  HighFunc Function;
  HighStmt NormalReturn;
  NormalReturn.Kind = StmtKind::Return;
  NormalReturn.Addr = 0x1058;
  HighStmt PadLabel;
  PadLabel.Kind = StmtKind::Block;
  PadLabel.Addr = Proof.LandingPad;
  HighStmt PadLoad;
  PadLoad.Kind = StmtKind::Assign;
  PadLoad.Addr = Proof.LandingPad + 4;
  HighStmt PadExit = PadLoad;
  PadExit.Addr = Proof.LandingPad + 8;
  HighStmt PadResume = PadLoad;
  PadResume.Addr = Proof.LandingPad + 16;
  PadResume.Val = HighExpr::makeCall("__Unwind_Resume", 0x2020, {});
  HighStmt SyntheticReturn;
  SyntheticReturn.Kind = StmtKind::Return;
  Function.Body = {NormalReturn, PadLabel,  PadLoad,
                   PadExit,      PadResume, SyntheticReturn};
  HighFunc WithNormalEdge = Function;
  HighStmt GotoPad;
  GotoPad.Kind = StmtKind::Goto;
  GotoPad.GotoTarget = Proof.LandingPad;
  WithNormalEdge.Body.insert(WithNormalEdge.Body.begin(), GotoPad);
  EXPECT_FALSE(omitProvenObjCSynchronizedLandingPad(WithNormalEdge, Proof));
  WithNormalEdge = Function;
  WithNormalEdge.Body[1].Addr += 4;
  EXPECT_FALSE(omitProvenObjCSynchronizedLandingPad(WithNormalEdge, Proof));
  HighFunc WithValueReturn = Function;
  MedVar SyntheticValue;
  SyntheticValue.Kind = MedVar::Reg;
  SyntheticValue.Size = 8;
  WithValueReturn.Body.back().RetVal =
      HighExpr::makeVar(SyntheticValue, NdType::makeInt(8));
  EXPECT_TRUE(omitProvenObjCSynchronizedLandingPad(WithValueReturn, Proof));
  WithValueReturn = Function;
  WithValueReturn.Body.back().RetVal =
      HighExpr::makeCall("unexpected", 0x2030, {});
  EXPECT_FALSE(omitProvenObjCSynchronizedLandingPad(WithValueReturn, Proof));
  EXPECT_TRUE(omitProvenObjCSynchronizedLandingPad(Function, Proof));
  ASSERT_EQ(Function.Body.size(), 1U);
  EXPECT_EQ(Function.Body.front().Kind, StmtKind::Return);
}

TEST(ObjCSourceProjection, SynchronizedRegisterReceiverNeedsStableSelf) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Segment Code;
  Code.VA = 0x3000;
  Code.Size = 0x64;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.assign(Code.Size, 0);
  auto Put = [&](va_t Address, uint32_t Word) {
    llvm::support::endian::write32le(Code.Data.data() + Address - Code.VA,
                                     Word);
  };
  auto BL = [&](va_t From, va_t To) {
    Put(From, 0x94000000U | ((To - From) / 4));
  };
  Put(0x3000, 0xa9be4ff4U); // stp x20, x19, [sp, #-0x20]!
  Put(0x3004, 0xa9017bfdU); // stp x29, x30, [sp, #0x10]
  Put(0x3008, 0x910043fdU); // add x29, sp, #0x10
  Put(0x300c, 0xaa0003f3U); // mov x19, x0
  Put(0x3010, 0xd503201fU); // nop
  BL(0x3014, 0x4000);       // objc_retain
  Put(0x3018, 0xaa1303e0U);
  BL(0x301c, 0x4010); // objc_sync_enter
  Put(0x3020, 0xaa1303e0U);
  BL(0x3024, 0x4020); // protected Objective-C call
  Put(0x3028, 0xaa1303e0U);
  BL(0x302c, 0x4030); // normal objc_sync_exit
  Put(0x3050, 0xaa0003f4U);
  Put(0x3054, 0xaa1303e0U);
  BL(0x3058, 0x4030); // exceptional objc_sync_exit
  Put(0x305c, 0xaa1403e0U);
  BL(0x3060, 0x4040); // _Unwind_Resume
  Image.Segments.push_back(std::move(Code));
  for (const auto &[Address, Name] :
       {std::pair<va_t, const char *>{0x4000, "_objc_retain"},
        {0x4010, "_objc_sync_enter"},
        {0x4020, "_objc_msgSend$cancelInternal"},
        {0x4030, "_objc_sync_exit"},
        {0x4040, "__Unwind_Resume"}}) {
    auto Symbol = Symbol::makeFunc(Address);
    Symbol.Name = Name;
    Image.Symbols.push_back(std::move(Symbol));
  }
  HighFunc Function;
  Function.Entry = 0x3000;
  Function.Params.push_back({"objc_self", NdType::makePtr(NdType::makeVoid())});
  auto &EH = Function.ExceptionMetadata.emplace();
  EH.CodeRange = {0x3000, 0x3064};
  EH.Personality = ExceptionPersonality::ObjCPersonalityV0;
  auto &LSDA = EH.Itanium.emplace();
  LSDA.CallSites.resize(3);
  LSDA.CallSites[0].GuardedRange = {0x3000, 0x3020};
  LSDA.CallSites[1].GuardedRange = {0x3020, 0x3028};
  LSDA.CallSites[1].LandingPadVA = 0x3050;
  LSDA.CallSites[2].GuardedRange = {0x3028, 0x3064};
  auto &ObjC = EH.ObjC.emplace();
  ObjC.LandingPads.push_back(
      {{0x3020, 0x3028}, 0x3050, ObjCPadKind::SynchronizedExit, {}});

  const auto Proof = proveObjCSynchronizedReceiverCleanup(Image, Function);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->EnterCall, 0x301cU);
  EXPECT_EQ(Proof->GuardStopCall, 0x302cU);
  EXPECT_EQ(Proof->ExitCall, 0x302cU);
  llvm::support::endian::write32le(Image.Segments[0].Data.data() + 0x24,
                                   0xaa0003f3U); // overwrite x19
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(Image, Function));
  const auto Rewrite = [&](va_t Address, uint32_t Word) {
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + Address - 0x3000, Word);
  };
  Rewrite(0x3024, 0x94000000U | ((0x4020 - 0x3024) / 4));
  Rewrite(0x3028, 0xaa0003f4U); // preserve the protected call result
  Rewrite(0x302c, 0xaa1503e0U); // release x21 outside the protected range
  Rewrite(0x3030, 0x94000000U | ((0x4050 - 0x3030) / 4));
  Rewrite(0x3034, 0xaa1303e0U);
  Rewrite(0x3038, 0x94000000U | ((0x4030 - 0x3038) / 4));
  auto Release = Symbol::makeFunc(0x4050);
  Release.Name = "_objc_release";
  Image.Symbols.push_back(std::move(Release));
  const auto ReleasedProof =
      proveObjCSynchronizedReceiverCleanup(Image, Function);
  ASSERT_TRUE(ReleasedProof);
  EXPECT_EQ(ReleasedProof->UnprotectedReleases, 1U);
  EXPECT_EQ(ReleasedProof->GuardStopCall, 0x3030U);
  EXPECT_EQ(ReleasedProof->ExitCall, 0x3038U);
  Rewrite(0x3028, 0xaa1603e0U);
  Rewrite(0x302c, 0x94000000U | ((0x4050 - 0x302c) / 4));
  Rewrite(0x3030, 0xaa1503e0U);
  Rewrite(0x3034, 0x94000000U | ((0x4050 - 0x3034) / 4));
  Rewrite(0x3038, 0xaa1303e0U);
  Rewrite(0x303c, 0x94000000U | ((0x4030 - 0x303c) / 4));
  const auto TwoReleases =
      proveObjCSynchronizedReceiverCleanup(Image, Function);
  ASSERT_TRUE(TwoReleases);
  EXPECT_EQ(TwoReleases->UnprotectedReleases, 2U);
  EXPECT_EQ(TwoReleases->GuardStopCall, 0x302cU);
  EXPECT_EQ(TwoReleases->ExitCall, 0x303cU);
  Rewrite(0x3014, 0x94000000U | ((0x4060 - 0x3014) / 4));
  Rewrite(0x3018, 0xaa0003f3U); // save the retained lock, not objc_self
  auto RetainedLock = Symbol::makeFunc(0x4060);
  RetainedLock.Name = "_objc_retainAutoreleasedReturnValue";
  Image.Symbols.push_back(std::move(RetainedLock));
  const auto LocalReceiver =
      proveObjCSynchronizedReceiverCleanup(Image, Function);
  ASSERT_TRUE(LocalReceiver);
  EXPECT_TRUE(LocalReceiver->ReceiverIsSavedLocal);
}

struct SynchronizedBranchFixture {
  BinaryImage Image;
  HighFunc Function;

  void put(va_t Address, uint32_t Word) {
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + Address - 0x3000, Word);
  }

  void call(va_t Address, va_t Target) {
    put(Address, 0x94000000U | ((Target - Address) / 4));
  }

  static uint32_t conditional(va_t Address, va_t Target, uint32_t Opcode) {
    return Opcode | (((Target - Address) / 4 & 0x7ffffU) << 5);
  }

  SynchronizedBranchFixture() {
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    Segment Code;
    Code.VA = 0x3000;
    Code.Size = 0xb4;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data.assign(Code.Size, 0);
    Image.Segments.push_back(std::move(Code));
    for (va_t Address = 0x3000; Address < 0x30b4; Address += 4)
      put(Address, 0xd503201fU);
    put(0x3000, 0xa9bd57f6U);
    put(0x3004, 0xa9014ff4U);
    put(0x3008, 0xa9027bfdU);
    put(0x300c, 0x910083fdU);
    put(0x3010, 0xaa0203f4U); // saved method argument
    put(0x3014, 0xaa0003f3U); // stable objc_self in x19
    put(0x3018, 0xaa0203e0U);
    call(0x301c, 0x4000);
    put(0x3020, conditional(0x3020, 0x3030, 0xb5000014U));
    put(0x3024, 0xaa1303e0U);
    call(0x3028, 0x4020);
    put(0x302c, 0xaa0003f4U);
    put(0x3030, 0xaa1303e0U);
    call(0x3034, 0x4000);
    put(0x3038, 0xaa1303e0U);
    call(0x303c, 0x4010);
    put(0x3040, 0xaa0203f5U); // non-throwing lock-body preparation
    put(0x3048, conditional(0x3048, 0x3058, 0xb4000014U));
    put(0x304c, 0xaa1303e0U);
    call(0x3050, 0x4020);
    put(0x3054, 0x14000004U); // both protected paths join at 0x3064
    put(0x3058, 0xaa1303e0U);
    call(0x305c, 0x4020);
    put(0x3064, 0xaa1503e0U);
    call(0x3068, 0x4030); // unprotected ARC release
    put(0x306c, 0xaa1303e0U);
    call(0x3070, 0x4040);
    put(0x3074, 0xaa1303e0U);
    call(0x3078, 0x4030);
    put(0x307c, 0xd65f03c0U);
    put(0x30a0, 0xaa0003f4U);
    put(0x30a4, 0xaa1303e0U);
    call(0x30a8, 0x4040);
    put(0x30ac, 0xaa1403e0U);
    call(0x30b0, 0x4050);
    for (const auto &[Address, Name] :
         {std::pair<va_t, const char *>{0x4000, "_objc_retain"},
          {0x4010, "_objc_sync_enter"},
          {0x4020, "_objc_msgSend$work"},
          {0x4030, "_objc_release"},
          {0x4040, "_objc_sync_exit"},
          {0x4050, "__Unwind_Resume"}}) {
      auto Symbol = Symbol::makeFunc(Address);
      Symbol.Name = Name;
      Image.Symbols.push_back(std::move(Symbol));
    }
    Function.Entry = 0x3000;
    Function.Params.push_back(
        {"objc_self", NdType::makePtr(NdType::makeVoid())});
    auto &EH = Function.ExceptionMetadata.emplace();
    EH.CodeRange = {0x3000, 0x30b4};
    EH.Personality = ExceptionPersonality::ObjCPersonalityV0;
    auto &LSDA = EH.Itanium.emplace();
    LSDA.CallSites.resize(3);
    LSDA.CallSites[0].GuardedRange = {0x3000, 0x3048};
    LSDA.CallSites[1].GuardedRange = {0x3048, 0x3064};
    LSDA.CallSites[1].LandingPadVA = 0x30a0;
    LSDA.CallSites[2].GuardedRange = {0x3064, 0x30b4};
    EH.ObjC.emplace().LandingPads.push_back(
        {{0x3048, 0x3064}, 0x30a0, ObjCPadKind::SynchronizedExit, {}});
  }

  void rejects(va_t Address, uint32_t Word) {
    const uint32_t Original = *objcSynchronizedWord(Image, Address);
    put(Address, Word);
    EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(Image, Function))
        << Address << ':' << Word;
    put(Address, Original);
  }
};

TEST(ObjCSourceProjection, SynchronizedForwardBranchesConvergeBeforeUnlock) {
  SynchronizedBranchFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->EnterCall, 0x303cU);
  EXPECT_EQ(Proof->GuardStopCall, 0x3068U);
  EXPECT_EQ(Proof->ExitCall, 0x3070U);
  EXPECT_FALSE(Proof->ReceiverIsSavedLocal);

  F.rejects(0x3048, F.conditional(0x3048, 0x3048, 0xb4000014U));
  F.rejects(0x3054, 0x14000007U); // skip the normal unlock
  F.rejects(0x3054, 0x14000013U); // enter the exceptional pad normally
  F.rejects(0x3054, 0xd61f0100U); // indirect jump
  F.rejects(0x3054, 0xd65f03c0U); // early return while locked
  F.rejects(0x3020, F.conditional(0x3020, 0x3040, 0xb5000014U));
  F.rejects(0x3020, F.conditional(0x3020, 0x3014, 0xb5000014U));
  F.rejects(0x3010, 0x14000008U); // bypass capture of objc_self
  F.rejects(0x3050, 0x2a0003f3U); // write the low half of the lock
  F.rejects(0x3040, 0x94000000U | ((0x4020 - 0x3040) / 4));

  for (const auto Word :
       {F.conditional(0x3048, 0x3058, 0x54000000U), // b.eq
        F.conditional(0x3048, 0x3058, 0x35000014U), // cbnz w20
        uint32_t{0x36180094U},                      // tbz w20, #3
        uint32_t{0x37180094U}}) {                   // tbnz w20, #3
    F.put(0x3048, Word);
    EXPECT_TRUE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  }
  F.Function.ExceptionMetadata->Itanium->CallSites[1].GuardedRange.Begin += 2;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
}

TEST(ObjCSourceProjection, SynchronizedUnprotectedRetainStopsCleanupFirst) {
  SynchronizedBranchFixture F;
  F.put(0x3064, 0xaa1403e0U);
  F.call(0x3068, 0x4000);
  F.put(0x306c, 0xaa1503e0U);
  F.call(0x3070, 0x4030);
  F.put(0x3074, 0xaa1303e0U);
  F.call(0x3078, 0x4040);
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->GuardStopCall, 0x3068U);
  EXPECT_EQ(Proof->ExitCall, 0x3078U);
  EXPECT_EQ(Proof->UnprotectedReleases, 1U);
  EXPECT_EQ(Proof->UnprotectedRetains, 1U);
  const std::string Source =
      "void neverd_objc_imp_3000(void* objc_self, int path) {\n"
      "    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));\n"
      "    if (path) work(); else alternate_work();\n"
      "    (uint64_t)(uintptr_t)(objc_retain(objc_self));\n"
      "    objc_release(v2);\n"
      "    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));\n"
      "}\n";
  const auto Result = addObjCSynchronizedReceiverCleanup(Source, *Proof);
  ASSERT_TRUE(Result);
  EXPECT_LT(Result->find("neverd_objc_sync_guard = 0;"),
            Result->find("objc_retain(objc_self)"));
  auto WrappedRetain = Source;
  WrappedRetain.replace(WrappedRetain.find("(uint64_t)(uintptr_t)("),
                        std::string("(uint64_t)(uintptr_t)(").size(),
                        "unexpected(");
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(WrappedRetain, *Proof));
  auto ExtraRetain = Source;
  ExtraRetain.insert(ExtraRetain.find("    objc_release(v2);"),
                     "    objc_retain(objc_self);\n");
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(ExtraRetain, *Proof));
  F.rejects(0x3050, 0x94000000U | ((0x4000 - 0x3050) / 4));
}

TEST(ObjCSourceProjection, SynchronizedResultMayBeSavedInX22) {
  SynchronizedBranchFixture F;
  F.put(0x3064, 0xaa0003f6U); // retain the result in x22 across the unlock
  F.put(0x3068, 0xaa1303e0U);
  F.call(0x306c, 0x4040);
  F.put(0x3070, 0xaa1303e0U);
  F.call(0x3074, 0x4030);
  F.put(0x3078, 0xd65f03c0U);
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->GuardStopCall, 0x306cU);
  EXPECT_EQ(Proof->ExitCall, 0x306cU);
  EXPECT_EQ(Proof->UnprotectedReleases, 0U);
  F.rejects(0x3064, 0xaa0003f3U); // saving a result must not overwrite the lock
}

TEST(ObjCSourceProjection, SynchronizedRegisterAllocationKeepsOneReceiver) {
  for (unsigned Register : {20U, 21U, 22U}) {
    SCOPED_TRACE(Register);
    SynchronizedBranchFixture F;
    const uint32_t Save = 0xaa0003e0U | Register;
    const uint32_t Argument = 0xaa0003e0U | (Register << 16);
    F.put(0x3014, Save);
    F.put(0x302c, 0xd503201fU);
    for (const va_t Address :
         {0x3024, 0x3030, 0x3038, 0x304c, 0x3058, 0x306c, 0x3074, 0x30a4})
      F.put(Address, Argument);
    F.put(0x3040, 0xd503201fU);
    F.put(0x30a0,
          0xaa0003f3U); // exception in x19, receiver in another register
    F.put(0x30ac, 0xaa1303e0U);
    const auto Proof =
        proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
    ASSERT_TRUE(Proof);
    EXPECT_FALSE(Proof->ReceiverIsSavedLocal);
    EXPECT_EQ(Proof->EnterCall, 0x303cU);
    EXPECT_EQ(Proof->ExitCall, 0x3070U);
    F.rejects(0x3050, Save);
    F.rejects(0x302c, Save);
    F.rejects(0x3050, 0x2a0003e0U | Register); // low-half overwrite
    F.rejects(0x30a0, Save);                   // exception overwrites the lock
    F.rejects(0x30a4, 0xaa1803e0U);            // exceptional unlock uses x24
    F.rejects(0x30ac, Argument); // resumes with receiver, not exception
  }
}

TEST(ObjCSourceProjection, SynchronizedRetainedLocalKeepsItsRegisterIdentity) {
  for (unsigned Register : {20U, 21U, 22U}) {
    SCOPED_TRACE(Register);
    SynchronizedBranchFixture F;
    auto Retain = Symbol::makeFunc(0x4060);
    Retain.Name = "_objc_retainAutoreleasedReturnValue";
    F.Image.Symbols.push_back(std::move(Retain));
    F.call(0x3034, 0x4060);
    F.put(0x3038, 0xaa0003e0U | Register);
    F.put(0x3040, 0xd503201fU);
    const uint32_t Argument = 0xaa0003e0U | (Register << 16);
    F.put(0x306c, Argument);
    F.put(0x30a4, Argument);
    F.put(0x30a0, 0xaa0003f3U);
    F.put(0x30ac, 0xaa1303e0U);
    const auto Proof =
        proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
    ASSERT_TRUE(Proof);
    EXPECT_TRUE(Proof->ReceiverIsSavedLocal);
    F.rejects(0x3020, F.conditional(0x3020, 0x3038, 0xb5000014U));
    F.rejects(0x3034, 0x94000000U | ((0x4020 - 0x3034) / 4));
    F.rejects(0x3040, 0xaa0003e0U | Register);
  }
}

TEST(ObjCSourceProjection, SynchronizedCExecutesBranchesAndUnwindAtO0AndO2) {
  SynchronizedBranchFixture F;
  F.put(0x3064, 0xaa1403e0U);
  F.call(0x3068, 0x4000);
  F.put(0x306c, 0xaa1503e0U);
  F.call(0x3070, 0x4030);
  F.put(0x3074, 0xaa1303e0U);
  F.call(0x3078, 0x4040);
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  const std::string Source = R"C(
#include <stdint.h>
extern int32_t neverd_darwin_objc_sync_enter(void*);
extern int32_t neverd_darwin_objc_sync_exit(void*);
extern void work(void*, int);
extern void* objc_retain(void*);
extern void objc_release(void*);
extern void after(void);
void neverd_objc_imp_3000(void* objc_self, int path) {
    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));
    if (path) work(objc_self, 2); else work(objc_self, 3);
    (uint64_t)(uintptr_t)(objc_retain(objc_self));
    objc_release(objc_self);
    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));
    after();
}
)C";
  const auto Result = addObjCSynchronizedReceiverCleanup(Source, *Proof);
  ASSERT_TRUE(Result);
  const char *Harness = R"CPP(
#include <stdint.h>
static void *expected;
static int failure, order, bad, exits;
static void step(void *lock, int tag) {
    if (lock != expected) bad = 1;
    order = order * 10 + tag;
}
extern "C" int32_t neverd_darwin_objc_sync_enter(void *lock) {
    step(lock, 1); return 0;
}
extern "C" int32_t neverd_darwin_objc_sync_exit(void*)
    __asm__("_objc_sync_exit");
extern "C" int32_t neverd_darwin_objc_sync_exit(void *lock) {
    step(lock, 6); ++exits; return 0;
}
extern "C" void work(void *lock, int tag) {
    step(lock, tag); if (failure == 1) throw 1;
}
extern "C" void *objc_retain(void *lock) {
    step(lock, 4); if (failure == 2) throw 2; return lock;
}
extern "C" void objc_release(void *lock) {
    step(lock, 5); if (failure == 3) throw 3;
}
extern "C" void after(void) {
    step(expected, 7); if (failure == 4) throw 4;
}
extern "C" void neverd_objc_imp_3000(void*, int);
int main() {
    for (int path = 0; path < 2; ++path) {
        expected = (void*)(uintptr_t)(0x1234 + path);
        for (failure = 0; failure <= 4; ++failure) {
            order = bad = exits = 0;
            int caught = 0;
            try { neverd_objc_imp_3000(expected, path); }
            catch (int exception) { caught = exception; }
            const int prefix = path ? 12 : 13;
            const int wanted = failure == 1 ? prefix * 10 + 6 :
                               failure == 2 ? prefix * 10 + 4 :
                               failure == 3 ? prefix * 100 + 45 :
                                              prefix * 10000 + 4567;
            const int unlocks = failure == 2 || failure == 3 ? 0 : 1;
            if (caught != failure || order != wanted || exits != unlocks || bad)
                return 10 + failure + path * 5;
        }
    }
    return 0;
}
)CPP";
  executeSynchronizedSource(*Result, Harness);
}

namespace {
struct SynchronizedRetainedStackFixture {
  BinaryImage Image;
  HighFunc Function;

  void put(va_t Address, uint32_t Word) {
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + Address - 0x3000, Word);
  }
  void call(va_t Address, va_t Target) {
    put(Address, 0x94000000U | ((Target - Address) / 4));
  }
  SynchronizedRetainedStackFixture() {
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    Segment Code;
    Code.VA = 0x3000;
    Code.Size = Code.FileSz = 0x3000;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data.assign(Code.Size, 0);
    Image.Segments.push_back(std::move(Code));
    for (const auto &[Address, Name] :
         {std::pair<va_t, const char *>{0x3000, "__text"},
          {0x4000, "__objc_stubs"},
          {0x5000, "__const"}}) {
      Section S;
      S.VA = Address;
      S.FileOff = Address - 0x3000;
      S.Size = S.FileSz = 0x1000;
      S.Name = Name;
      S.Flags = Address == 0x5000
                    ? SegmentFlags::Readable
                    : SegmentFlags::Readable | SegmentFlags::Executable;
      if (Address != 0x5000)
        S.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      Image.Sections.push_back(std::move(S));
    }
    Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
    for (const auto &[Slot, Name] :
         {std::pair<va_t, const char *>{0x5100, "_objc_msgSend"},
          {0x5108, "_objc_retainAutoreleasedReturnValue"},
          {0x5110, "_objc_release_x19"},
          {0x5118, "_objc_release"}}) {
      Image.ImportPtrSlots[Slot] = Name;
      Image.DyldBindSlots[Slot] = {Name, 0, "/usr/lib/libobjc.A.dylib", false};
    }
    const auto RuntimeStub = [&](va_t Address, va_t Slot) {
      put(Address, 0xb0000010U); // adrp x16, 0x5000
      put(Address + 4, 0xf9400210U | ((Slot - 0x5000) / 8 << 10));
      put(Address + 8, 0xd61f0200U);
    };
    for (const auto &[Address, Slot] :
         {std::pair<va_t, va_t>{0x4000, 0x5200}, {0x4040, 0x5210}}) {
      ObjCSourceReference R;
      R.Address = Slot;
      R.Size = 8;
      R.TheKind = ObjCSourceReference::Kind::Selector;
      R.Name = Slot == 0x5200 ? "cache" : "clear";
      Image.ObjCSourceReferences[Slot] = R;
      put(Address, 0xb0000001U); // adrp x1, 0x5000
      put(Address + 4, 0xf9400021U | ((Slot - 0x5000) / 8 << 10));
      RuntimeStub(Address + 8, 0x5100);
    }
    RuntimeStub(0x4100, 0x5108);
    RuntimeStub(0x4110, 0x5110);
    RuntimeStub(0x4120, 0x5118);
    for (const auto &[Address, Name] :
         {std::pair<va_t, const char *>{0x4200, "_objc_sync_enter"},
          {0x4210, "_objc_sync_exit"},
          {0x4220, "__Unwind_Resume"}}) {
      auto S = Symbol::makeFunc(Address);
      S.Name = Name;
      Image.Symbols.push_back(std::move(S));
    }
    for (const auto &[Offset, Word] :
         {std::pair<unsigned, uint32_t>{0x00, 0xd100c3ffU},
          {0x04, 0xa9014ff4U},
          {0x08, 0xa9027bfdU},
          {0x0c, 0x910083fdU},
          {0x10, 0xaa0003f3U},
          {0x18, 0xaa1d03fdU},
          {0x20, 0xf90007e0U},
          {0x28, 0xaa1303e0U},
          {0x30, 0xaa1d03fdU},
          {0x38, 0xaa0003f3U},
          {0x44, 0xf94007e0U},
          {0x4c, 0xf94007e0U},
          {0x50, 0xa9427bfdU},
          {0x54, 0xa9414ff4U},
          {0x58, 0x9100c3ffU},
          {0x60, 0xaa0003f3U},
          {0x64, 0xf94007e0U},
          {0x6c, 0xaa1303e0U}})
      put(0x3000 + Offset, Word);
    call(0x3014, 0x4000);
    call(0x301c, 0x4100);
    call(0x3024, 0x4200);
    call(0x302c, 0x4000);
    call(0x3034, 0x4100);
    call(0x303c, 0x4040);
    call(0x3040, 0x4110);
    call(0x3048, 0x4210);
    put(0x305c, 0x14000000U | ((0x4120 - 0x305c) / 4));
    call(0x3068, 0x4210);
    call(0x3070, 0x4220);
    Function.Entry = 0x3000;
    Function.Params = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                       {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
    auto &EH = Function.ExceptionMetadata.emplace();
    EH.CodeRange = {0x3000, 0x3074};
    EH.Personality = ExceptionPersonality::ObjCPersonalityV0;
    auto &Sites = EH.Itanium.emplace().CallSites;
    Sites.resize(3);
    Sites[0].GuardedRange = {0x3000, 0x3028};
    Sites[1].GuardedRange = {0x3028, 0x3040};
    Sites[1].LandingPadVA = 0x3060;
    Sites[2].GuardedRange = {0x3040, 0x3074};
    EH.ObjC.emplace().LandingPads.push_back(
        {{0x3028, 0x3040}, 0x3060, ObjCPadKind::SynchronizedExit, {}});
  }
};

const char *SynchronizedRetainedStackSource = R"C(
#include <stdint.h>
extern void *cache(void*);
extern void *objc_retainAutoreleasedReturnValue(void*);
extern int32_t neverd_darwin_objc_sync_enter(void*);
extern int32_t neverd_darwin_objc_sync_exit(void*);
extern void clear(void*);
extern void objc_release(void*);
void neverd_objc_imp_3000(void* objc_self, void* objc_cmd) {
    uint64_t var_m28, v1, v2, v3, v4;
    v1 = (uint64_t)(uintptr_t)cache(objc_self);
    v2 = (uint64_t)(uintptr_t)objc_retainAutoreleasedReturnValue((void*)(uintptr_t)v1);
    var_m28 = v2;
    (uint32_t)(neverd_darwin_objc_sync_enter((void*)(uintptr_t)(v2)));
    v3 = (uint64_t)(uintptr_t)cache(objc_self);
    v4 = (uint64_t)(uintptr_t)objc_retainAutoreleasedReturnValue((void*)(uintptr_t)v3);
    clear((void*)(uintptr_t)v4);
    objc_release((void*)(uintptr_t)(v4));
    (uint32_t)(neverd_darwin_objc_sync_exit((void*)(uintptr_t)(var_m28)));
    objc_release((void*)(uintptr_t)(var_m28));
}
)C";
} // namespace

TEST(ObjCSourceProjection, SynchronizedRetainedStackLockKeepsItsOwnIdentity) {
  SynchronizedRetainedStackFixture F;
  EXPECT_TRUE(objcSynchronizedSourceRegion(F.Image, F.Function));
  EXPECT_TRUE(objcSelectorStubPreservesNonvolatileRegisters(F.Image, 0x4000));
  EXPECT_TRUE(objcSelectorStubPreservesNonvolatileRegisters(F.Image, 0x4040));
  const auto Slot = darwinImportVeneerSlot(F.Image, 0x4100);
  ASSERT_TRUE(Slot);
  EXPECT_EQ(*Slot, 0x5108U);
  const auto Hint = objcRuntimeSourceCallHint(F.Image, *Slot);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->Signature.Parameters.front().Location.ValueBytes, 8U);
  EXPECT_EQ(Hint->Signature.Parameters.front().Location.Kind,
            SourceABICarrierKind::IntegerRegister);
  EXPECT_EQ(Hint->Signature.Parameters.front().Location.RegisterOffset,
            a64reg::X0);
  EXPECT_TRUE(objcSynchronizedRuntimeTargetIs(
      F.Image, 0x4100, "objc_retainAutoreleasedReturnValue", a64reg::X0));
  EXPECT_TRUE(objcSynchronizedRuntimeTargetIs(F.Image, 0x4110, "objc_release",
                                              a64reg::X19));
  EXPECT_TRUE(objcSynchronizedRuntimeTargetIs(F.Image, 0x4120, "objc_release",
                                              a64reg::X0));
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  EXPECT_TRUE(Proof->ReceiverIsSavedLocal);
  EXPECT_EQ(Proof->EnterCall, 0x3024U);
  EXPECT_EQ(Proof->GuardStopCall, 0x3040U);
  EXPECT_EQ(Proof->ExitCall, 0x3048U);
  for (const va_t Address : {0x3000, 0x3010, 0x3020, 0x3028, 0x3030, 0x3038,
                             0x3044, 0x3054, 0x3058, 0x3060, 0x3064, 0x306c}) {
    const auto Word = *objcSynchronizedWord(F.Image, Address);
    F.put(Address, Word ^ 1);
    EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
    F.put(Address, Word);
  }
  F.Image.DyldBindSlots[0x5110].WeakImport = true;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  F.Image.DyldBindSlots[0x5110].WeakImport = false;
  F.Image.DyldBindSlots[0x5110].Module = "/tmp/foreign.dylib";
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  F.Image.DyldBindSlots[0x5110].Module = "/usr/lib/libobjc.A.dylib";
  F.Image.ImportPtrSlots[0x5110] = "_objc_release_x20";
  F.Image.DyldBindSlots[0x5110].Name = "_objc_release_x20";
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
}

TEST(ObjCSourceProjection, SynchronizedStackCopyMustBeDefinedAndUnchanged) {
  SynchronizedRetainedStackFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  const std::string Source = SynchronizedRetainedStackSource;
  const auto Result = addObjCSynchronizedReceiverCleanup(Source, *Proof);
  ASSERT_TRUE(Result);
  EXPECT_NE(Result->find("neverd_objc_sync_guard = (void*)(uintptr_t)(v2);"),
            std::string::npos);
  EXPECT_LT(Result->find("neverd_objc_sync_guard = 0;"),
            Result->find("objc_release((void*)(uintptr_t)(v4))"));
  for (const char *Copy : {"var_m28 = v1;", "var_m28 = factory();", ""}) {
    auto Changed = Source;
    const auto At = Changed.find("var_m28 = v2;");
    Changed.replace(At, std::string("var_m28 = v2;").size(), Copy);
    EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(Changed, *Proof));
  }
  for (const char *Write : {"v2 = 0;", "var_m28 = 0;"}) {
    auto Changed = Source;
    Changed.insert(Changed.find("    clear("),
                   "    " + std::string(Write) + "\n");
    EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(Changed, *Proof));
    Changed = Source;
    Changed.insert(Changed.find("    (uint32_t)(neverd_darwin_objc_sync_enter"),
                   "    " + std::string(Write) + "\n");
    EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(Changed, *Proof));
  }
}

TEST(ObjCSourceProjection, SynchronizedStackLockExecutesOriginalARCAndUnlocks) {
  SynchronizedRetainedStackFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  const auto Source = addObjCSynchronizedReceiverCleanup(
      SynchronizedRetainedStackSource, *Proof);
  ASSERT_TRUE(Source);
  const char *Harness = R"CPP(
#include <stdint.h>
static void *self = (void*)(uintptr_t)0x1111;
static void *lock = (void*)(uintptr_t)0x2222;
static void *value = (void*)(uintptr_t)0x3333;
static int failure, order, bad, exits, queries;
static void step(void *object, void *expected, int tag) {
    if (object != expected) bad = 1;
    order = order * 10 + tag;
    if (tag == failure) throw tag;
}
extern "C" void *cache(void *object) {
    step(object, self, ++queries == 1 ? 1 : 4);
    return queries == 1 ? lock : value;
}
extern "C" void *objc_retainAutoreleasedReturnValue(void *object) {
    step(object, queries == 1 ? lock : value, queries == 1 ? 2 : 5);
    return object;
}
extern "C" int32_t neverd_darwin_objc_sync_enter(void *object) {
    step(object, lock, 3); return 0;
}
extern "C" int32_t neverd_darwin_objc_sync_exit(void*)
    __asm__("_objc_sync_exit");
extern "C" int32_t neverd_darwin_objc_sync_exit(void *object) {
    ++exits; step(object, lock, 8); return 0;
}
extern "C" void clear(void *object) { step(object, value, 6); }
extern "C" void objc_release(void *object) {
    step(object, object == lock ? lock : value, object == lock ? 9 : 7);
}
extern "C" void neverd_objc_imp_3000(void*, void*);
int main() {
    const int wanted[] = {123456789, 1, 12, 123, 12348, 123458,
                          1234568, 1234567, 12345678, 123456789};
    for (failure = 0; failure <= 9; ++failure) {
        order = bad = exits = queries = 0;
        int caught = 0;
        try { neverd_objc_imp_3000(self, 0); }
        catch (int exception) { caught = exception; }
        const int unlocks = failure == 0 || (failure >= 4 && failure != 7);
        if (caught != failure || order != wanted[failure] || bad ||
            exits != unlocks)
            return 10 + failure;
    }
    return 0;
}
)CPP";
  executeSynchronizedSource(*Source, Harness);
}

namespace {
struct SynchronizedInterleavedFixture : SynchronizedRetainedStackFixture {
  SynchronizedInterleavedFixture() {
    for (va_t Address = 0x3000; Address < 0x30a0; Address += 4)
      put(Address, 0xd503201fU);
    Image.ImportPtrSlots[0x5120] = "_objc_retain";
    Image.DyldBindSlots[0x5120] = {"_objc_retain", 0,
                                   "/usr/lib/libobjc.A.dylib", false};
    put(0x4130, 0xb0000010U);
    put(0x4134, 0xf9400210U | (0x120 / 8 << 10));
    put(0x4138, 0xd61f0200U);
    for (const auto &[Address, Word] :
         {std::pair<va_t, uint32_t>{0x3000, 0xa9bd57f6U},
          {0x3004, 0xa9014ff4U},
          {0x3008, 0xa9027bfdU},
          {0x300c, 0x910083fdU},
          {0x3010, 0xaa0203f4U},
          {0x3014, 0xaa0003f3U},
          {0x301c, 0xaa1303e0U},
          {0x3024, 0xaa1303e0U},
          {0x302c, 0xaa1d03fdU},
          {0x3034, 0xaa0003f5U},
          {0x3038, 0xaa1503e0U},
          {0x3040, 0xb4000135U}, // cbz x21, 0x3064
          {0x3044, 0xaa1303e0U},
          {0x304c, 0xaa1d03fdU},
          {0x3054, 0xaa0003f4U},
          {0x305c, 0xaa1403e0U},
          {0x3064, 0xaa1503e0U},
          {0x306c, 0xaa1303e0U},
          {0x3088, 0xd65f03c0U},
          {0x308c, 0xaa0003f4U},
          {0x3090, 0xaa1303e0U},
          {0x3098, 0xaa1403e0U}})
      put(Address, Word);
    call(0x3020, 0x4200);
    call(0x3028, 0x4000);
    call(0x3030, 0x4100);
    call(0x303c, 0x4120);
    call(0x3048, 0x4000);
    call(0x3050, 0x4100);
    call(0x3058, 0x4040);
    call(0x3060, 0x4120);
    call(0x3068, 0x4130);
    call(0x3070, 0x4210);
    call(0x3094, 0x4210);
    call(0x309c, 0x4220);
    auto &EH = *Function.ExceptionMetadata;
    EH.CodeRange = {0x3000, 0x30a0};
    auto &Sites = EH.Itanium->CallSites;
    Sites.assign(5, {});
    Sites[0].GuardedRange = {0x3000, 0x3024};
    Sites[1].GuardedRange = {0x3024, 0x3034};
    Sites[1].LandingPadVA = 0x308c;
    Sites[2].GuardedRange = {0x3034, 0x3044};
    Sites[3].GuardedRange = {0x3044, 0x305c};
    Sites[3].LandingPadVA = 0x308c;
    Sites[4].GuardedRange = {0x305c, 0x30a0};
    EH.ObjC->LandingPads = {
        {{0x3024, 0x3034}, 0x308c, ObjCPadKind::SynchronizedExit, {}},
        {{0x3044, 0x305c}, 0x308c, ObjCPadKind::SynchronizedExit, {}}};
  }
};

const char *SynchronizedInterleavedSource = R"C(
#include <stdint.h>
extern int32_t neverd_darwin_objc_sync_enter(void*);
extern int32_t neverd_darwin_objc_sync_exit(void*);
extern void work(void*, int);
extern void *objc_retain(void*);
extern void objc_release(void*);
extern void after(void);
uint64_t neverd_objc_imp_3000(void* objc_self, void *value, int path) {
    uint64_t result;
    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));
    work(value, 2);
    objc_release(value);
    if (path) {
        work(value, 4);
        objc_release(value);
    }
    result = (uint64_t)(uintptr_t)objc_retain(value);
    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));
    after();
    return result;
}
)C";
} // namespace

TEST(ObjCSourceProjection, SynchronizedInterleavedRangesPreserveEveryHole) {
  SynchronizedInterleavedFixture F;
  const auto Regions = objcSynchronizedInterleavedRanges(F.Image, F.Function);
  ASSERT_TRUE(Regions);
  ASSERT_EQ(Regions->size(), 2U);
  EXPECT_EQ(Regions->front().End, 0x3034U);
  EXPECT_EQ(Regions->back().Begin, 0x3044U);
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->EnterCall, 0x3020U);
  EXPECT_EQ(Proof->ExitCall, 0x3070U);
  EXPECT_EQ(Proof->LandingPad, 0x308cU);
  EXPECT_EQ(Proof->SuspendARC, 3U);
  auto &EH = *F.Function.ExceptionMetadata;
  const auto Sites = EH.Itanium->CallSites;
  const auto Pads = EH.ObjC->LandingPads;
  EH.Itanium->CallSites[2].GuardedRange.Begin += 4;
  EXPECT_TRUE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  EH.Itanium->CallSites[2].GuardedRange.Begin +=
      8; // omitted ARC call at 0x303c
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  EH.Itanium->CallSites = Sites;
  EH.Itanium->CallSites[2].FirstActionOffset = 0;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  EH.Itanium->CallSites = Sites;
  EH.Itanium->CallSites[3].LandingPadVA += 4;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  EH.Itanium->CallSites = Sites;
  EH.ObjC->LandingPads.pop_back();
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  EH.ObjC->LandingPads = Pads;
  EH.ObjC->LandingPads[1].GuardedRange.End -= 4;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
}

TEST(ObjCSourceProjection,
     SynchronizedInterleavedCleanupNeedsStableBoundaries) {
  for (const auto &[Address, Word] :
       {std::pair<va_t, uint32_t>{0x3018, 0x14000003U}, // skip enter load
        {0x3040, 0xb40001b5U},                          // skip normal unlock
        {0x3040, 0xb4fffe15U},                          // backwards jump
        {0x3024, 0x2a1f03f3U},    // overwrite low receiver bytes
        {0x3024, 0xd65f03c0U},    // return before unlock
        {0x3074, 0x17ffffecU},    // reenter protected body after unlock
        {0x3088, 0x14000001U},    // fall into cleanup pad normally
        {0x306c, 0xaa1403e0U},    // wrong normal receiver
        {0x3090, 0xaa1403e0U},    // wrong exceptional receiver
        {0x308c, 0xaa0003f3U}}) { // clobber lock with exception
    SynchronizedInterleavedFixture F;
    F.put(Address, Word);
    EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function))
        << llvm::utohexstr(Address);
  }
  for (const auto &[Address, Target] :
       {std::pair<va_t, va_t>{0x303c, 0x4000}, // unprotected non-ARC call
        {0x3028, 0x4120},                      // release is also protected
        {0x3030, 0x4130}}) {                   // retain is also protected
    SynchronizedInterleavedFixture F;
    F.call(Address, Target);
    EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  }
  SynchronizedInterleavedFixture F;
  F.Image.DyldBindSlots[0x5118].WeakImport = true;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  F.Image.DyldBindSlots[0x5118].WeakImport = false;
  F.Image.DyldBindSlots[0x5118].Module = "/tmp/foreign.dylib";
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
}

namespace {
struct SynchronizedForwardedPadFixture : SynchronizedInterleavedFixture {
  SynchronizedForwardedPadFixture() {
    put(0x3088, 0x14000001U); // a second exceptional entry: B 0x308c
    put(0x3084, 0xd65f03c0U); // normal return never enters either pad
    auto &EH = *Function.ExceptionMetadata;
    EH.Itanium->CallSites[1].LandingPadVA = 0x3088;
    EH.ObjC->LandingPads[0].PadVA = 0x3088;
    EH.ObjC->LandingPads[0].Kind = ObjCPadKind::Cleanup;
  }
};
} // namespace

TEST(ObjCSourceProjection, SynchronizedSparseLSDARequiresCompleteNonCallGaps) {
  SynchronizedForwardedPadFixture F;
  auto &EH = *F.Function.ExceptionMetadata;
  EH.Itanium->CallSites[2].GuardedRange = {0x3038, 0x303c};
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  EXPECT_TRUE(objcSynchronizedNonCallGap(F.Image, 0x3034, 0x3038));
  EXPECT_FALSE(objcSynchronizedNonCallGap(F.Image, 0x303c, 0x3044));
  // The second gap above contains an ARC call. A table must explicitly
  // describe it as unprotected rather than silently omit its unwind policy.
  EH.Itanium->CallSites[2].GuardedRange.End = 0x3040;
  ASSERT_TRUE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  for (uint32_t Word :
       {0x94000001U, 0xd63f0260U, 0xffffffffU, 0xd4000001U, 0xd69f03e0U}) {
    SynchronizedForwardedPadFixture Changed;
    Changed.Function.ExceptionMetadata->Itanium->CallSites[2].GuardedRange = {
        0x3038, 0x3040};
    Changed.put(0x3034, Word);
    EXPECT_FALSE(
        proveObjCSynchronizedReceiverCleanup(Changed.Image, Changed.Function));
  }
  // A decodable branch in an omitted interval is still subject to the
  // lifetime proof: backwards edges, skipped unlocks and pad entry fail.
  for (va_t Target : {0x3024U, 0x3074U, 0x3088U, 0x308cU}) {
    SynchronizedForwardedPadFixture Changed;
    Changed.Function.ExceptionMetadata->Itanium->CallSites[2].GuardedRange = {
        0x3038, 0x3040};
    Changed.put(0x3034, 0x14000000U | ((Target - 0x3034) / 4 & 0x3ffffffU));
    EXPECT_FALSE(
        proveObjCSynchronizedReceiverCleanup(Changed.Image, Changed.Function));
  }
  EXPECT_FALSE(objcSynchronizedNonCallGap(F.Image, 0x3035, 0x3038));
  EXPECT_FALSE(objcSynchronizedNonCallGap(F.Image, 0x3034, 0x3034));
  EXPECT_FALSE(objcSynchronizedNonCallGap(F.Image, 0x8000, 0x8004));
  EXPECT_FALSE(objcSynchronizedNonCallGap(F.Image, 0x3034, 0x7038));
}

TEST(ObjCSourceProjection,
     SynchronizedForwardedPadKeepsBothExceptionalEntries) {
  SynchronizedForwardedPadFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->LandingPad, 0x3088U);
  EXPECT_EQ(Proof->LandingPadPrelude, 4U);
  EXPECT_EQ(Proof->ResumeTarget, 0x4220U);
  EXPECT_EQ(Proof->SuspendARC, 3U);

  for (uint32_t Word : {0x14000000U, 0x14000002U, 0x17ffffffU, 0x54000020U,
                        0xd61f0260U, 0xd503201fU, 0xf90007e0U, 0xaa0003f3U}) {
    SynchronizedForwardedPadFixture Changed;
    Changed.put(0x3088, Word);
    EXPECT_FALSE(
        proveObjCSynchronizedReceiverCleanup(Changed.Image, Changed.Function));
  }
  for (va_t Target : {0x3088U, 0x308cU, 0x309cU}) {
    SynchronizedForwardedPadFixture Changed;
    Changed.put(0x3084, 0x14000000U | ((Target - 0x3084) / 4));
    EXPECT_FALSE(
        proveObjCSynchronizedReceiverCleanup(Changed.Image, Changed.Function));
  }
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SynchronizedForwardedPadFixture Changed;
    auto &EH = *Changed.Function.ExceptionMetadata;
    switch (Mutation) {
    case 0:
      EH.Itanium->CallSites[3].LandingPadVA += 4;
      break;
    case 1:
      EH.ObjC->LandingPads[0].Kind = ObjCPadKind::Catch;
      break;
    case 2:
      EH.ObjC->LandingPads[1].Kind = ObjCPadKind::Cleanup;
      break;
    case 3:
      EH.ObjC->LandingPads[0].Catches.emplace_back();
      break;
    case 4:
      EH.CodeRange.End += 4;
      break;
    case 5:
      Changed.put(0x3090, 0xaa1403e0U);
      break;
    case 6:
      Changed.put(0x3098, 0xaa1303e0U);
      break;
    case 7:
      Changed.call(0x3094, 0x4120);
      break;
    }
    EXPECT_FALSE(
        proveObjCSynchronizedReceiverCleanup(Changed.Image, Changed.Function))
        << Mutation;
  }
}

TEST(ObjCSourceProjection,
     SynchronizedForwardedPadOnlyOmitsPureUnreachableTail) {
  SynchronizedForwardedPadFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = 0x3084;
  HighStmt ForwardLabel;
  ForwardLabel.Kind = StmtKind::Block;
  ForwardLabel.Addr = 0x3088;
  HighStmt CleanupLabel = ForwardLabel;
  CleanupLabel.Addr = 0x308c;
  HighStmt Exit;
  Exit.Kind = StmtKind::Assign;
  Exit.Addr = 0x3094;
  Exit.Val = HighExpr::makeCall("_objc_sync_exit", 0x4210, {});
  HighStmt Resume = Exit;
  Resume.Addr = 0x309c;
  Resume.Val = HighExpr::makeCall("__Unwind_Resume", 0x4220, {});
  HighStmt SyntheticReturn;
  SyntheticReturn.Kind = StmtKind::Return;
  F.Function.Body = {Return, ForwardLabel, CleanupLabel,
                     Exit,   Resume,       SyntheticReturn};
  for (va_t Target : {0x3088U, 0x308cU, 0x309cU}) {
    auto Changed = F.Function;
    HighStmt Goto;
    Goto.Kind = StmtKind::Goto;
    Goto.GotoTarget = Target;
    Changed.Body.insert(Changed.Body.begin(), Goto);
    EXPECT_FALSE(omitProvenObjCSynchronizedLandingPad(Changed, *Proof));
  }
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    auto Changed = F.Function;
    switch (Mutation) {
    case 0:
      Changed.Body[1].Val = HighExpr::makeCall("work", 0x4000, {});
      break;
    case 1:
      Changed.Body[2].Val = HighExpr::makeCall("work", 0x4000, {});
      break;
    case 2:
      Changed.Body[2].Body.push_back(Exit);
      break;
    case 3:
      Changed.Body[3].Addr = 0x3098;
      break;
    case 4:
      Changed.Body[4].Val = HighExpr::makeCall("work", 0x4000, {});
      break;
    }
    EXPECT_FALSE(omitProvenObjCSynchronizedLandingPad(Changed, *Proof))
        << Mutation;
  }
  EXPECT_TRUE(omitProvenObjCSynchronizedLandingPad(F.Function, *Proof));
  ASSERT_EQ(F.Function.Body.size(), 1U);
  EXPECT_EQ(F.Function.Body.front().Addr, 0x3084U);
}

TEST(ObjCSourceProjection,
     SynchronizedForwardedPadARCDoesNotRearmAnUnlockedPath) {
  SynchronizedForwardedPadFixture F;
  F.put(0x3040, 0xb4000175U); // early path joins the normal unlock at 0x306c
  F.put(0x3074, 0xaa1303e0U);
  F.call(0x3078, 0x4120); // ARC release after the lock has been closed
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  const std::string Source = R"C(
#include <stdint.h>
extern int32_t neverd_darwin_objc_sync_enter(void*);
extern int32_t neverd_darwin_objc_sync_exit(void*);
extern void work(void*, int);
extern void *objc_retain(void*);
extern void objc_release(void*);
void neverd_objc_imp_3000(void* objc_self, void *value, int path) {
    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));
    work(value, 2);
    objc_release(value);
    if (!path) {
        (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));
        objc_release(objc_self);
        return;
    }
    work(value, 4);
    objc_release(value);
    (uint64_t)(uintptr_t)objc_retain(value);
    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));
    objc_release(objc_self);
}
)C";
  const auto Result = addObjCSynchronizedReceiverCleanup(Source, *Proof);
  ASSERT_TRUE(Result);
  auto ExtraUnlock = Source;
  ExtraUnlock.insert(
      ExtraUnlock.rfind("    objc_release(objc_self);"),
      "    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));\n");
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(ExtraUnlock, *Proof));
  EXPECT_NE(Result->find("void *active = *guard ? lock : 0;"),
            std::string::npos);
  auto Changed = Source;
  const auto LastExit = Changed.rfind("sync_exit(objc_self)");
  Changed.replace(LastExit, std::string("sync_exit(objc_self)").size(),
                  "sync_exit(value)");
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(Changed, *Proof));
  const char *Harness = R"CPP(
#include <stdint.h>
static void *lock = (void*)(uintptr_t)0x1111;
static void *value = (void*)(uintptr_t)0x2222;
static int failure, order, bad, exits, releases;
static void step(void *object, void *expected, int tag) {
    if (object != expected) bad = 1;
    order = order * 10 + tag;
    if (tag == failure) throw tag;
}
extern "C" int32_t neverd_darwin_objc_sync_enter(void *object) {
    step(object, lock, 1); return 0;
}
extern "C" int32_t neverd_darwin_objc_sync_exit(void*)
    __asm__("_objc_sync_exit");
extern "C" int32_t neverd_darwin_objc_sync_exit(void *object) {
    ++exits; step(object, lock, 7); return 0;
}
extern "C" void work(void *object, int tag) { step(object, value, tag); }
extern "C" void objc_release(void *object) {
    if (object == lock) step(object, lock, 9);
    else step(object, value, ++releases == 1 ? 3 : 5);
}
extern "C" void *objc_retain(void *object) {
    step(object, value, 6); return object;
}
extern "C" void neverd_objc_imp_3000(void*, void*, int);
int main() {
    for (int path = 0; path < 2; ++path) {
        const int wanted[] = {12345679, 1, 127, 123, 12347, 12345,
                              123456, 1234567, 12345679, 12345679};
        const int shortWanted[] = {12379, 1, 127, 123, 12379, 12379,
                                   12379, 1237, 12379, 12379};
        for (failure = 0; failure <= 9; ++failure) {
            order = bad = exits = releases = 0;
            int caught = 0;
            try { neverd_objc_imp_3000(lock, value, path); }
            catch (int exception) { caught = exception; }
            const int thrown = failure == 8 ||
                (!path && failure >= 4 && failure <= 6) ? 0 : failure;
            const int unlocks = thrown == 0 || thrown == 2 || thrown == 4 ||
                                thrown == 7 || thrown == 9;
            if (caught != thrown || exits != unlocks || bad ||
                order != (path ? wanted[failure] : shortWanted[failure]))
                return 10 + failure + path * 10;
        }
    }
    return 0;
}
)CPP";
  executeSynchronizedSource(*Result, Harness);
}

TEST(ObjCSourceProjection, SynchronizedExpandedUnlockNeedsUnconditionalReturn) {
  for (const char *Tail : {"\nobjc_release(p);\nreturn;\n",
                           "\n/* if (...) { goto x; } */ return;\n",
                           "\nconsume(\"}; return; if\"); return 0;\n"}) {
    const std::string Source = "exit();" + std::string(Tail) + "next_exit();";
    EXPECT_TRUE(
        objcSynchronizedUnlockReturns(Source, 0, Source.find("next_exit")));
  }
  for (const char *Tail :
       {"\nobjc_release(p);\n", "\n/* return; */\n",
        "\nconsume(\"return;\");\n", "\nif (p) return;\n",
        "\nif (p)\nreturn;\n", "\n{ return; }\n", "\ngoto done; return;\n",
        "\n} return;\n", "\ndone: return;\n", "\nreturn next_exit();\n"}) {
    const std::string Source = "exit();" + std::string(Tail) + "next_exit();";
    EXPECT_FALSE(
        objcSynchronizedUnlockReturns(Source, 0, Source.find("next_exit")));
  }
}

TEST(ObjCSourceProjection, SynchronizedInterleavedCOnlyWrapsUnprotectedARC) {
  SynchronizedInterleavedFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  const std::string Source = SynchronizedInterleavedSource;
  const auto Result = addObjCSynchronizedReceiverCleanup(Source, *Proof);
  ASSERT_TRUE(Result);
  EXPECT_NE(Result->find("neverd_objc_sync_release(&neverd_objc_sync_guard, "
                         "objc_self, value)"),
            std::string::npos);
  EXPECT_NE(
      Result->find("result = (uint64_t)(uintptr_t)neverd_objc_sync_retain("),
      std::string::npos);
  for (const char *Receiver : {"value", "factory()"}) {
    auto Changed = Source;
    const auto At = Changed.find("sync_exit(objc_self)");
    Changed.replace(At, std::string("sync_exit(objc_self)").size(),
                    "sync_exit(" + std::string(Receiver) + ")");
    EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(Changed, *Proof));
  }
  auto Expanded = Source;
  Expanded.insert(Expanded.find("    after();"),
                  "    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));\n");
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(Expanded, *Proof));
  auto Conditional = Source;
  Conditional.insert(Conditional.find("    objc_release(value);"),
                     "    if (path) ");
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(Conditional, *Proof));
  const std::string Helper =
      "\nvoid helper(void *object) { objc_release(object); }\n";
  const auto Appended =
      addObjCSynchronizedReceiverCleanup(Source + Helper, *Proof);
  ASSERT_TRUE(Appended);
  EXPECT_EQ(Appended->substr(Appended->size() - Helper.size()), Helper);
}

TEST(ObjCSourceProjection,
     SynchronizedInterleavedCExecutesProtectedAndARCErrors) {
  SynchronizedInterleavedFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  const auto Result =
      addObjCSynchronizedReceiverCleanup(SynchronizedInterleavedSource, *Proof);
  ASSERT_TRUE(Result);
  const char *Harness = R"CPP(
#include <stdint.h>
static void *lock = (void*)(uintptr_t)0x1111;
static void *value = (void*)(uintptr_t)0x2222;
static void *retained = (void*)(uintptr_t)0x3333;
static int failure, order, bad, exits, path, releases;
static void step(void *object, void *expected, int tag) {
    if (object != expected) bad = 1;
    order = order * 10 + tag;
    if (tag == failure) throw tag;
}
extern "C" int32_t neverd_darwin_objc_sync_enter(void *object) {
    step(object, lock, 1); return 0;
}
extern "C" int32_t neverd_darwin_objc_sync_exit(void*)
    __asm__("_objc_sync_exit");
extern "C" int32_t neverd_darwin_objc_sync_exit(void *object) {
    ++exits; step(object, lock, 7); return 0;
}
extern "C" void work(void *object, int tag) { step(object, value, tag); }
extern "C" void objc_release(void *object) {
    step(object, value, ++releases == 1 ? 3 : 5);
}
extern "C" void *objc_retain(void *object) {
    step(object, value, 6); return retained;
}
extern "C" void after(void) { step(value, value, 8); }
extern "C" uint64_t neverd_objc_imp_3000(void*, void*, int);
int main() {
    for (path = 0; path < 2; ++path) {
        const int wanted[] = {12345678, 1, 127, 123, 12347, 12345,
                              123456, 1234567, 12345678};
        const int shortWanted[] = {123678, 1, 127, 123, 123678, 123678,
                                   1236, 12367, 123678};
        for (failure = 0; failure <= 8; ++failure) {
            order = bad = exits = releases = 0;
            int caught = 0;
            uint64_t result = 0;
            try { result = neverd_objc_imp_3000(lock, value, path); }
            catch (int exception) { caught = exception; }
            const int thrown = !path && (failure == 4 || failure == 5) ? 0 : failure;
            const int unlocks = thrown == 0 || thrown == 2 || thrown == 4 ||
                                thrown == 7 || thrown == 8;
            if (caught != thrown || order != (path ? wanted[failure] : shortWanted[failure]) ||
                exits != unlocks || bad ||
                (!thrown && result != (uint64_t)(uintptr_t)retained))
                return 10 + failure + path * 9;
        }
    }
    return 0;
}
)CPP";
  executeSynchronizedSource(*Result, Harness);
}

TEST(ObjCSourceProjection, SynchronizedInterleavedPrefixCanSkipWholeLock) {
  for (uint32_t Branch :
       {0x14000017U, 0xb40002f4U, 0xb50002f4U, 0x360002f4U, 0x370002f4U}) {
    SynchronizedInterleavedFixture F;
    F.put(0x3018, Branch); // forward branch to 0x3074, after normal unlock
    const auto Proof =
        proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
    ASSERT_TRUE(Proof) << llvm::utohexstr(Branch);
    EXPECT_EQ(Proof->SuspendARC, 3U);
  }
  for (uint32_t Branch : {
           0x14000003U, // enter the protected body without acquiring
           0x14000015U, // enter the normal unlock argument load
           0x14000016U, // unlock without acquiring
           0x1400001dU, // enter the exceptional pad
           0x17ffffffU, // backward branch
           0xd61f0280U  // indirect branch through x20
       }) {
    SynchronizedInterleavedFixture F;
    F.put(0x3018, Branch);
    EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function))
        << llvm::utohexstr(Branch);
  }
  SynchronizedInterleavedFixture F;
  F.put(0x3018, 0x14000017U);
  F.put(0x3074, 0x14000006U); // bypass route reaches the cleanup pad
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
}

TEST(ObjCSourceProjection, SynchronizedInterleavedBypassNeverUnlocks) {
  SynchronizedInterleavedFixture F;
  F.put(0x3018, 0xb40002f4U);
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  const auto Source = addObjCSynchronizedReceiverCleanup(R"C(
#include <stdint.h>
extern int32_t neverd_darwin_objc_sync_enter(void*);
extern int32_t neverd_darwin_objc_sync_exit(void*);
extern void work(void*, int);
extern void *objc_retain(void*);
extern void objc_release(void*);
extern void after(void);
uint64_t neverd_objc_imp_3000(void* objc_self, void *value, int path) {
    uint64_t result = (uint64_t)(uintptr_t)value;
    if (path) {
        (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));
        work(value, 2);
        objc_release(value);
        work(value, 4);
        objc_release(value);
        result = (uint64_t)(uintptr_t)objc_retain(value);
        (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));
    }
    after();
    return result;
}
)C",
                                                         *Proof);
  ASSERT_TRUE(Source);
  const char *Harness = R"CPP(
#include <stdint.h>
static void *lock = (void*)(uintptr_t)0x1111;
static void *value = (void*)(uintptr_t)0x2222;
static void *retained = (void*)(uintptr_t)0x3333;
static int failure, order, bad, exits, releases;
static void step(void *object, void *expected, int tag) {
    if (object != expected) bad = 1;
    order = order * 10 + tag;
    if (tag == failure) throw tag;
}
extern "C" int32_t neverd_darwin_objc_sync_enter(void *object) {
    step(object, lock, 1); return 0;
}
extern "C" int32_t neverd_darwin_objc_sync_exit(void*)
    __asm__("_objc_sync_exit");
extern "C" int32_t neverd_darwin_objc_sync_exit(void *object) {
    ++exits; step(object, lock, 7); return 0;
}
extern "C" void work(void *object, int tag) { step(object, value, tag); }
extern "C" void objc_release(void *object) {
    step(object, value, ++releases == 1 ? 3 : 5);
}
extern "C" void *objc_retain(void *object) {
    step(object, value, 6); return retained;
}
extern "C" void after(void) { step(value, value, 8); }
extern "C" uint64_t neverd_objc_imp_3000(void*, void*, int);
int main() {
    const int wanted[] = {12345678, 1, 127, 123, 12347, 12345,
                          123456, 1234567, 12345678};
    for (int path = 0; path < 2; ++path)
        for (failure = 0; failure <= 8; ++failure) {
            order = bad = exits = releases = 0;
            int caught = 0;
            uint64_t result = 0;
            try { result = neverd_objc_imp_3000(lock, value, path); }
            catch (int exception) { caught = exception; }
            const int thrown = path || failure == 8 ? failure : 0;
            const int unlocks = path && (thrown == 0 || thrown == 2 ||
                                         thrown == 4 || thrown == 7 || thrown == 8);
            const uint64_t returned = (uint64_t)(uintptr_t)(path ? retained : value);
            if (caught != thrown || order != (path ? wanted[failure] : 8) ||
                exits != unlocks || bad || (!thrown && result != returned))
                return 10 + failure + path * 9;
        }
    return 0;
}
)CPP";
  executeSynchronizedSource(*Source, Harness);
}

namespace {
struct SynchronizedSingleRangeBypassFixture : SynchronizedInterleavedFixture {
  SynchronizedSingleRangeBypassFixture() {
    put(0x3018, 0xb40002f4U); // cbz x20, 0x3074: skip the entire lock
    auto &EH = *Function.ExceptionMetadata;
    auto &Sites = EH.Itanium->CallSites;
    Sites.assign(3, {});
    Sites[0].GuardedRange = {0x3000, 0x3024};
    Sites[1].GuardedRange = {0x3024, 0x306c};
    Sites[1].LandingPadVA = 0x308c;
    Sites[2].GuardedRange = {0x306c, 0x30a0};
    EH.ObjC->LandingPads = {
        {{0x3024, 0x306c}, 0x308c, ObjCPadKind::SynchronizedExit, {}}};
  }
};
} // namespace

TEST(ObjCSourceProjection, SynchronizedSingleRangePrefixCanBypassLock) {
  for (uint32_t Branch :
       {0x14000017U, 0xb40002f4U, 0xb50002f4U, 0x360002f4U, 0x370002f4U}) {
    SynchronizedSingleRangeBypassFixture F;
    F.put(0x3018, Branch);
    const auto Proof =
        proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
    ASSERT_TRUE(Proof) << llvm::utohexstr(Branch);
    EXPECT_EQ(Proof->GuardStopCall, 0x3070U);
    EXPECT_EQ(Proof->SuspendARC, 0U);
    EXPECT_EQ(Proof->UnprotectedReleases, 0U);
    EXPECT_EQ(Proof->UnprotectedRetains, 0U);
  }
  for (uint32_t Branch : {0x14000003U, 0x14000015U, 0x14000016U, 0x1400001dU,
                          0x17ffffffU, 0xd61f0280U}) {
    SynchronizedSingleRangeBypassFixture F;
    F.put(0x3018, Branch);
    EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function))
        << llvm::utohexstr(Branch);
  }
  SynchronizedSingleRangeBypassFixture F;
  F.put(0x3018, 0xd503201fU);
  EXPECT_FALSE(proveObjCSynchronizedInterleavedCleanup(F.Image, F.Function));
  EXPECT_TRUE(
      proveObjCSynchronizedRegisterReceiverCleanup(F.Image, F.Function));
  EXPECT_TRUE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  F.put(0x3018, 0xb40002f4U);
  F.Function.ExceptionMetadata->Itanium->CallSites[1].GuardedRange.End -= 4;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
}

TEST(ObjCSourceProjection, SynchronizedSingleRangeBypassProtectsEveryBodyCall) {
  SynchronizedSingleRangeBypassFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  const auto Source = addObjCSynchronizedReceiverCleanup(R"C(
#include <stdint.h>
extern int32_t neverd_darwin_objc_sync_enter(void*);
extern int32_t neverd_darwin_objc_sync_exit(void*);
extern void work(void*, int);
extern void *objc_retain(void*);
extern void objc_release(void*);
extern void after(void);
uint64_t neverd_objc_imp_3000(void* objc_self, void *value, int path) {
    uint64_t result = (uint64_t)(uintptr_t)value;
    if (path) {
        (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));
        work(value, 2);
        objc_release(value);
        work(value, 4);
        objc_release(value);
        result = (uint64_t)(uintptr_t)objc_retain(value);
        (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));
    }
    after();
    return result;
}
)C",
                                                         *Proof);
  ASSERT_TRUE(Source);
  const char *Harness = R"CPP(
#include <stdint.h>
static void *lock = (void*)(uintptr_t)0x1111;
static void *value = (void*)(uintptr_t)0x2222;
static void *retained = (void*)(uintptr_t)0x3333;
static int failure, order, bad, exits, releases;
static void step(void *object, void *expected, int tag) {
    if (object != expected) bad = 1;
    order = order * 10 + tag;
    if (tag == failure) throw tag;
}
extern "C" int32_t neverd_darwin_objc_sync_enter(void *object) {
    step(object, lock, 1); return 0;
}
extern "C" int32_t neverd_darwin_objc_sync_exit(void*)
    __asm__("_objc_sync_exit");
extern "C" int32_t neverd_darwin_objc_sync_exit(void *object) {
    ++exits; step(object, lock, 7); return 0;
}
extern "C" void work(void *object, int tag) { step(object, value, tag); }
extern "C" void objc_release(void *object) {
    step(object, value, ++releases == 1 ? 3 : 5);
}
extern "C" void *objc_retain(void *object) {
    step(object, value, 6); return retained;
}
extern "C" void after(void) { step(value, value, 8); }
extern "C" uint64_t neverd_objc_imp_3000(void*, void*, int);
int main() {
    const int wanted[] = {12345678, 1, 127, 1237, 12347, 123457,
                          1234567, 1234567, 12345678};
    for (int path = 0; path < 2; ++path)
        for (failure = 0; failure <= 8; ++failure) {
            order = bad = exits = releases = 0;
            int caught = 0;
            uint64_t result = 0;
            try { result = neverd_objc_imp_3000(lock, value, path); }
            catch (int exception) { caught = exception; }
            const int thrown = path || failure == 8 ? failure : 0;
            const int unlocks = path && thrown != 1;
            const uint64_t returned = (uint64_t)(uintptr_t)(path ? retained : value);
            if (caught != thrown || order != (path ? wanted[failure] : 8) ||
                exits != unlocks || bad || (!thrown && result != returned))
                return 10 + failure + path * 9;
        }
    return 0;
}
)CPP";
  executeSynchronizedSource(*Source, Harness);
}

namespace {
struct SynchronizedSharedSuffixFixture : SynchronizedSingleRangeBypassFixture {
  SynchronizedSharedSuffixFixture() {
    put(0x3018, 0xb4000354U); // cbz x20, 0x3080: bypass block after return
    put(0x307c, 0xd65f03c0U);
    put(0x3088, 0x17fffffcU); // b 0x3078: join the normal cleanup
  }
};
} // namespace

TEST(ObjCSourceProjection, SynchronizedSuffixAllowsAcyclicBackwardJoins) {
  SynchronizedSharedSuffixFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->ExitCall, 0x3070U);
  EXPECT_EQ(Proof->SuspendARC, 0U);
  for (uint32_t Branch :
       {0x54000040U, 0xb4000054U, 0xb5000054U, 0x36000054U, 0x37000054U}) {
    F.put(0x3074, Branch); // conditional forward join to the return
    EXPECT_TRUE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function))
        << llvm::utohexstr(Branch);
  }
  F.put(0x3074, 0xd503201fU);
  F.put(0x307c, 0x140003e1U); // external tail at 0x4000 before bypass block
  EXPECT_TRUE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
}

TEST(ObjCSourceProjection, SynchronizedSuffixRejectsCyclesAndLockEdges) {
  for (uint32_t Branch : {
           0x17fffff9U, // normal unlock argument load
           0x17ffffe7U, // protected body
           0x17fffffaU, // normal unlock call
           0x14000001U, // exceptional pad
           0x17fffffeU, // cycle through the bypass block
           0x14000000U, // self-loop
           0xd61f0280U, // indirect branch
           0xd503201fU  // fall through into the exceptional pad
       }) {
    SynchronizedSharedSuffixFixture F;
    F.put(0x3088, Branch);
    EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function))
        << llvm::utohexstr(Branch);
  }
  for (va_t Target : {0x4200U, 0x4210U, 0x4220U}) {
    SynchronizedSharedSuffixFixture F;
    F.call(0x3074, Target); // relock, extra unlock or resume in normal suffix
    EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  }
  SynchronizedSharedSuffixFixture F;
  F.put(0x307c, 0xd503201fU); // fallthrough makes a cycle across the join
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  F.put(0x307c, 0xd65f03c0U);
  F.put(0x3074, 0x54000060U); // both paths reach the shared suffix
  EXPECT_TRUE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  F.put(0x3074, 0xb4000114U); // conditional edge into the exceptional pad
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
}

TEST(ObjCSourceProjection, SynchronizedSharedSuffixKeepsARCExceptionBoundary) {
  SynchronizedSharedSuffixFixture F;
  auto &EH = *F.Function.ExceptionMetadata;
  EH.Itanium->CallSites[1].GuardedRange.End = 0x305c;
  EH.Itanium->CallSites[2].GuardedRange.Begin = 0x305c;
  EH.ObjC->LandingPads[0].GuardedRange.End = 0x305c;
  F.call(0x303c, 0x4000); // only the release after the protected range is ARC
  F.put(0x3068, 0xd503201fU);
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  ASSERT_EQ(Proof->SuspendARC, 1U);
  const auto Source = addObjCSynchronizedReceiverCleanup(R"C(
#include <stdint.h>
extern int32_t neverd_darwin_objc_sync_enter(void*);
extern int32_t neverd_darwin_objc_sync_exit(void*);
extern void work(void*);
extern void objc_release(void*);
extern void after(void);
extern void *finish(void*);
uint64_t neverd_objc_imp_3000(void* objc_self, void *value, int path) {
    uint64_t result = (uint64_t)(uintptr_t)value;
    if (!path) goto cleanup;
    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));
    work(value);
    objc_release(value);
    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));
cleanup:
    objc_release(value);
    after();
    return (uint64_t)(uintptr_t)finish((void*)(uintptr_t)result);
}
)C",
                                                         *Proof);
  ASSERT_TRUE(Source);
  const char *Harness = R"CPP(
#include <stdint.h>
static void *lock = (void*)(uintptr_t)0x1111;
static void *value = (void*)(uintptr_t)0x2222;
static int failure, order, bad, exits, releases, path;
static void step(void *object, void *expected, int tag) {
    if (object != expected) bad = 1;
    order = order * 10 + tag;
    if (tag == failure) throw tag;
}
extern "C" int32_t neverd_darwin_objc_sync_enter(void *object) {
    step(object, lock, 1); return 0;
}
extern "C" int32_t neverd_darwin_objc_sync_exit(void*)
    __asm__("_objc_sync_exit");
extern "C" int32_t neverd_darwin_objc_sync_exit(void *object) {
    ++exits; step(object, lock, 4); return 0;
}
extern "C" void work(void *object) { step(object, value, 2); }
extern "C" void objc_release(void *object) {
    step(object, value, path && ++releases == 1 ? 3 : 5);
}
extern "C" void after(void) { step(value, value, 6); }
extern "C" void *finish(void *object) {
    step(object, value, 7); return object;
}
extern "C" uint64_t neverd_objc_imp_3000(void*, void*, int);
int main() {
    const int wanted[] = {1234567, 1, 124, 123, 1234, 12345, 123456, 1234567};
    const int bypass[] = {567, 567, 567, 567, 567, 5, 56, 567};
    for (path = 0; path < 2; ++path)
        for (failure = 0; failure <= 7; ++failure) {
            order = bad = exits = releases = 0;
            int caught = 0;
            uint64_t result = 0;
            try { result = neverd_objc_imp_3000(lock, value, path); }
            catch (int exception) { caught = exception; }
            const int thrown = path || failure >= 5 ? failure : 0;
            const int unlocks = path && thrown != 1 && thrown != 3;
            if (caught != thrown || order != (path ? wanted[failure] : bypass[failure]) ||
                exits != unlocks || bad ||
                (!thrown && result != (uint64_t)(uintptr_t)value))
                return 10 + failure + path * 8;
        }
    return 0;
}
)CPP";
  executeSynchronizedSource(*Source, Harness);
}

namespace {
struct SynchronizedInterleavedLocalFixture : SynchronizedInterleavedFixture {
  SynchronizedInterleavedLocalFixture() {
    put(0x3010, 0xaa0003f4U); // save self separately from the lock
    call(0x3014, 0x4000);
    call(0x3018, 0x4100);
    put(0x301c, 0xaa0003f3U); // lock is the retained return value
  }
};

const char *SynchronizedInterleavedLocalSource = R"C(
#include <stdint.h>
extern void *cache(void*);
extern void *objc_retainAutoreleasedReturnValue(void*);
extern int32_t neverd_darwin_objc_sync_enter(void*);
extern int32_t neverd_darwin_objc_sync_exit(void*);
extern void work(void*, int);
extern void *objc_retain(void*);
extern void objc_release(void*);
extern void after(void);
uint64_t neverd_objc_imp_3000(void* objc_self, void *value, int path) {
    uint64_t saved, result;
    saved = (uint64_t)(uintptr_t)cache(objc_self);
    saved = (uint64_t)(uintptr_t)objc_retainAutoreleasedReturnValue((void*)(uintptr_t)saved);
    (uint32_t)(neverd_darwin_objc_sync_enter((void*)(uintptr_t)(saved)));
    work(value, 4);
    objc_release(value);
    if (path) {
        work(value, 6);
        objc_release(value);
    }
    result = (uint64_t)(uintptr_t)objc_retain(value);
    (uint32_t)(neverd_darwin_objc_sync_exit((void*)(uintptr_t)(saved)));
    after();
    return result;
}
)C";
} // namespace

TEST(ObjCSourceProjection,
     SynchronizedInterleavedLocalKeepsRetainedLockIdentity) {
  SynchronizedInterleavedLocalFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  EXPECT_TRUE(Proof->ReceiverIsSavedLocal);
  EXPECT_EQ(Proof->SuspendARC, 3U);
  const std::string Source = SynchronizedInterleavedLocalSource;
  const auto Result = addObjCSynchronizedReceiverCleanup(Source, *Proof);
  ASSERT_TRUE(Result);
  EXPECT_NE(Result->find("neverd_objc_sync_guard = (void*)(uintptr_t)(saved);"),
            std::string::npos);
  EXPECT_NE(Result->find("neverd_objc_sync_release(&neverd_objc_sync_guard, "
                         "(void*)(uintptr_t)(saved), value)"),
            std::string::npos);
  for (const char *Write : {"saved = 0;", "escape(&saved);"}) {
    auto Changed = Source;
    Changed.insert(Changed.find("    work(value, 4);"),
                   "    " + std::string(Write) + "\n");
    EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(Changed, *Proof));
  }
  for (const auto &[Address, Word] :
       {std::pair<va_t, uint32_t>{0x3010, 0x14000003U}, // skip retained origin
        {0x301c, 0xaa0003f4U},                          // wrong saved register
        {0x3044, 0xaa0003f3U}}) { // replace lock during protected work
    const auto Original = *objcSynchronizedWord(F.Image, Address);
    F.put(Address, Word);
    EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
    F.put(Address, Original);
  }
  F.Image.DyldBindSlots[0x5108].WeakImport = true;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
  F.Image.DyldBindSlots[0x5108].WeakImport = false;
  F.Image.DyldBindSlots[0x5108].Module = "/tmp/foreign.dylib";
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(F.Image, F.Function));
}

TEST(ObjCSourceProjection, SynchronizedInterleavedLocalExecutesOriginalLock) {
  SynchronizedInterleavedLocalFixture F;
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  const auto Result = addObjCSynchronizedReceiverCleanup(
      SynchronizedInterleavedLocalSource, *Proof);
  ASSERT_TRUE(Result);
  const char *Harness = R"CPP(
#include <stdint.h>
#include <algorithm>
#include <vector>
static void *self = (void*)(uintptr_t)0x1111;
static void *lock = (void*)(uintptr_t)0x2222;
static void *value = (void*)(uintptr_t)0x3333;
static void *retained = (void*)(uintptr_t)0x4444;
static int failure, bad, exits, releases;
static std::vector<int> events;
static void step(void *object, void *expected, int tag) {
    if (object != expected) bad = 1;
    events.push_back(tag);
    if (tag == failure) throw tag;
}
extern "C" void *cache(void *object) {
    step(object, self, 1); return lock;
}
extern "C" void *objc_retainAutoreleasedReturnValue(void *object) {
    step(object, lock, 2); return object;
}
extern "C" int32_t neverd_darwin_objc_sync_enter(void *object) {
    step(object, lock, 3); return 0;
}
extern "C" int32_t neverd_darwin_objc_sync_exit(void*)
    __asm__("_objc_sync_exit");
extern "C" int32_t neverd_darwin_objc_sync_exit(void *object) {
    ++exits; step(object, lock, 9); return 0;
}
extern "C" void work(void *object, int tag) { step(object, value, tag); }
extern "C" void objc_release(void *object) {
    step(object, value, ++releases == 1 ? 5 : 7);
}
extern "C" void *objc_retain(void *object) {
    step(object, value, 8); return retained;
}
extern "C" void after(void) { step(value, value, 10); }
extern "C" uint64_t neverd_objc_imp_3000(void*, void*, int);
int main() {
    for (int path = 0; path < 2; ++path) {
        for (failure = 0; failure <= 10; ++failure) {
            std::vector<int> wanted = path ? std::vector<int>{1,2,3,4,5,6,7,8,9,10}
                                          : std::vector<int>{1,2,3,4,5,8,9,10};
            const auto fail = std::find(wanted.begin(), wanted.end(), failure);
            const int thrown = fail != wanted.end() ? failure : 0;
            if (thrown) {
                wanted.erase(fail + 1, wanted.end());
                if (thrown == 4 || thrown == 6) wanted.push_back(9);
            }
            bad = exits = releases = 0;
            events.clear();
            int caught = 0;
            uint64_t result = 0;
            try { result = neverd_objc_imp_3000(self, value, path); }
            catch (int exception) { caught = exception; }
            const int unlocks = thrown == 0 || thrown == 4 || thrown == 6 ||
                                thrown == 9 || thrown == 10;
            if (caught != thrown || events != wanted || exits != unlocks || bad ||
                (!thrown && result != (uint64_t)(uintptr_t)retained))
                return 10 + failure + path * 11;
        }
    }
    return 0;
}
)CPP";
  executeSynchronizedSource(*Result, Harness);
}

TEST(ObjCSourceProjection, BranchedSynchronizedTokenMutationNeedsStableLock) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::MachO;
  Segment Code;
  Code.VA = 0x3000;
  Code.Size = 0xbc;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.assign(Code.Size, 0);
  const auto Put = [&](va_t Address, uint32_t Word) {
    llvm::support::endian::write32le(Code.Data.data() + Address - Code.VA,
                                     Word);
  };
  for (va_t Address = Code.VA; Address < Code.VA + Code.Size; Address += 4)
    Put(Address, 0xd503201fU); // nop
  const auto BL = [&](va_t From, va_t To) {
    Put(From, 0x94000000U | ((To - From) / 4));
  };
  Put(0x3000, 0xa9bd57f6U); // save x22/x21
  Put(0x3004, 0xa9014ff4U); // save x20/x19
  Put(0x3008, 0xa9027bfdU); // save fp/lr
  Put(0x300c, 0x910083fdU);
  Put(0x3010, 0xaa0203f3U); // x19 = argument
  Put(0x3014, 0xaa0003f5U); // x21 = objc_self
  Put(0x3018, 0xaa0203e0U);
  BL(0x301c, 0x4000);       // objc_retain(argument)
  Put(0x3030, 0xb40002b3U); // cbz x19, 0x3084
  Put(0x3034, 0xaa1503e0U);
  BL(0x3038, 0x4010); // runningTokens
  Put(0x303c, 0xaa1d03fdU);
  BL(0x3040, 0x4020);       // objc_retainAutoreleasedReturnValue
  Put(0x3044, 0xaa0003f4U); // x20 = retained lock
  BL(0x3048, 0x4030);       // objc_sync_enter
  Put(0x304c, 0xaa1503e0U);
  BL(0x3050, 0x4010);       // runningTokens
  BL(0x3058, 0x4020);       // objc_retainAutoreleasedReturnValue
  Put(0x305c, 0xaa0003f5U); // x21 = retained mutation receiver
  Put(0x3060, 0xaa1303e2U); // x2 = argument
  BL(0x3064, 0x4040);       // addObject:
  Put(0x3068, 0xaa1503e0U);
  BL(0x306c, 0x4050); // release mutation receiver
  Put(0x3070, 0xaa1403e0U);
  BL(0x3074, 0x4060); // normal objc_sync_exit
  Put(0x3078, 0xaa1403e0U);
  BL(0x307c, 0x4050);       // release lock
  Put(0x3080, 0x14000005U); // join null path at 0x3094
  Put(0x3094, 0xaa1303e0U);
  Put(0x3098, 0xa9427bfdU);
  Put(0x309c, 0xa9414ff4U);
  Put(0x30a0, 0xa8c357f6U);
  Put(0x30a4, 0x14000000U | ((0x4050 - 0x30a4) / 4));
  Put(0x30a8, 0xaa0003f3U); // preserve exception
  Put(0x30ac, 0xaa1403e0U); // same lock
  BL(0x30b0, 0x4060);       // exceptional objc_sync_exit
  Put(0x30b4, 0xaa1303e0U);
  BL(0x30b8, 0x4070); // __Unwind_Resume
  Image.Segments.push_back(std::move(Code));
  for (const auto &[Address, Name] :
       {std::pair<va_t, const char *>{0x4000, "_objc_retain"},
        {0x4010, "_objc_msgSend$runningTokens"},
        {0x4020, "_objc_retainAutoreleasedReturnValue"},
        {0x4030, "_objc_sync_enter"},
        {0x4040, "_objc_msgSend$addObject:"},
        {0x4050, "_objc_release"},
        {0x4060, "_objc_sync_exit"},
        {0x4070, "__Unwind_Resume"}}) {
    auto Symbol = Symbol::makeFunc(Address);
    Symbol.Name = Name;
    Image.Symbols.push_back(std::move(Symbol));
  }
  HighFunc Function;
  Function.Entry = 0x3000;
  Function.Params.push_back({"objc_self", NdType::makePtr(NdType::makeVoid())});
  auto &EH = Function.ExceptionMetadata.emplace();
  EH.CodeRange = {0x3000, 0x30bc};
  EH.Personality = ExceptionPersonality::ObjCPersonalityV0;
  auto &LSDA = EH.Itanium.emplace();
  LSDA.CallSites.resize(3);
  LSDA.CallSites[0].GuardedRange = {0x3000, 0x304c};
  LSDA.CallSites[1].GuardedRange = {0x304c, 0x3068};
  LSDA.CallSites[1].LandingPadVA = 0x30a8;
  LSDA.CallSites[2].GuardedRange = {0x3068, 0x30bc};
  auto &ObjC = EH.ObjC.emplace();
  ObjC.LandingPads.push_back(
      {{0x304c, 0x3068}, 0x30a8, ObjCPadKind::SynchronizedExit, {}});

  const auto Proof = proveObjCSynchronizedReceiverCleanup(Image, Function);
  ASSERT_TRUE(Proof);
  EXPECT_EQ(Proof->EnterCall, 0x3048U);
  EXPECT_EQ(Proof->GuardStopCall, 0x306cU);
  EXPECT_EQ(Proof->ExitCall, 0x3074U);
  EXPECT_EQ(Proof->LandingPad, 0x30a8U);
  EXPECT_EQ(Proof->UnprotectedReleases, 1U);
  EXPECT_TRUE(Proof->ReceiverIsSavedLocal);
  const auto Rewrite = [&](va_t Address, uint32_t Word) {
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + Address - 0x3000, Word);
  };
  const auto RejectMutation = [&](va_t Address, uint32_t Word) {
    const uint32_t Original = *objcSynchronizedWord(Image, Address);
    Rewrite(Address, Word);
    EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(Image, Function))
        << "unexpected proof with mutation at " << std::hex << Address;
    Rewrite(Address, Original);
  };
  RejectMutation(0x3030, 0xb40000b3U); // null argument enters the lock
  RejectMutation(0x3038, 0x94000000U | ((0x4040 - 0x3038) / 4));
  RejectMutation(0x3054, 0xaa0003f4U); // overwrite saved lock
  RejectMutation(0x3064, 0x94000000U | ((0x4060 - 0x3064) / 4));
  RejectMutation(0x3080, 0x14000006U); // normal path enters skip block
  RejectMutation(0x30a4, 0x14000001U); // tail no longer releases argument
  RejectMutation(0x30ac, 0xaa1303e0U); // pad unlocks wrong object
  LSDA.CallSites[1].FirstActionOffset = 1;
  EXPECT_FALSE(proveObjCSynchronizedReceiverCleanup(Image, Function));
}

TEST(ObjCSourceProjection, SynchronizedRegisterReceiverStopsAtNormalExit) {
  const char *Source =
      "extern int32_t neverd_darwin_objc_sync_enter(void*);\n"
      "extern int32_t neverd_darwin_objc_sync_exit(void*);\n"
      "void neverd_objc_imp_3000(void* objc_self) {\n"
      "    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));\n"
      "    work();\n"
      "    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));\n"
      "}\n";
  const auto Result = addObjCSynchronizedReceiverCleanup(
      Source, ObjCSynchronizedSourceProof{0x301c, 0x302c, 0x302c});
  ASSERT_TRUE(Result);
  EXPECT_LT(Result->find("neverd_objc_sync_guard = 0;"),
            Result->find("neverd_darwin_objc_sync_exit(objc_self);"));
  const char *ReleaseSource =
      "void neverd_objc_imp_3000(void* objc_self) {\n"
      "    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));\n"
      "    work();\n"
      "    objc_release(v2);\n"
      "    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));\n"
      "    objc_release(objc_self);\n"
      "}\n";
  const auto Released = addObjCSynchronizedReceiverCleanup(
      ReleaseSource, ObjCSynchronizedSourceProof{0x301c, 0x3030, 0x3038, 0x3050,
                                                 0x4040, true});
  ASSERT_TRUE(Released);
  EXPECT_LT(Released->find("neverd_objc_sync_guard = 0;"),
            Released->find("objc_release(v2);"));
  EXPECT_LT(Released->find("objc_release(v2);"),
            Released->find("neverd_darwin_objc_sync_exit(objc_self);"));
  const char *TwoReleaseSource =
      "void neverd_objc_imp_3000(void* objc_self) {\n"
      "    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));\n"
      "    work();\n"
      "    objc_release(v2);\n"
      "    objc_release(v3);\n"
      "    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));\n"
      "    objc_release(objc_self);\n"
      "}\n";
  const auto Twice = addObjCSynchronizedReceiverCleanup(
      TwoReleaseSource,
      ObjCSynchronizedSourceProof{0x301c, 0x302c, 0x303c, 0x3050, 0x4040, 2});
  ASSERT_TRUE(Twice);
  EXPECT_LT(Twice->find("neverd_objc_sync_guard = 0;"),
            Twice->find("objc_release(v2);"));
  const char *LocalReceiverSource =
      "void neverd_objc_imp_3000(void* objc_self) {\n"
      "    (uint32_t)(neverd_darwin_objc_sync_enter((void*)(uintptr_t)(v1)));\n"
      "    work();\n"
      "    objc_release(v2);\n"
      "    (uint32_t)(neverd_darwin_objc_sync_exit((void*)(uintptr_t)(v1)));\n"
      "}\n";
  const auto LocalReceiver = addObjCSynchronizedReceiverCleanup(
      LocalReceiverSource,
      ObjCSynchronizedSourceProof{0x301c, 0x3030, 0x3038, 0x3050, 0x4040, 1,
                                  true});
  ASSERT_TRUE(LocalReceiver);
  EXPECT_NE(
      LocalReceiver->find("neverd_objc_sync_guard = (void*)(uintptr_t)(v1);"),
      std::string::npos);
  std::string ChangedExit = LocalReceiverSource;
  const size_t ExitArgument = ChangedExit.rfind("(v1)");
  ASSERT_NE(ExitArgument, std::string::npos);
  ChangedExit.replace(ExitArgument, 4, "(v2)");
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(
      ChangedExit, ObjCSynchronizedSourceProof{0x301c, 0x3030, 0x3038, 0x3050,
                                               0x4040, 1, true}));
  std::string ChangedLock = LocalReceiverSource;
  const size_t ProtectedWork = ChangedLock.find("    work();");
  ASSERT_NE(ProtectedWork, std::string::npos);
  ChangedLock.insert(ProtectedWork, "    v1 = another_lock();\n");
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(
      ChangedLock, ObjCSynchronizedSourceProof{0x301c, 0x3030, 0x3038, 0x3050,
                                               0x4040, 1, true}));
}

TEST(ObjCSourceProjection, SynchronizedCleanupCoversExpandedReturnTailsOnly) {
  const std::string Source =
      "void neverd_objc_imp_3000(void* objc_self, int path) {\n"
      "    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));\n"
      "    if (!path) {\n"
      "        work();\n"
      "        (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));\n"
      "        return;\n"
      "    }\n"
      "    alternate_work();\n"
      "    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));\n"
      "}\n"
      "void unrelated(void) {\n"
      "    (uint32_t)(neverd_darwin_objc_sync_exit(0));\n"
      "}\n";
  const ObjCSynchronizedSourceProof Proof{0x301c, 0x302c, 0x302c};
  const auto Result = addObjCSynchronizedReceiverCleanup(Source, Proof);
  ASSERT_TRUE(Result);
  EXPECT_NE(Result->find("        neverd_objc_sync_guard = 0;\n"
                         "        (uint32_t)(neverd_darwin_objc_sync_exit"),
            std::string::npos);
  EXPECT_NE(Result->find("    neverd_objc_sync_guard = 0;\n"
                         "    (uint32_t)(neverd_darwin_objc_sync_exit"),
            std::string::npos);
  const auto Unrelated = Source.substr(Source.find("void unrelated"));
  EXPECT_EQ(Result->substr(Result->find("void unrelated")), Unrelated);
  auto WrongReceiver = Source;
  WrongReceiver.replace(WrongReceiver.rfind("sync_exit(objc_self)"),
                        std::string("sync_exit(objc_self)").size(),
                        "sync_exit(other)");
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(WrongReceiver, Proof));
  auto EffectfulReceiver = Source;
  EffectfulReceiver.replace(EffectfulReceiver.find("sync_exit(objc_self)"),
                            std::string("sync_exit(objc_self)").size(),
                            "sync_exit(factory())");
  EXPECT_FALSE(addObjCSynchronizedReceiverCleanup(EffectfulReceiver, Proof));
}

TEST(ObjCSourceProjection,
     SynchronizedSourceScopeIgnoresPrototypesAndLiterals) {
  const std::string Source =
      "const char *noise = \"neverd_objc_imp_fake(){ }\";\n"
      "void neverd_objc_imp_3000(void*);\n"
      "void helper(void) { /* neverd_objc_imp_fake(){} */ }\n"
      "void neverd_objc_imp_3000(void* objc_self) {\n"
      "    const char *message = \"}{\\\"}\"; // }\n"
      "    /* } */ (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));\n"
      "    work(message);\n"
      "    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));\n"
      "}\n"
      "void after_method(void) { }\n";
  const auto Body = objcSynchronizedSourceBody(Source);
  ASSERT_TRUE(Body);
  EXPECT_EQ(Body->first,
            Source.find('{', Source.rfind("void neverd_objc_imp_")));
  EXPECT_EQ(Source.substr(Body->second), "\nvoid after_method(void) { }\n");
  EXPECT_TRUE(addObjCSynchronizedReceiverCleanup(
      Source, ObjCSynchronizedSourceProof{0x301c, 0x302c, 0x302c}));
  EXPECT_FALSE(objcSynchronizedSourceBody("void neverd_objc_imp_1(void);"));
  EXPECT_FALSE(
      objcSynchronizedSourceBody("void neverd_objc_imp_1(void) { /* }"));
}

TEST(ObjCSourceProjection,
     SynchronizedExpandedTailsExecuteUnlockOnceAtO0AndO2) {
  SynchronizedBranchFixture F;
  F.put(0x3064, 0xaa1303e0U);
  F.call(0x3068, 0x4040);
  const auto Proof = proveObjCSynchronizedReceiverCleanup(F.Image, F.Function);
  ASSERT_TRUE(Proof);
  const std::string Source = R"C(
#include <stdint.h>
extern int32_t neverd_darwin_objc_sync_enter(void*);
extern int32_t neverd_darwin_objc_sync_exit(void*);
extern void work(void*, int);
extern void after(void);
void neverd_objc_imp_3000(void* objc_self, int path) {
    (uint32_t)(neverd_darwin_objc_sync_enter(objc_self));
    if (!path) {
        work(objc_self, 3);
        (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));
        after();
        return;
    }
    work(objc_self, 2);
    (uint32_t)(neverd_darwin_objc_sync_exit(objc_self));
    after();
}
)C";
  const auto Result = addObjCSynchronizedReceiverCleanup(Source, *Proof);
  ASSERT_TRUE(Result);
  const char *Harness = R"CPP(
#include <stdint.h>
static void *expected;
static int failure, order, bad, exits;
static void step(void *lock, int tag) {
    if (lock != expected) bad = 1;
    order = order * 10 + tag;
}
extern "C" int32_t neverd_darwin_objc_sync_enter(void *lock) {
    step(lock, 1); return 0;
}
extern "C" int32_t neverd_darwin_objc_sync_exit(void*)
    __asm__("_objc_sync_exit");
extern "C" int32_t neverd_darwin_objc_sync_exit(void *lock) {
    step(lock, 6); ++exits; if (failure == 2) throw 2; return 0;
}
extern "C" void work(void *lock, int tag) {
    step(lock, tag); if (failure == 1) throw 1;
}
extern "C" void after(void) { step(expected, 7); }
extern "C" void neverd_objc_imp_3000(void*, int);
int main() {
    for (int path = 0; path < 2; ++path) {
        expected = (void*)(uintptr_t)(0x1234 + path);
        for (failure = 0; failure <= 2; ++failure) {
            order = bad = exits = 0;
            int caught = 0;
            try { neverd_objc_imp_3000(expected, path); }
            catch (int exception) { caught = exception; }
            const int prefix = path ? 12 : 13;
            const int wanted = failure ? prefix * 10 + 6 : prefix * 100 + 67;
            if (caught != failure || order != wanted || exits != 1 || bad)
                return 10 + failure + path * 3;
        }
    }
    return 0;
}
)CPP";
  executeSynchronizedSource(*Result, Harness);
}
