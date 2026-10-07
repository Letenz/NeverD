//===- HighValueForwardTests.cpp - Folding values into their uses ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <string>

namespace {
using namespace neverd;

ExprPtr input() {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = 0;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(8, false));
}

/// A temporary as MedToHigh leaves it: a signed integer by default.
ExprPtr local(int Id, TypeRef Type = NdType::makeInt(8, true)) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, std::move(Type));
}

ExprPtr constant(uint64_t N) { return HighExpr::makeConst(N, 8); }

ExprPtr op(NdOp Op, ExprPtr A, ExprPtr B, bool Bool = false) {
  auto E = HighExpr::makeBinop(Op, std::move(A), std::move(B));
  E->Type = Bool ? NdType::makeInt(1, false) : NdType::makeInt(8, true);
  return E;
}

HighStmt assign(ExprPtr Dst, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = std::move(Dst);
  S.Val = std::move(Value);
  return S;
}

HighStmt result(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

HighStmt when(ExprPtr Cond, std::vector<HighStmt> Body) {
  HighStmt S;
  S.Kind = StmtKind::If;
  S.Cond = std::move(Cond);
  S.Body = std::move(Body);
  return S;
}

HighFunc function(const std::string &Name, std::vector<HighStmt> Body) {
  HighFunc F;
  F.Name = Name;
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(8, false);
  F.Params = {{"arg0", NdType::makeInt(8, false)}};
  F.Body = std::move(Body);
  return F;
}

std::string emit(const std::vector<HighFunc> &Functions) {
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  EXPECT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  return Source;
}

std::string declarationOf(const std::string &Source, const std::string &Name) {
  const std::string Suffix = " " + Name + ";";
  size_t At = Source.find(Suffix);
  if (At == std::string::npos)
    return {};
  const size_t Start = Source.rfind('\n', At) + 1;
  return Source.substr(Start, At + Suffix.size() - Start);
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
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-signedness", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-signedness", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-signedness", "err",
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

ExprPtr renamed(int Tag) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Tag;
  V.RenameTag = Tag;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(8, true));
}

HighStmt loop(ExprPtr Cond, std::vector<HighStmt> Body) {
  HighStmt S;
  S.Kind = StmtKind::While;
  S.Cond = std::move(Cond);
  S.Body = std::move(Body);
  return S;
}

TEST(HighValueForward, LoopConditionKeepsAValueTheLoopChanges) {
  // n = count + 1 before the loop, and the loop changes count under the
  // same name: the condition must keep reading n.
  HighFunc F = function(
      "bound",
      {assign(renamed(3), input()),
       assign(local(1), op(NdOp::INT_ADD, renamed(3), constant(1))),
       assign(renamed(2), constant(0)),
       loop(op(NdOp::INT_LESS, renamed(2), local(1), true),
            {assign(renamed(2), op(NdOp::INT_ADD, renamed(2), constant(1))),
             assign(renamed(3), op(NdOp::INT_ADD, renamed(3), constant(5)))}),
       result(renamed(2))});
  const std::string Source = emit({F});
  EXPECT_NE(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t x = 0; x < 40; ++x)
    if (bound(x) != x + 1)
      return 1;
  return 0;
}
)");
}

TEST(HighValueForward, LoopConditionFoldsAValueTheLoopKeeps) {
  // The loop never assigns count, so n may print as count + 1.
  HighFunc F = function(
      "fixed",
      {assign(renamed(3), input()),
       assign(local(1), op(NdOp::INT_ADD, renamed(3), constant(1))),
       assign(renamed(2), constant(0)),
       loop(op(NdOp::INT_LESS, renamed(2), local(1), true),
            {assign(renamed(2), op(NdOp::INT_ADD, renamed(2), constant(1)))}),
       result(renamed(2))});
  const std::string Source = emit({F});
  EXPECT_EQ(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t x = 0; x < 40; ++x)
    if (fixed(x) != x + 1)
      return 1;
  return 0;
}
)");
}

ExprPtr frameSlot(uint64_t Distance) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = 900;
  V.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeBinop(NdOp::INT_SUB,
                             HighExpr::makeVar(V, NdType::makeInt(8, false)),
                             constant(Distance));
}

HighStmt store(ExprPtr Address, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Store;
  S.StoreAddr = std::move(Address);
  S.StoreVal = std::move(Value);
  return S;
}

/// Saves x in a slot, reads it back, then stores 5 to the slot at
/// \p Overwritten before returning what it read.
HighFunc reloaded(const std::string &Name, uint64_t Overwritten) {
  HighFunc F = function(
      Name, {store(frameSlot(16), input()),
             assign(local(1), HighExpr::makeLoad(frameSlot(16),
                                                 NdType::makeInt(8, false))),
             store(frameSlot(Overwritten), constant(5)), result(local(1))});
  F.FrameSize = 64;
  return F;
}

TEST(HighValueForward, SlotLoadStaysBeforeAStoreToItsSlot) {
  const std::string Source = emit({reloaded("same_slot", 16)});
  EXPECT_NE(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
int main(void) { return same_slot(7) == 7 && same_slot(9) == 9 ? 0 : 1; }
)");
}

TEST(HighValueForward, SlotLoadStaysBeforeAnAssignmentToItsSlot) {
  HighFunc F = reloaded("assigned_slot", 16);
  F.Body[2] = assign(HighExpr::makeLoad(frameSlot(16),
                                       NdType::makeInt(8, false)),
                     constant(5));
  const std::string Source = emit({F});
  EXPECT_NE(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
int main(void) { return assigned_slot(7) == 7 && assigned_slot(9) == 9 ? 0 : 1; }
)");
}

TEST(HighValueForward, SlotLoadFoldsPastAStoreToAnotherSlot) {
  const std::string Source = emit({reloaded("other_slot", 32)});
  EXPECT_EQ(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
int main(void) { return other_slot(7) == 7 && other_slot(9) == 9 ? 0 : 1; }
)");
}

TEST(HighValueForward, CopyKeepsTheValueItsSourceLoses) {
  // t1 copies v3, then v3 changes under the same name; t1 must keep the old
  // value, in straight-line code and around a loop.
  HighFunc Straight =
      function("copy_straight",
               {assign(renamed(3), input()), assign(local(1), renamed(3)),
                assign(renamed(3), op(NdOp::INT_ADD, renamed(3), constant(5))),
                result(op(NdOp::INT_MULT, local(1), renamed(3)))});
  HighFunc Looped = function(
      "copy_looped",
      {assign(renamed(3), input()), assign(local(1), renamed(3)),
       loop(op(NdOp::INT_LESS, renamed(3), constant(20), true),
            {assign(renamed(3), op(NdOp::INT_ADD, renamed(3), constant(7)))}),
       result(op(NdOp::INT_SUB, renamed(3), local(1)))});
  const std::string Source = emit({Straight, Looped});
  EXPECT_NE(declarationOf(Source, "t1"), "") << Source;
  compileAndRun(Source + R"(
static uint64_t looped_ref(uint64_t x) { uint64_t c = x; while (x < 20) x += 7; return x - c; }
int main(void) {
  for (uint64_t x = 0; x < 30; ++x) {
    if (copy_straight(x) != x * (x + 5)) return 1;
    if (copy_looped(x) != looped_ref(x)) return 2;
  }
  return 0;
}
)");
}

} // namespace
