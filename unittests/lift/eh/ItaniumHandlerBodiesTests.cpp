//===- ItaniumHandlerBodiesTests.cpp - Catch bodies from landing pads -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A C++ view spells an Itanium region's clauses as `catch` bodies taken from
/// the landing pad: the pad's code as it runs for each clause's selector.
/// These tests pin the runs that read as handlers, the cleanup scopes a pad's
/// no-match path becomes, and the shapes that keep their pad in place.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t kEntry = 0x401000;
constexpr va_t kCall = kEntry + 0x4;
constexpr va_t kAfter = kEntry + 0x9;
constexpr va_t kPad = kEntry + 0x40;
constexpr va_t kRisky = 0x402000;
constexpr va_t kDtor = 0x402100;
constexpr va_t kBeginCatch = 0x403000;
constexpr va_t kEndCatch = 0x403010;
constexpr va_t kResume = 0x403020;

MedVar var(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Size;
  V.TheArch = Arch::X64;
  return V;
}

ExprPtr use(int Id, uint16_t Size = 8) {
  return HighExpr::makeVar(var(Id, Size), NdType::makeInt(Size, false));
}

ExprPtr exception() {
  MedVar V;
  V.Kind = MedVar::EHException;
  V.Id = -1;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(8, false));
}

ExprPtr selector() {
  MedVar V;
  V.Kind = MedVar::EHSelector;
  V.Id = -1;
  V.Size = 4;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(4));
}

ExprPtr constant(uint64_t Value, uint16_t Size = 8) {
  auto E = HighExpr::makeConst(Value, Size, ConstantAddressProvenance::Scalar);
  E->Type = NdType::makeInt(Size);
  return E;
}

ExprPtr call(const std::string &Name, va_t Target, std::vector<ExprPtr> Args,
             bool Void = false) {
  auto E = HighExpr::makeCall(Name, Target, std::move(Args));
  E->Type = Void ? NdType::makeVoid() : NdType::makeInt(8, false);
  return E;
}

HighStmt assign(int Id, ExprPtr Value, va_t Addr = 0) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Addr = Addr;
  S.Dst = use(Id);
  S.Val = std::move(Value);
  return S;
}

HighStmt statement(ExprPtr Call, va_t Addr = 0) {
  HighStmt S;
  S.Kind = StmtKind::Call;
  S.Addr = Addr;
  S.CallExpr = std::move(Call);
  return S;
}

HighStmt ret(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

HighStmt anchor(va_t Addr) {
  HighStmt S;
  S.Kind = StmtKind::Block;
  S.Addr = Addr;
  return S;
}

HighStmt branch(ExprPtr Cond, std::vector<HighStmt> Then) {
  HighStmt S;
  S.Kind = StmtKind::If;
  S.Cond = std::move(Cond);
  S.Body = std::move(Then);
  return S;
}

ExprPtr equals(ExprPtr L, uint64_t R) {
  auto E = HighExpr::makeBinop(NdOp::INT_EQUAL, std::move(L), constant(R, 4));
  E->Type = NdType::makeInt(1, false);
  return E;
}

ExprPtr beginCatch(ExprPtr Object) {
  return call("__cxa_begin_catch", kBeginCatch, {std::move(Object)});
}

HighStmt endCatch() {
  return statement(call("__cxa_end_catch", kEndCatch, {}, true));
}

HighStmt resume(ExprPtr Object) {
  auto E = call("_Unwind_Resume", kResume, {std::move(Object)}, true);
  E->DoesNotReturn = true;
  return statement(std::move(E));
}

/// The LSDA of one call site at kCall landing on kPad, whose action chain
/// names \p Filters in order (0 is a cleanup), with a type entry per filter.
ItaniumEHInfo lsda(const std::vector<int64_t> &Filters) {
  ItaniumEHInfo LSDA;
  LSDA.IsCallSiteAddressForm = true;
  ItaniumCallSite Site;
  Site.GuardedRange = {kCall, kAfter};
  Site.LandingPadVA = kPad;
  Site.FirstActionOffset = 0;
  LSDA.CallSites.push_back(Site);
  for (size_t I = 0; I < Filters.size(); ++I) {
    ItaniumAction Action;
    Action.TableOffset = I * 2;
    Action.TypeFilter = Filters[I];
    if (I + 1 < Filters.size())
      Action.NextActionOffset = (I + 1) * 2;
    LSDA.Actions.push_back(Action);
  }
  return LSDA;
}

HighEHClause clause(int64_t Filter, const std::string &TypeName) {
  HighEHClause Clause;
  Clause.Kind = HighEHClauseKind::ItaniumCatch;
  Clause.TypeFilter = Filter;
  Clause.TypeName = TypeName;
  Clause.TypeDescriptorVA = TypeName.empty() ? 0 : 0x404000 + Filter * 0x10;
  Clause.LandingPadVAs = {kPad};
  Clause.HandlerVA = kPad;
  return Clause;
}

/// `try { v1 = risky(); } <clauses>; return v1;` followed by \p Pad.
HighFunc function(const std::vector<HighEHClause> &Clauses,
                  std::vector<HighStmt> Pad,
                  const std::vector<int64_t> &Chain) {
  HighFunc Func;
  Func.Entry = kEntry;
  Func.Name = "guarded";
  Func.ReturnType = NdType::makeInt(8);
  ExceptionFunction EH;
  EH.CodeRange = {kEntry, kPad + 0x40};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium = lsda(Chain);
  Func.ExceptionMetadata = std::move(EH);
  HighStmt Try;
  Try.Kind = StmtKind::ItaniumTry;
  Try.Addr = kCall;
  Try.EHRange = {kCall, kAfter};
  Try.EHIsReducible = true;
  Try.Body.push_back(assign(1, call("risky", kRisky, {}), kCall));
  Try.EHClauses = Clauses;
  Try.EHClauseBodies.resize(Clauses.size());
  Func.Body.push_back(std::move(Try));
  Func.Body.push_back(ret(use(1)));
  Pad.insert(Pad.begin(), anchor(kPad));
  for (HighStmt &S : Pad)
    Func.Body.push_back(std::move(S));
  return Func;
}

std::string emit(const HighFunc &Func, bool Structured = true) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.EmitComments = false;
  Options.StructuredExceptionSyntax = Structured;
  EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  return Source;
}

/// The definition of `guarded` in \p Source.
std::string body(const std::string &Source) {
  const size_t Begin = Source.find("guarded(void) {");
  return Begin == std::string::npos ? Source : Source.substr(Begin);
}

TEST(ItaniumHandlerBodies, OneClausePadBecomesItsCatch) {
  // L_pad: v2 = __cxa_begin_catch(exc); v3 = *(v2 + 8); __cxa_end_catch();
  //        return -v3;
  std::vector<HighStmt> Pad;
  Pad.push_back(assign(2, beginCatch(exception())));
  Pad.push_back(
      assign(3, HighExpr::makeLoad(
                    HighExpr::makeBinop(NdOp::INT_ADD, use(2), constant(8)),
                    NdType::makeInt(8, false))));
  Pad.push_back(endCatch());
  Pad.push_back(ret(HighExpr::makeBinop(NdOp::INT_SUB, constant(0), use(3))));
  const HighFunc Func =
      function({clause(1, "_ZTISt13runtime_error")}, std::move(Pad), {1});
  const std::string Source = body(emit(Func));
  EXPECT_NE(Source.find("try {"), std::string::npos) << Source;
  EXPECT_NE(Source.find("} catch (std::runtime_error &e) {"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("&e"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__cxa_begin_catch"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__cxa_end_catch"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("eh_exception"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("L_401040"), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, SelectorTestsAreDecidedPerClause) {
  // v6 = exc; v7 = sel;
  // if (v7 == 2) { begin_catch(v6); end_catch(); return 20; }
  // begin_catch(v6); end_catch(); return 10;
  std::vector<HighStmt> Pad;
  Pad.push_back(assign(6, exception()));
  Pad.push_back(assign(7, selector()));
  std::vector<HighStmt> Second;
  Second.push_back(statement(beginCatch(use(6))));
  Second.push_back(endCatch());
  Second.push_back(ret(constant(20)));
  Pad.push_back(branch(equals(use(7, 4), 2), std::move(Second)));
  Pad.push_back(statement(beginCatch(use(6))));
  Pad.push_back(endCatch());
  Pad.push_back(ret(constant(10)));
  const HighFunc Func =
      function({clause(2, "_ZTIi"), clause(1, "")}, std::move(Pad), {2, 1});
  const std::string Source = body(emit(Func));
  const size_t Int = Source.find("} catch (int &) {");
  const size_t All = Source.find("} catch (...) {");
  ASSERT_NE(Int, std::string::npos) << Source;
  ASSERT_NE(All, std::string::npos) << Source;
  EXPECT_NE(Source.find("return 0x14;", Int), std::string::npos) << Source;
  EXPECT_LT(Source.find("return 0x14;", Int), All) << Source;
  EXPECT_NE(Source.find("return 10;", All), std::string::npos) << Source;
  EXPECT_EQ(Source.find("eh_selector"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("v7 =="), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, ANoMatchCleanupWrapsTheRegion) {
  // The chain ends in a cleanup: with no clause matched the pad destroys an
  // object and resumes.  That is a catch-all around the try that rethrows,
  // and the handler, which an exception it throws must not destroy again,
  // keeps only its own code.
  std::vector<HighStmt> Pad;
  Pad.push_back(assign(6, exception()));
  std::vector<HighStmt> Caught;
  Caught.push_back(statement(beginCatch(use(6))));
  Caught.push_back(endCatch());
  Caught.push_back(ret(constant(7)));
  Pad.push_back(branch(equals(selector(), 1), std::move(Caught)));
  Pad.push_back(statement(call("held_dtor", kDtor, {}, true)));
  Pad.push_back(resume(use(6)));
  const HighFunc Func = function({clause(1, "_ZTIi")}, std::move(Pad), {1, 0});
  const std::string Source = body(emit(Func));
  const size_t Outer = Source.find("try {");
  const size_t Inner = Source.find("try {", Outer + 1);
  const size_t Int = Source.find("} catch (int &) {");
  const size_t Cleanup = Source.find("} catch (...) {");
  ASSERT_NE(Inner, std::string::npos) << Source;
  ASSERT_NE(Int, std::string::npos) << Source;
  ASSERT_NE(Cleanup, std::string::npos) << Source;
  EXPECT_LT(Int, Cleanup) << Source;
  EXPECT_GT(Source.find("held_dtor(", Cleanup), Cleanup) << Source;
  EXPECT_NE(Source.find("throw;", Cleanup), std::string::npos) << Source;
  // The handler does not run the cleanup.
  EXPECT_GT(Source.find("held_dtor("), Cleanup) << Source;
  EXPECT_EQ(Source.find("_Unwind_Resume"), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, APadAnotherTryAlsoDispatchesStaysInPlace) {
  // The call site's chain names a clause of an enclosing try, which these
  // handlers do not spell: the region keeps its pad.
  std::vector<HighStmt> Pad;
  Pad.push_back(statement(beginCatch(exception())));
  Pad.push_back(endCatch());
  Pad.push_back(ret(constant(1)));
  const HighFunc Func = function({clause(1, "_ZTIi")}, std::move(Pad), {1, 2});
  const std::string Source = body(emit(Func));
  EXPECT_EQ(Source.find("} catch ("), std::string::npos) << Source;
  EXPECT_NE(Source.find("/* catch ("), std::string::npos) << Source;
  EXPECT_NE(Source.find("__cxa_begin_catch"), std::string::npos) << Source;
  EXPECT_NE(Source.find("L_401040"), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, PlainCKeepsThePadAndItsUnwinderValues) {
  // C has no catch: the pad stays, labelled, and its copies of the values the
  // unwinder hands it read them rather than an unknown.
  std::vector<HighStmt> Pad;
  Pad.push_back(assign(6, exception()));
  Pad.push_back(statement(call("held_dtor", kDtor, {}, true)));
  Pad.push_back(resume(use(6)));
  HighFunc Func = function({clause(1, "_ZTIi")}, std::move(Pad), {0});
  // A cleanup-only call site has no region.
  Func.Body.erase(Func.Body.begin());
  Func.Body.insert(Func.Body.begin(),
                   assign(1, call("risky", kRisky, {}), kCall));
  const std::string Source = body(emit(Func, /*Structured=*/false));
  EXPECT_NE(Source.find("L_401040: /* landing pad"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("held_dtor("), std::string::npos) << Source;
  EXPECT_NE(Source.find("_Unwind_Resume"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
}

TEST(ItaniumHandlerBodies, ACatchParameterPointerIsItsValue) {
  std::vector<HighStmt> Pad;
  Pad.push_back(assign(2, beginCatch(exception())));
  Pad.push_back(
      assign(3, HighExpr::makeLoad(
                    HighExpr::makeBinop(NdOp::INT_ADD, use(2), constant(8)),
                    NdType::makeInt(8, false))));
  Pad.push_back(endCatch());
  Pad.push_back(ret(use(3)));
  const HighFunc Func =
      function({clause(1, "P15CxxEhProbeError")}, std::move(Pad), {1});
  const std::string Source = body(emit(Func));
  EXPECT_NE(Source.find("} catch (CxxEhProbeError *e) {"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("&e"), std::string::npos) << Source;
}

} // namespace
