//===- HighSwitchFollowTests.cpp - Switch follow semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <string>

namespace {
using namespace neverd;

TypeRef word() { return NdType::makeInt(8, false); }

ExprPtr input() {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = 0;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, word());
}

ExprPtr value() {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = 1;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, word());
}

ExprPtr constant(uint64_t N) { return HighExpr::makeConst(N, 8); }

ExprPtr bitSet(uint64_t Bit) {
  return HighExpr::makeBinop(
      NdOp::INT_NOTEQUAL,
      HighExpr::makeBinop(NdOp::INT_AND, input(), constant(Bit)), constant(0));
}

HighStmt set(va_t Address, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Addr = Address;
  S.Dst = value();
  S.Val = std::move(Value);
  return S;
}

HighStmt add(va_t Address, uint64_t N) {
  return set(Address, HighExpr::makeBinop(NdOp::INT_ADD, value(), constant(N)));
}

HighStmt jump(va_t Address, va_t Target) {
  HighStmt S;
  S.Kind = StmtKind::Goto;
  S.Addr = Address;
  S.GotoTarget = Target;
  return S;
}

HighStmt leave(va_t Address) {
  HighStmt S;
  S.Kind = StmtKind::Break;
  S.Addr = Address;
  return S;
}

HighStmt result(va_t Address, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.Addr = Address;
  S.RetVal = std::move(Value);
  return S;
}

HighStmt when(va_t Address, uint64_t Bit, std::vector<HighStmt> Body) {
  HighStmt S;
  S.Kind = StmtKind::If;
  S.Addr = Address;
  S.Cond = bitSet(Bit);
  S.Body = std::move(Body);
  return S;
}

constexpr va_t Tail = 0x160, Q = 0x180, P = 0x184, J = 0x190;

enum class Variant {
  Moves,
  NoDefault,
  BreakUnderTry,
  OnlyBreakUnderTry,
  ElseFallsOut,
  EnteredFromOutside,
  LooseBreak,
  UnnamedStart,
  LabelWouldMove
};

// v = 5;
// if (x & 64) {
//   switch (x & 7) {
//   case 0: v = 1; goto J;   case 1: v = 2; goto J;   case 2: v = 3; goto J;
//   case 3: v = 4;
//   case 4: v = 6; if (x & 8) break; v = 7; goto J;
//   case 5: v = 8; goto P;
//   case 6: v = 9;
//   default: return 99;
//   }
//   Tail: v = v + 10;
// } else {
//   return 50;
// }
// Q: v = v + 100; P: v = v + 1000; J: return v;
HighFunc build(Variant Kind, const std::string &Name) {
  std::vector<HighStmt> Fourth = {set(0x130, constant(6)),
                                  when(0x134, 8, {leave(0x138)}),
                                  set(0x13c, constant(7)), jump(0x140, J)};
  const bool OnlyTry = Kind == Variant::OnlyBreakUnderTry;
  if (Kind == Variant::BreakUnderTry || OnlyTry) {
    HighStmt Try;
    Try.Kind = StmtKind::SEHTry;
    Try.Addr = 0x134;
    Try.Body = {Fourth[1]};
    Fourth[1] = Try;
  }
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.Addr = 0x108;
  Switch.SwitchExpr = HighExpr::makeBinop(NdOp::INT_AND, input(), constant(7));
  Switch.Cases = {{0, {set(0x110, constant(1)), jump(0x114, J)}},
                  {1, {set(0x118, constant(2)), jump(0x11c, J)}},
                  {2, {set(0x120, constant(3)), jump(0x124, J)}},
                  {3, {set(0x128, constant(4))}},
                  {4, Fourth},
                  {5,
                   {set(0x148, constant(8)),
                    jump(0x14c, Kind == Variant::LabelWouldMove ? Q : P)}},
                  {6, {set(0x154, constant(9))}}};
  // With every other case leaving by jump, the break under the try is the
  // only way out.
  if (OnlyTry) {
    Switch.Cases[3].Body.push_back(jump(0x12c, J));
    Switch.Cases[6].Body.push_back(jump(0x158, J));
  }
  if (Kind != Variant::NoDefault)
    Switch.DefaultBody = {result(0x150, constant(99))};
  HighStmt Choice;
  Choice.Kind = StmtKind::IfElse;
  Choice.Addr = 0x104;
  Choice.Cond = bitSet(64);
  Choice.Body = {Switch, add(Kind == Variant::UnnamedStart ? 0 : Tail, 10)};
  if (Kind == Variant::LooseBreak)
    Choice.Body.push_back(when(0x164, 16, {leave(0x168)}));
  // A second statement at Q's address: Q names no single statement, and
  // the move would make it name this one.
  if (Kind == Variant::LabelWouldMove)
    Choice.Body.push_back(add(Q, 0));
  Choice.ElseBody = {Kind == Variant::ElseFallsOut
                         ? set(0x170, constant(50))
                         : result(0x170, constant(50))};
  HighFunc F;
  F.Name = Name;
  F.Entry = 0x100;
  F.ReturnType = word();
  F.Params = {{"arg0", word()}};
  F.Body = {set(0x100, constant(5))};
  if (Kind == Variant::EnteredFromOutside)
    F.Body.push_back(when(0x102, 128, {jump(0x103, Q)}));
  F.Body.insert(F.Body.end(),
                {Choice, add(Q, 100), add(P, 1000), result(J, value())});
  return F;
}

std::string printed(const HighFunc &F) {
  std::string Text;
  for (const HighStmt &S : F.Body)
    Text += S.str() + "\n";
  return Text;
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-switch-follow", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-switch-follow", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-switch-follow", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-fsanitize=undefined",
        "-fsanitize-trap=undefined",
        "-Werror=uninitialized",
        "-Werror=return-type",
        SourcePath,
        "-o",
        BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 30, 0, &Error);
    ASSERT_EQ(Result, 0) << Error << '\n' << Source;
  }
}

TEST(HighSwitchFollow, BusiestExitFollowsTheSwitch) {
  // Four jumps go to J, and the switch falls out to Tail three times (cases
  // 3 and 6 and the break), four without a default.  Tail, Q and P move into
  // case 6, the other ways out jump to Tail, and J follows the if/else, so
  // the jumps to J become breaks.  The C of each function before and after
  // returns the same for every input.
  std::vector<HighFunc> Functions;
  for (Variant Kind : {Variant::Moves, Variant::NoDefault}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    const std::string Name =
        Kind == Variant::Moves ? "with_default" : "without_default";
    HighFunc Before = build(Kind, Name + "_before");
    HighFunc After = build(Kind, Name + "_after");
    ASSERT_TRUE(busiestExitFollowsTheSwitch(After.Body)) << printed(After);
    breakToTheLoopFollow(After.Body);
    size_t ToJ = 0, ToTail = 0, FromBreak = 0;
    walkStmts(After.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Goto)
        return;
      ToJ += S.GotoTarget == J;
      ToTail += S.GotoTarget == Tail;
      FromBreak += S.GotoTarget == Tail && S.Addr == 0x138;
    });
    EXPECT_EQ(ToJ, 0u) << printed(After);
    EXPECT_EQ(FromBreak, 1u) << printed(After);
    EXPECT_EQ(ToTail, Kind == Variant::Moves ? 2u : 3u) << printed(After);
    Functions.push_back(std::move(Before));
    Functions.push_back(std::move(After));
  }
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  compileAndRun(Source + R"(
static uint64_t reference(uint64_t x, int with_default) {
  uint64_t v = 5;
  if (!(x & 64))
    return 50;
  switch (x & 7) {
  case 0: return 1;
  case 1: return 2;
  case 2: return 3;
  case 3: v = 4; break;
  case 4: v = 6; if (x & 8) break; return 7;
  case 5: return 8 + 1000;
  case 6: v = 9; break;
  default: if (with_default) return 99; break;
  }
  return v + 10 + 100 + 1000;
}
int main(void) {
  for (uint64_t x = 0; x < 256; ++x) {
    if (with_default_before(x) != reference(x, 1))
      return 1;
    if (with_default_after(x) != reference(x, 1))
      return 2;
    if (without_default_before(x) != reference(x, 0))
      return 3;
    if (without_default_after(x) != reference(x, 0))
      return 4;
  }
  return 0;
}
)");
}

TEST(HighSwitchFollow, BreakUnderATryJumpsToTheMovedCode) {
  // A break under a try leaves the switch too.  Moved code never goes under
  // the try; the break jumps to it instead, which C allows out of a try.
  // With no other way out, the code follows the default's return.
  for (Variant Kind : {Variant::BreakUnderTry, Variant::OnlyBreakUnderTry}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    HighFunc F = build(Kind, "follow_try");
    ASSERT_TRUE(busiestExitFollowsTheSwitch(F.Body)) << printed(F);
    bool Jumps = false, UnderTry = false;
    std::vector<HighStmt> Holder;
    walkStmts(F.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::SEHTry)
        walkStmts(S.Body, [&](const HighStmt &T) {
          Jumps |= T.Kind == StmtKind::Goto && T.GotoTarget == Tail &&
                   T.Addr == 0x138;
          UnderTry |= T.Addr == Tail || T.Addr == P;
        });
      if (S.Kind == StmtKind::Switch)
        Holder =
            Kind == Variant::BreakUnderTry ? S.Cases[6].Body : S.DefaultBody;
    });
    EXPECT_TRUE(Jumps) << printed(F);
    EXPECT_FALSE(UnderTry) << printed(F);
    ASSERT_GE(Holder.size(), 3u) << printed(F);
    EXPECT_EQ(Holder[Holder.size() - 3].Addr, Tail) << printed(F);
    EXPECT_EQ(Holder.back().Addr, P) << printed(F);
  }
}

TEST(HighSwitchFollow, SwitchKeepsItsFollowWhenTheMoveIsUnsafe) {
  // The else arm falls out too, a jump from outside enters Q, Tail holds a
  // loose break, two ways out need a Tail with no address to jump to, or
  // the move would change which statement a label names.
  for (Variant Kind :
       {Variant::ElseFallsOut, Variant::EnteredFromOutside, Variant::LooseBreak,
        Variant::UnnamedStart, Variant::LabelWouldMove}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    HighFunc F = build(Kind, "follow_kept");
    const std::string Original = printed(F);
    EXPECT_FALSE(busiestExitFollowsTheSwitch(F.Body));
    EXPECT_EQ(printed(F), Original);
  }
}

} // namespace
