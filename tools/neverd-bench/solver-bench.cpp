//===- solver-bench.cpp - Compare and export bitvector queries ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/solver/Z3Solver.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;
using namespace neverd::solver;
using namespace neverd::symbolic;

namespace {

cl::opt<std::string> Input(cl::Positional, cl::Required,
                           cl::desc("<expression corpus>"));
cl::opt<unsigned> Width("width", cl::init(32));
cl::opt<unsigned> Repeat("repeat", cl::init(3));
cl::opt<unsigned> Timeout("timeout-ms", cl::init(1000));
cl::opt<unsigned long long> Conflicts("max-conflicts", cl::init(10000));
cl::opt<std::string> Backend("backend", cl::init("both"),
                             cl::desc("builtin, z3, or both"));
cl::opt<std::string> DumpDirectory("dump-dir", cl::init(""),
                                   cl::desc("Export Z3 queries for replay"));

struct Measurement {
  EquivResult Verdict = EquivResult::Invalid;
  std::optional<EquivResult> Decided;
  bool ModelValid = true;
  bool Stable = true;
  std::vector<int64_t> Times;
  std::vector<int64_t> DecisiveTimes;
  std::map<std::string, unsigned> VerdictCounts;
  std::string Query;
  std::string Reason;
};

EquivResult equivalence(SatResult R) {
  switch (R) {
  case SatResult::Sat:
    return EquivResult::Different;
  case SatResult::Unsat:
    return EquivResult::Equal;
  case SatResult::Unknown:
    return EquivResult::Unknown;
  case SatResult::Invalid:
    return EquivResult::Invalid;
  }
  llvm_unreachable("invalid SAT result");
}

bool decisive(EquivResult R) {
  return R == EquivResult::Equal || R == EquivResult::Different;
}

Measurement measure(SymContext &Ctx, SymRef A, SymRef B, bool UseZ3) {
  Measurement M;
  for (unsigned I = 0; I < Repeat; ++I) {
    BitVectorModel Model;
    EquivResult Result;
    const auto Start = std::chrono::steady_clock::now();
    if (UseZ3) {
      Z3SolverOptions Options;
      Options.TimeoutMs = Timeout;
      Z3Solver Solver(Ctx, Options);
      Result = equivalence(Solver.checkDistinct(A, B));
      Model = Solver.model();
      M.Reason = Solver.reasonUnknown();
      const auto End = std::chrono::steady_clock::now();
      M.Times.push_back(
          std::chrono::duration_cast<std::chrono::microseconds>(End - Start)
              .count());
      if (I == 0 && !DumpDirectory.empty())
        M.Query = Solver.dumpSMT2();
    } else {
      SolverOptions Options;
      Options.Sat.MaxConflicts = Conflicts;
      Options.Sat.MaxPropagations = uint64_t(1) << 24;
      Options.Sat.MaxWatchVisits = uint64_t(1) << 26;
      BitVectorSolver Solver(Ctx, Options);
      Solver.assertDistinct(A, B);
      Result = equivalence(Solver.check());
      Model = Solver.model();
      M.Times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - Start)
                            .count());
    }
    if (decisive(Result)) {
      if (M.Decided && Result != *M.Decided)
        M.Stable = false;
      M.Decided = Result;
      M.DecisiveTimes.push_back(M.Times.back());
    }
    ++M.VerdictCounts[equivResultName(Result)];
    M.Verdict = Result;
    if (Result == EquivResult::Different) {
      const auto Values = Model.asVarValues(Ctx);
      M.ModelValid &= Ctx.eval(A, Values) != Ctx.eval(B, Values);
    }
  }
  std::sort(M.Times.begin(), M.Times.end());
  std::sort(M.DecisiveTimes.begin(), M.DecisiveTimes.end());
  return M;
}

json::Object report(const Measurement &M) {
  json::Object O{{"verdict", equivResultName(M.Verdict)},
                 {"modelValid", M.ModelValid},
                 {"stable", M.Stable},
                 {"medianMicros", M.Times[M.Times.size() / 2]},
                 {"maxMicros", M.Times.back()}};
  if (!M.Reason.empty())
    O["reason"] = M.Reason;
  json::Object Counts;
  for (const auto &[Name, Count] : M.VerdictCounts)
    Counts[Name] = Count;
  O["verdictCounts"] = std::move(Counts);
  O["allRunsDecisive"] = M.DecisiveTimes.size() == M.Times.size();
  if (!M.DecisiveTimes.empty())
    O["decisiveMedianMicros"] = M.DecisiveTimes[M.DecisiveTimes.size() / 2];
  return O;
}

} // namespace

int main(int Argc, char **Argv) {
  InitLLVM X(Argc, Argv);
  cl::ParseCommandLineOptions(
      Argc, Argv,
      "Compare bounded bitvector proofs; lines are original ; candidate.\n"
      "A line without ';' compares the input with its MBA simplification.\n");
  if (!Repeat || !Width ||
      (Backend != "builtin" && Backend != "z3" && Backend != "both")) {
    errs() << "invalid width, repeat count, or backend\n";
    return 2;
  }
  const bool UseBuiltIn = Backend != "z3";
  const bool UseZ3 = Backend != "builtin";
  if (UseZ3 && !Z3Solver::available()) {
    errs() << "Z3 backend is not enabled in this build\n";
    return 2;
  }
  if (!DumpDirectory.empty()) {
    if (!UseZ3) {
      errs() << "--dump-dir requires the Z3 backend\n";
      return 2;
    }
    if (auto EC = sys::fs::create_directories(DumpDirectory)) {
      errs() << EC.message() << '\n';
      return 2;
    }
  }
  auto File = MemoryBuffer::getFileOrSTDIN(Input);
  if (!File) {
    errs() << File.getError().message() << '\n';
    return 2;
  }
  SmallVector<StringRef, 64> Lines;
  (*File)->getBuffer().split(Lines, '\n');
  json::Array Results;
  unsigned Number = 0;
  unsigned Failures = 0;
  for (StringRef Line : Lines) {
    ++Number;
    Line = Line.trim();
    if (Line.empty() || Line.starts_with("#"))
      continue;
    SymContext Ctx;
    auto [Left, Right] = Line.split(';');
    const bool HasCandidate = Line.contains(';');
    auto A = parseSymExpr(Ctx, Left.trim(), Width);
    auto B = HasCandidate ? parseSymExpr(Ctx, Right.trim(), Width)
                          : SymParseResult{};
    if (!A.ok() || (HasCandidate && !B.ok())) {
      errs() << "line " << Number << ": " << (A.ok() ? B.Error : A.Error)
             << '\n';
      return 2;
    }
    if (!HasCandidate)
      B.Root = simplifyMBADeep(Ctx, A.Root).Expr;
    json::Object Row{{"line", Number},
                     {"input", Ctx.toString(A.Root)},
                     {"candidate", Ctx.toString(B.Root)}};
    Measurement BuiltIn, Z3;
    bool Failed = false;
    if (UseBuiltIn) {
      BuiltIn = measure(Ctx, A.Root, B.Root, false);
      Row["builtin"] = report(BuiltIn);
      Failed |= !BuiltIn.ModelValid || !BuiltIn.Stable ||
                BuiltIn.Verdict == EquivResult::Invalid;
    }
    if (UseZ3) {
      Z3 = measure(Ctx, A.Root, B.Root, true);
      Row["z3"] = report(Z3);
      Failed |=
          !Z3.ModelValid || !Z3.Stable || Z3.Verdict == EquivResult::Invalid;
      if (!DumpDirectory.empty()) {
        SmallString<256> Path(DumpDirectory);
        sys::path::append(Path, "query-" + std::to_string(Number) + ".smt2");
        std::error_code EC;
        raw_fd_ostream Out(Path, EC);
        if (EC) {
          errs() << EC.message() << '\n';
          return 2;
        }
        Out << Z3.Query;
      }
    }
    if (UseBuiltIn && UseZ3 && BuiltIn.Decided && Z3.Decided &&
        BuiltIn.Decided != Z3.Decided)
      Failed = true;
    Failures += Failed;
    Results.push_back(std::move(Row));
  }
  json::Object Output{{"width", Width.getValue()},
                      {"repeats", Repeat.getValue()},
                      {"failures", Failures},
                      {"backend", Backend.getValue()},
                      {"z3Version", Z3Solver::version()},
                      {"z3TimeoutMs", Timeout.getValue()},
                      {"builtinMaxConflicts", uint64_t(Conflicts)},
                      {"builtinMaxPropagations", uint64_t(1) << 24},
                      {"builtinMaxWatchVisits", uint64_t(1) << 26},
                      {"results", std::move(Results)}};
  outs() << formatv("{0:2}\n", json::Value(std::move(Output)));
  return Failures ? 1 : 0;
}
