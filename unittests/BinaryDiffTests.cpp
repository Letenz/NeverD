//===- BinaryDiffTests.cpp - Function correspondence and deltas -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The binary comparison over programs built by hand: identical, rebased,
/// reordered, stripped and renamed builds; constant, branch, call and
/// structure edits; ties that must stay ties; overrides; saved comparisons
/// and their refusal of other inputs; cancellation; and results that do not
/// depend on the number of threads.
///
//===----------------------------------------------------------------------===//

#include "neverd/diff/Comparison.h"
#include "neverd/diff/ComparisonJSON.h"
#include "neverd/diff/FunctionDelta.h"
#include "neverd/diff/Program.h"
#include "neverd/diff/RegionsJSON.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace neverd::diff;

namespace {

uint64_t textHash(llvm::StringRef Text) {
  uint64_t H = 1469598103934665603ULL;
  for (unsigned char C : Text) {
    H ^= C;
    H *= 1099511628211ULL;
  }
  return H ? H : 1;
}

/// One instruction of a test function.  A call names its callee; an
/// operand's digits are its constants.
struct Op {
  std::string Mnemonic;
  std::string Operands;
  InstructionFlow Flow = InstructionFlow::None;
  std::string Callee;
};

struct Fn {
  std::string Name;
  NameSource Source = NameSource::Symbol;
  uint16_t Traits = 0;
  std::string Module;
  std::vector<std::vector<Op>> Blocks;
  std::vector<std::tuple<uint32_t, uint32_t, EdgeKind>> Edges;
  std::vector<std::string> Strings;
};

constexpr uint64_t OpSize = 6;

/// Lays \p Fns out from \p Base in \p Layout order (the listed order when
/// empty) and builds the program.
Program build(const std::vector<Fn> &Fns, uint64_t Base,
              std::vector<size_t> Layout = {}, std::string SHA = "",
              std::string Analysis = "test") {
  if (Layout.empty())
    for (size_t I = 0; I < Fns.size(); ++I)
      Layout.push_back(I);
  llvm::StringMap<uint64_t> Address;
  uint64_t Next = Base, NextImport = Base + 0x100000;
  std::vector<std::pair<uint64_t, size_t>> Placed;
  for (size_t I : Layout) {
    const Fn &F = Fns[I];
    if (F.Traits & TraitImport) {
      Address[F.Name] = NextImport;
      Placed.emplace_back(NextImport, I);
      NextImport += 8;
      continue;
    }
    Address[F.Name] = Next;
    Placed.emplace_back(Next, I);
    uint64_t Count = 0;
    for (const auto &B : F.Blocks)
      Count += B.size();
    Next += Count * OpSize + 16;
  }
  llvm::sort(Placed);
  ProgramIdentity Id;
  Id.Path = "test.bin";
  Id.SHA256 = SHA.empty() ? std::string(64, 'a') : SHA;
  Id.Format = "ELF";
  Id.Architecture = "x86_64";
  Id.Bits = 64;
  Id.Endian = "little";
  Id.ImageBase = Base;
  Id.Analysis = Analysis;
  ProgramBuilder B(Id);
  for (const auto &[At, I] : Placed) {
    const Fn &F = Fns[I];
    uint64_t Count = 0;
    for (const auto &Blk : F.Blocks)
      Count += Blk.size();
    B.beginFunction(At, Count * OpSize, F.Name, F.Source, F.Traits, F.Module);
    uint64_t PC = At;
    for (const auto &Blk : F.Blocks) {
      const uint32_t BI = B.addBlock(PC);
      for (const Op &O : Blk) {
        std::string Skeleton = O.Operands;
        for (char &C : Skeleton)
          if (C >= '0' && C <= '9')
            C = '#';
        std::vector<uint8_t> Bytes(OpSize);
        const uint64_t Opcode = textHash(O.Mnemonic);
        Bytes[0] = uint8_t(Opcode);
        Bytes[1] = uint8_t(Opcode >> 8);
        std::optional<uint64_t> Target;
        uint64_t Payload = textHash(O.Operands);
        uint64_t Shape = textHash(O.Mnemonic + " " + O.Operands);
        uint64_t Skel = textHash(O.Mnemonic + " " + Skeleton);
        if (!O.Callee.empty()) {
          Target = Address.lookup(O.Callee);
          Payload = *Target - (PC + OpSize);
          Shape = Skel = textHash(O.Mnemonic + " <function>");
        }
        for (unsigned K = 0; K < 4; ++K)
          Bytes[2 + K] = uint8_t(Payload >> (8 * K));
        B.addInstruction(PC, Bytes, O.Mnemonic, Shape, Skel, O.Flow, Target);
        PC += OpSize;
      }
      for (const std::string &S : F.Strings)
        if (BI == 0)
          B.addReference(ReferenceKind::String, textHash(S), 0);
    }
    for (const auto &[From, To, Kind] : F.Edges)
      B.addEdge(From, To, Kind);
  }
  return B.finish();
}

std::vector<Op> body(unsigned Seed, unsigned Count) {
  std::vector<Op> Ops;
  for (unsigned I = 0; I < Count; ++I)
    Ops.push_back(
        {I % 3 ? "mov" : "add", "r" + std::to_string((Seed + I) % 7) + ", " +
                                    std::to_string(Seed * 131 + I * 7)});
  return Ops;
}

Fn leaf(std::string Name, unsigned Seed, unsigned Count = 6) {
  Fn F;
  F.Name = std::move(Name);
  auto Ops = body(Seed, Count);
  Ops.push_back({"ret", ""});
  F.Blocks.push_back(std::move(Ops));
  return F;
}

/// if (x == Limit) A else B; join.
Fn diamond(std::string Name, unsigned Seed, unsigned Limit = 10,
           bool Inverted = false) {
  Fn F;
  F.Name = std::move(Name);
  auto Entry = body(Seed, 3);
  Entry.push_back({"cmp", "r1, " + std::to_string(Limit)});
  Entry.push_back({Inverted ? "jnz" : "jz", "", InstructionFlow::Branch});
  auto Then = body(Seed + 1, 4);
  Then.push_back({"jmp", "", InstructionFlow::Jump});
  auto Else = body(Seed + 2, 5);
  auto Join = body(Seed + 3, 2);
  Join.push_back({"ret", "", InstructionFlow::Return});
  if (!Inverted) {
    F.Blocks = {Entry, Then, Else, Join};
    F.Edges = {{0, 1, EdgeKind::Taken},
               {0, 2, EdgeKind::NotTaken},
               {1, 3, EdgeKind::Jump},
               {2, 3, EdgeKind::Fallthrough}};
  } else {
    // The same logic with the condition and the arms' layout swapped.
    auto Else2 = Else;
    Else2.push_back({"jmp", "", InstructionFlow::Jump});
    auto Then2 = Then;
    Then2.pop_back();
    F.Blocks = {Entry, Else2, Then2, Join};
    F.Edges = {{0, 2, EdgeKind::NotTaken},
               {0, 1, EdgeKind::Taken},
               {1, 3, EdgeKind::Jump},
               {2, 3, EdgeKind::Fallthrough}};
  }
  return F;
}

/// A loop calling \p Callees.
Fn caller(std::string Name, unsigned Seed, std::vector<std::string> Callees) {
  Fn F;
  F.Name = std::move(Name);
  auto Head = body(Seed, 2);
  auto Loop = body(Seed + 5, 2);
  for (const std::string &C : Callees)
    Loop.push_back({"call", "", InstructionFlow::Call, C});
  Loop.push_back({"cmp", "r2, 0"});
  Loop.push_back({"jnz", "", InstructionFlow::Branch});
  std::vector<Op> Exit = {{"ret", "", InstructionFlow::Return}};
  F.Blocks = {Head, Loop, Exit};
  F.Edges = {{0, 1, EdgeKind::Fallthrough},
             {1, 1, EdgeKind::Taken},
             {1, 2, EdgeKind::NotTaken}};
  return F;
}

Fn import(std::string Name, std::string Module = "libc.so.6") {
  Fn F;
  F.Name = std::move(Name);
  F.Source = NameSource::Import;
  F.Traits = TraitImport;
  F.Module = std::move(Module);
  return F;
}

/// A small program: imports, leaves, branches and callers.
std::vector<Fn> sample() {
  std::vector<Fn> Fns;
  Fns.push_back(import("malloc"));
  Fns.push_back(import("free"));
  Fns.push_back(import("memcpy"));
  for (unsigned I = 0; I < 6; ++I)
    Fns.push_back(leaf("leaf" + std::to_string(I), 10 + I * 3, 5 + I));
  for (unsigned I = 0; I < 4; ++I)
    Fns.push_back(diamond("check" + std::to_string(I), 50 + I * 4, 10 + I));
  Fn Alloc = caller("alloc_copy", 90, {"malloc", "memcpy", "leaf1"});
  Alloc.Strings = {"out of memory"};
  Fns.push_back(Alloc);
  Fns.push_back(caller("release", 95, {"free", "leaf2"}));
  Fns.push_back(caller(
      "driver", 100,
      {"alloc_copy", "check0", "check1", "release", "check2", "check3"}));
  return Fns;
}

const Function &named(const Program &P, llvm::StringRef Name) {
  for (const Function &F : P.Functions)
    if (F.Name == Name)
      return F;
  ADD_FAILURE() << "no function " << Name.str();
  return P.Functions.front();
}

uint32_t indexOf(const Program &P, llvm::StringRef Name) {
  return uint32_t(&named(P, Name) - P.Functions.data());
}

/// The secondary function's name that \p Name pairs with, or "".
std::string counterpart(const ComparisonResult &R, const Program &P,
                        const Program &S, llvm::StringRef Name) {
  const uint32_t Other = R.secondaryOf(indexOf(P, Name));
  return Other == NoIndex ? std::string() : S.Functions[Other].Name;
}

const FunctionMatch &matchOf(const ComparisonResult &R, const Program &P,
                             llvm::StringRef Name) {
  const FunctionMatch *M = R.matchOfPrimary(indexOf(P, Name));
  EXPECT_NE(M, nullptr) << Name.str();
  static FunctionMatch None;
  return M ? *M : None;
}

std::string saved(const ComparisonResult &R, const Program &P,
                  const Program &S) {
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  writeComparison(OS, R, P, S);
  return Out;
}

/// Saved text without timings, which differ between runs.
std::string savedWithoutTimings(ComparisonResult R, const Program &P,
                                const Program &S) {
  R.Timings.clear();
  return saved(R, P, S);
}

std::vector<Fn> stripped(std::vector<Fn> Fns) {
  llvm::StringMap<bool> Code;
  for (Fn &F : Fns)
    if (!(F.Traits & TraitImport)) {
      Code[F.Name] = true;
      F.Name = "sub_" + F.Name;
      F.Source = NameSource::Generated;
    }
  for (Fn &F : Fns)
    for (auto &Block : F.Blocks)
      for (Op &O : Block)
        if (!O.Callee.empty() && Code.count(O.Callee))
          O.Callee = "sub_" + O.Callee;
  return Fns;
}

} // namespace

TEST(BinaryDiff, ProgramsValidateTheirIndices) {
  Program P = build(sample(), 0x400000);
  EXPECT_FALSE(P.validate());
  Program Broken = P;
  Broken.Functions[5].FirstBlock = uint32_t(Broken.Blocks.size());
  EXPECT_TRUE(Broken.validate());
  Broken = P;
  std::swap(Broken.Functions[3], Broken.Functions[4]);
  EXPECT_TRUE(Broken.validate());
  Broken = P;
  Broken.Instructions[0].Callee = uint32_t(Broken.Functions.size());
  EXPECT_TRUE(Broken.validate());
}

TEST(BinaryDiff, IdenticalInputsHaveNoCodeChanges) {
  const Program P = build(sample(), 0x400000);
  const Program S = build(sample(), 0x400000);
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_EQ(R.Matches.size(), P.Functions.size());
  for (const FunctionMatch &M : R.Matches) {
    EXPECT_EQ(P.Functions[M.Primary].Name, S.Functions[M.Secondary].Name);
    EXPECT_TRUE(M.Measured);
    EXPECT_EQ(M.Changes, 0u) << P.Functions[M.Primary].Name;
    EXPECT_EQ(M.Similarity, M.IdentityOnly ? 0.0 : 1.0);
    EXPECT_EQ(M.identical(), P.Functions[M.Primary].hasBody());
  }
  EXPECT_EQ(R.Similarity, 1.0);
  EXPECT_EQ(R.PrimaryCoverage.Unmatched, 0u);
}

TEST(BinaryDiff, IndistinguishableDuplicatesStayExplicit) {
  // Three byte-identical stubs no code calls: nothing can tell them apart.
  std::vector<Fn> Fns = sample();
  for (unsigned I = 0; I < 3; ++I) {
    Fn Stub = leaf("stub" + std::to_string(I), 7, 1);
    Stub.Source = NameSource::Generated;
    Stub.Name = "sub_stub" + std::to_string(I);
    Fns.push_back(Stub);
  }
  const Program P = build(Fns, 0x400000);
  const Program S = build(Fns, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  for (unsigned I = 0; I < 3; ++I)
    EXPECT_EQ(R.matchOfPrimary(indexOf(P, "sub_stub" + std::to_string(I))),
              nullptr);
  ASSERT_EQ(R.Ambiguities.size(), 1u);
  EXPECT_EQ(R.Ambiguities[0].Primary.size(), 3u);
  EXPECT_EQ(R.Ambiguities[0].Secondary.size(), 3u);
  EXPECT_EQ(R.PrimaryCoverage.Ambiguous, 3u);
  // Every function that can be told apart still pairs without changes.
  for (const FunctionMatch &M : R.Matches)
    EXPECT_EQ(M.Changes, 0u);
}

TEST(BinaryDiff, ContextTellsApartDuplicatesItCalls) {
  // Two identical helpers, each called from one place of a unique caller.
  std::vector<Fn> Fns = sample();
  Fns.push_back(leaf("helper_a", 3, 2));
  Fns.push_back(leaf("helper_b", 3, 2));
  Fns.push_back(caller("dispatch", 120, {"helper_a", "leaf3", "helper_b"}));
  for (Fn &F : Fns)
    if (F.Name.rfind("helper", 0) == 0)
      F.Source = NameSource::Generated;
  const Program P = build(Fns, 0x400000);
  const Program S = build(Fns, 0x500000, {});
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(counterpart(R, P, S, "helper_a"), "helper_a");
  EXPECT_EQ(counterpart(R, P, S, "helper_b"), "helper_b");
  EXPECT_TRUE(R.Ambiguities.empty());
}

TEST(BinaryDiff, RebasedAndReorderedFunctionsOnlyMove) {
  const std::vector<Fn> Fns = sample();
  std::vector<size_t> Reversed;
  for (size_t I = Fns.size(); I-- > 0;)
    Reversed.push_back(I);
  const Program P = build(Fns, 0x400000);
  const Program S = build(stripped(Fns), 0x7ff000000, Reversed);
  const ComparisonResult R = compare(P, S, {});
  for (const Function &F : P.Functions) {
    const std::string Other = counterpart(R, P, S, F.Name);
    const bool Import = F.has(TraitImport);
    EXPECT_EQ(Other, Import ? F.Name : "sub_" + F.Name);
    const FunctionMatch &M = matchOf(R, P, F.Name);
    EXPECT_EQ(M.identical(), !Import)
        << F.Name << " " << changeLetters(M.Changes);
    EXPECT_EQ(M.IdentityOnly, Import);
    EXPECT_EQ(M.Similarity, Import ? 0.0 : 1.0) << F.Name;
  }
}

TEST(BinaryDiff, StrippedBuildsPairWithoutNames) {
  const std::vector<Fn> Fns = sample();
  const Program P = build(stripped(Fns), 0x400000);
  const Program S = build(stripped(Fns), 0x400000);
  ComparisonSettings NoNames;
  NoNames.UseNames = false;
  for (const ComparisonSettings &Settings : {ComparisonSettings(), NoNames}) {
    const ComparisonResult R = compare(P, S, Settings);
    for (const Function &F : P.Functions)
      EXPECT_EQ(counterpart(R, P, S, F.Name), F.Name);
  }
}

TEST(BinaryDiff, DuplicateNamesAreNotCollapsed) {
  std::vector<Fn> Fns = sample();
  Fn A = leaf("dup", 300, 7), B = diamond("dup", 400);
  B.Name = "dup";
  Fns.push_back(A);
  Fns.push_back(B);
  const Program P = build(Fns, 0x400000);
  const Program S = build(Fns, 0x480000);
  const ComparisonResult R = compare(P, S, {});
  unsigned Dups = 0;
  for (const FunctionMatch &M : R.Matches)
    if (P.Functions[M.Primary].Name == "dup") {
      ++Dups;
      EXPECT_EQ(P.Functions[M.Primary].BlockCount,
                S.Functions[M.Secondary].BlockCount);
      EXPECT_TRUE(M.identical());
    }
  EXPECT_EQ(Dups, 2u);
}

TEST(BinaryDiff, SameSizeConstantEditStaysVisible) {
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "check2")
      F = diamond("check2", 58, 13); // was 12
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  const FunctionMatch &M = matchOf(R, P, "check2");
  EXPECT_EQ(counterpart(R, P, S, "check2"), "check2");
  EXPECT_TRUE(M.Changes & ChangeConstants) << changeLetters(M.Changes);
  EXPECT_FALSE(M.Changes & ChangeBlocks);
  EXPECT_FALSE(M.Changes & ChangeEdges);
  EXPECT_FALSE(M.identical());
  EXPECT_LT(M.Similarity, 1.0);
  // The aligned listing names the changed instruction.
  std::vector<uint32_t> PMap(P.Functions.size(), NoIndex),
      SMap(S.Functions.size(), NoIndex);
  for (const FunctionMatch &Pair : R.Matches) {
    PMap[Pair.Primary] = Pair.Secondary;
    SMap[Pair.Secondary] = Pair.Primary;
  }
  const FunctionDelta D =
      compareFunctions(P, M.Primary, S, M.Secondary, {PMap, SMap});
  unsigned Constants = 0;
  for (const DeltaRow &Row : D.Rows)
    if (Row.Kind == RowKind::Constant) {
      ++Constants;
      EXPECT_EQ(P.mnemonic(P.Instructions[Row.Primary]), "cmp");
    }
  EXPECT_EQ(Constants, 1u);
}

TEST(BinaryDiff, BranchInversionStaysVisible) {
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "check1")
      F = diamond("check1", 54, 11, /*Inverted=*/true);
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(counterpart(R, P, S, "check1"), "check1");
  const FunctionMatch &M = matchOf(R, P, "check1");
  EXPECT_TRUE(M.Changes & ChangeBranchInversion) << changeLetters(M.Changes);
  EXPECT_FALSE(M.identical());
}

TEST(BinaryDiff, AddedAndRemovedFunctionsStayUnmatched) {
  std::vector<Fn> Old = sample(), New = sample();
  Old.push_back(leaf("removed_only", 600, 9));
  New.push_back(diamond("added_only", 700, 3));
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(R.matchOfPrimary(indexOf(P, "removed_only")), nullptr);
  EXPECT_EQ(R.matchOfSecondary(indexOf(S, "added_only")), nullptr);
  EXPECT_EQ(R.PrimaryCoverage.Unmatched, 1u);
  EXPECT_EQ(R.SecondaryCoverage.Unmatched, 1u);
}

TEST(BinaryDiff, ChangedCalleesAreReported) {
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "release")
      F = caller("release", 95, {"free", "leaf4"}); // was leaf2
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  const FunctionMatch &M = matchOf(R, P, "release");
  EXPECT_EQ(counterpart(R, P, S, "release"), "release");
  EXPECT_TRUE(M.Changes & ChangeCalls) << changeLetters(M.Changes);
}

TEST(BinaryDiff, ChangedBodiesPairBySimilarityWhenStripped) {
  // Without names, an edited function still pairs through its callers or
  // its similarity, and its edit shows.
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "leaf5") {
      F.Blocks[0].insert(F.Blocks[0].begin() + 2, {"xor", "r3, r3"});
      F.Blocks[0][4].Operands = "r4, 999";
    }
  const Program P = build(stripped(Old), 0x400000);
  const Program S = build(stripped(New), 0x400000);
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(counterpart(R, P, S, "sub_leaf5"), "sub_leaf5");
  const FunctionMatch &M = matchOf(R, P, "sub_leaf5");
  EXPECT_TRUE(M.Changes & ChangeInstructions) << changeLetters(M.Changes);
  EXPECT_LT(M.Similarity, 1.0);
}

TEST(BinaryDiff, SimilarStructureWithOtherInstructionsIsNotIdentical) {
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "check3")
      for (auto &Block : F.Blocks)
        for (Op &O : Block)
          if (O.Mnemonic == "mov")
            O.Mnemonic = "sub";
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  const FunctionMatch &M = matchOf(R, P, "check3");
  EXPECT_FALSE(M.identical());
  EXPECT_LT(M.Similarity, 1.0);
  EXPECT_TRUE(M.Changes & ChangeInstructions);
}

TEST(BinaryDiff, ResultsDoNotDependOnThreadCount) {
  std::vector<Fn> Old = sample(), New = sample();
  for (unsigned I = 0; I < 40; ++I) {
    Old.push_back(leaf("bulk" + std::to_string(I), 1000 + I, 4 + I % 5));
    New.push_back(
        leaf("bulk" + std::to_string(I), 1000 + I + (I % 7 == 0), 4 + I % 5));
  }
  const Program P = build(stripped(Old), 0x400000);
  const Program S = build(stripped(New), 0x900000);
  std::string Reference;
  for (unsigned Threads : {1u, 2u, 8u}) {
    neverd::setWorkerThreadCount(Threads);
    const std::string Text = savedWithoutTimings(compare(P, S, {}), P, S);
    if (Reference.empty())
      Reference = Text;
    EXPECT_EQ(Text, Reference) << Threads << " threads";
  }
  neverd::setWorkerThreadCount(0);
}

TEST(BinaryDiff, SavedComparisonsRoundTripAndRefuseOtherInputs) {
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "check2")
      F = diamond("check2", 58, 13);
  const Program P = build(Old, 0x400000, {}, std::string(64, 'b'));
  const Program S = build(New, 0x400000, {}, std::string(64, 'c'));
  ComparisonResult R = compare(P, S, {});
  ASSERT_FALSE(applyOverride(R, P, S,
                             {OverrideKind::Confirm, named(P, "leaf0").Address,
                              named(S, "leaf0").Address}));
  const std::string Text = saved(R, P, S);

  std::string Error;
  const auto Read = readComparison(Text, Error);
  ASSERT_TRUE(Read) << Error;
  ComparisonResult Bound;
  ASSERT_FALSE(bindComparison(*Read, P, S, Bound));
  EXPECT_EQ(savedWithoutTimings(Bound, P, S), savedWithoutTimings(R, P, S));
  EXPECT_EQ(Bound.Overrides.size(), 1u);
  EXPECT_TRUE(matchOf(Bound, P, "leaf0").Confirmed);

  // Another input, another analysis, or a function the analysis lost.
  BindFailure Why;
  const Program Other = build(Old, 0x400000, {}, std::string(64, 'd'));
  EXPECT_TRUE(bindComparison(*Read, Other, S, Bound, &Why));
  EXPECT_EQ(Why, BindFailure::StaleInput);
  const Program Reanalyzed =
      build(Old, 0x400000, {}, std::string(64, 'b'), "other settings");
  EXPECT_TRUE(bindComparison(*Read, Reanalyzed, S, Bound, &Why));
  EXPECT_EQ(Why, BindFailure::StaleAnalysis);
  std::vector<Fn> Fewer = Old;
  llvm::erase_if(Fewer, [](const Fn &F) { return F.Name == "leaf3"; });
  const Program Lost = build(Fewer, 0x400000, {}, std::string(64, 'b'));
  EXPECT_TRUE(bindComparison(*Read, Lost, S, Bound, &Why));
}

TEST(BinaryDiff, MalformedSavedComparisonsFailClearly) {
  const Program P = build(sample(), 0x400000);
  const Program S = build(sample(), 0x400000);
  const std::string Good = saved(compare(P, S, {}), P, S);
  std::string Error;
  ASSERT_TRUE(readComparison(Good, Error)) << Error;
  const auto Rejects = [&](std::string Text, llvm::StringRef Expected) {
    std::string E;
    EXPECT_FALSE(readComparison(Text, E));
    EXPECT_NE(E.find(Expected.str()), std::string::npos)
        << "error: " << E << "\nexpected: " << Expected.str();
  };
  const auto Replace = [&](llvm::StringRef From, llvm::StringRef To) {
    std::string Text = Good;
    const size_t At = Text.find(From.str());
    EXPECT_NE(At, std::string::npos) << From.str();
    if (At != std::string::npos)
      Text.replace(At, From.size(), To.str());
    return Text;
  };
  Rejects("", "not JSON");
  Rejects("[]", "must be a JSON object");
  Rejects("{\"schema\":\"other\"}", "not a saved comparison");
  Rejects(Replace("\"schema_version\":3", "\"schema_version\":99"),
          "version 99 is not supported");
  Rejects(Replace("\"state\":\"complete\"", "\"state\":\"maybe\""),
          "\"state\"");
  Rejects(Replace("\"step\":\"", "\"step\":\"bogus_"), "unknown matching step");
  Rejects(Replace("\"similarity\":1", "\"similarity\":7"), "\"similarity\"");
  Rejects(Replace("\"max_candidates\":16", "\"max_candidates\":0"),
          "candidates");
  Rejects(Replace("\"primary\":\"0x", "\"primary\":\"0xZZ"), "hexadecimal");
  Rejects(Replace("\"sha256\":\"aaaa", "\"sha256\":\"zzzz"), "sha256");
  Rejects(
      Replace("\"functions\":{\"primary\":[", "\"functions\":{\"primary\":[5,"),
      "a function must be");
}

TEST(BinaryDiff, OverridesKeepPairsOneToOneAndRevert) {
  const Program P = build(sample(), 0x400000);
  const Program S = build(sample(), 0x400000);
  ComparisonResult R = compare(P, S, {});
  const std::string Before = savedWithoutTimings(R, P, S);
  // Pair leaf0 with leaf1's counterpart: both former partners come free.
  OverrideEffect Effect;
  ASSERT_FALSE(applyOverride(R, P, S,
                             {OverrideKind::Match, named(P, "leaf0").Address,
                              named(S, "leaf1").Address},
                             &Effect));
  EXPECT_EQ(counterpart(R, P, S, "leaf0"), "leaf1");
  EXPECT_EQ(R.matchOfPrimary(indexOf(P, "leaf1")), nullptr);
  EXPECT_EQ(R.matchOfSecondary(indexOf(S, "leaf0")), nullptr);
  EXPECT_EQ(matchOf(R, P, "leaf0").Step, FunctionStep::Manual);
  EXPECT_TRUE(matchOf(R, P, "leaf0").Confirmed);
  EXPECT_EQ(R.Overrides.size(), 1u);
  revertOverride(R, P, S, Effect);
  EXPECT_EQ(savedWithoutTimings(R, P, S), Before);

  // Unpairing and confirming need an existing pair.
  EXPECT_TRUE(applyOverride(R, P, S,
                            {OverrideKind::Unmatch, named(P, "leaf0").Address,
                             named(S, "leaf1").Address}));
  EXPECT_TRUE(applyOverride(
      R, P, S, {OverrideKind::Confirm, named(P, "leaf0").Address, 0x1234}));
  ASSERT_FALSE(applyOverride(R, P, S,
                             {OverrideKind::Unmatch, named(P, "leaf2").Address,
                              named(S, "leaf2").Address},
                             &Effect));
  EXPECT_EQ(R.matchOfPrimary(indexOf(P, "leaf2")), nullptr);
  revertOverride(R, P, S, Effect);
  EXPECT_EQ(counterpart(R, P, S, "leaf2"), "leaf2");

  // Comparing again with the overrides reproduces them.
  const std::vector<Override> Decisions = {
      {OverrideKind::Match, named(P, "leaf3").Address,
       named(S, "leaf4").Address},
      {OverrideKind::Unmatch, named(P, "check0").Address,
       named(S, "check0").Address}};
  const ComparisonResult Again = compare(P, S, {}, Decisions);
  EXPECT_EQ(counterpart(Again, P, S, "leaf3"), "leaf4");
  EXPECT_NE(counterpart(Again, P, S, "check0"), "check0");
  EXPECT_EQ(Again.Overrides, Decisions);
}

TEST(BinaryDiff, StoppedComparisonsKeepWhatTheyHave) {
  const Program P = build(sample(), 0x400000);
  const Program S = build(sample(), 0x400000);
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  const ComparisonResult R = compare(P, S, {}, {}, Control);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_FALSE(R.StoppedPhase.empty());
  for (const FunctionMatch &M : R.Matches)
    EXPECT_EQ(P.Functions[M.Primary].Name, S.Functions[M.Secondary].Name);

  ComparisonControl Late;
  Late.Deadline = std::chrono::steady_clock::now();
  EXPECT_EQ(compare(P, S, {}, {}, Late).State, ComparisonState::TimedOut);

  std::vector<std::string> Phases;
  ComparisonControl Watch;
  Watch.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phases.empty() || Phases.back() != Phase)
      Phases.push_back(Phase.str());
  };
  EXPECT_EQ(compare(P, S, {}, {}, Watch).State, ComparisonState::Complete);
  EXPECT_FALSE(Phases.empty());
}

TEST(BinaryDiff, SavedMatchesCannotContradictEffectiveUnmatches) {
  const auto P = build({leaf("a", 1, 3), leaf("b", 2, 4)}, 0x400000);
  const auto S = build({leaf("a", 1, 3), leaf("b", 2, 4)}, 0x800000);
  auto Original = compare(P, S, {});
  const auto Before = saved(Original, P, S);
  std::string Error;
  const auto Read = readComparison(Before, Error);
  ASSERT_TRUE(Read) << Error;
  const auto A = named(P, "a").Address, B = named(S, "a").Address;
  for (const auto History :
       {std::vector<Override>{{OverrideKind::Unmatch, A, B}},
        std::vector<Override>{{OverrideKind::Unmatch, A, B},
                              {OverrideKind::Confirm, A, B}},
        std::vector<Override>{{OverrideKind::Match, A, B},
                              {OverrideKind::Unmatch, A, B}}}) {
    for (const auto State :
         {ComparisonState::Complete, ComparisonState::Cancelled,
          ComparisonState::TimedOut, ComparisonState::ResourceLimited}) {
      auto Forged = Original;
      Forged.Overrides = History;
      Forged.State = State;
      EXPECT_FALSE(readComparison(saved(Forged, P, S), Error));
      EXPECT_NE(Error.find("effective unmatch"), std::string::npos) << Error;

      auto Detached = *Read;
      Detached.Overrides = History;
      Detached.State = State;
      BindFailure Why;
      const auto Failure = bindComparison(Detached, P, S, Original, &Why);
      ASSERT_TRUE(Failure);
      EXPECT_NE(Failure->find("effective unmatch"), std::string::npos);
      EXPECT_EQ(Why, BindFailure::InvalidEvidence);
      EXPECT_EQ(saved(Original, P, S), Before);
    }
  }
}

TEST(BinaryDiff, SavedDecisionsRetainRemappingAndUndoSemantics) {
  const auto P = build({leaf("a", 1, 3), leaf("b", 2, 4)}, 0x400000);
  const auto S = build({leaf("a", 1, 3), leaf("b", 2, 4)}, 0x800000);
  const auto A = named(P, "a").Address, D = named(P, "b").Address;
  const auto B = named(S, "a").Address, C = named(S, "b").Address;
  const auto RoundTrip = [&](const ComparisonResult &Result) {
    const auto Text = saved(Result, P, S);
    std::string Error;
    const auto Read = readComparison(Text, Error);
    ASSERT_TRUE(Read) << Error;
    ComparisonResult Bound;
    ASSERT_FALSE(bindComparison(*Read, P, S, Bound));
    EXPECT_EQ(saved(Bound, P, S), Text);
  };
  for (const auto History :
       {std::vector<Override>{{OverrideKind::Unmatch, A, B},
                              {OverrideKind::Match, A, C},
                              {OverrideKind::Match, D, B}},
        std::vector<Override>{{OverrideKind::Unmatch, A, B},
                              {OverrideKind::Confirm, A, B}},
        std::vector<Override>{{OverrideKind::Unmatch, A, B},
                              {OverrideKind::Match, A, B},
                              {OverrideKind::Match, A, C}},
        std::vector<Override>{{OverrideKind::Unmatch, A, B},
                              {OverrideKind::Match, A, C},
                              {OverrideKind::Match, D, B},
                              {OverrideKind::Match, A, B}}}) {
    RoundTrip(compare(P, S, {}, History));
  }
  auto Result = compare(P, S, {});
  const auto Before = saved(Result, P, S);
  std::vector<OverrideEffect> Effects;
  for (const auto Decision : {Override{OverrideKind::Unmatch, A, B},
                              Override{OverrideKind::Match, A, B},
                              Override{OverrideKind::Match, A, C}}) {
    Effects.emplace_back();
    ASSERT_FALSE(applyOverride(Result, P, S, Decision, &Effects.back()));
    RoundTrip(Result);
  }
  while (!Effects.empty()) {
    revertOverride(Result, P, S, Effects.back());
    Effects.pop_back();
    RoundTrip(Result);
  }
  EXPECT_EQ(saved(Result, P, S), Before);
}

TEST(BinaryDiff, EffectiveRejectionsAgreeWithReverseHistoryOracle) {
  // Every sequence of five decisions on two pairs, including shared endpoints.
  constexpr unsigned Sequences = 6 * 6 * 6 * 6 * 6;
  const std::pair<uint64_t, uint64_t> Pairs[] = {{10, 20}, {10, 30}};
  for (unsigned Sequence = 0; Sequence < Sequences; ++Sequence) {
    unsigned Code = Sequence;
    std::vector<Override> History;
    for (unsigned I = 0; I < 5; ++I, Code /= 6) {
      const auto &Pair = Pairs[Code % 2];
      History.push_back(
          {static_cast<OverrideKind>((Code % 6) / 2), Pair.first, Pair.second});
    }
    std::vector<std::pair<uint64_t, uint64_t>> Expected;
    for (const auto &Pair : Pairs) {
      for (auto It = History.rbegin(); It != History.rend(); ++It) {
        if (It->Primary != Pair.first || It->Secondary != Pair.second ||
            It->Kind == OverrideKind::Confirm)
          continue;
        if (It->Kind == OverrideKind::Unmatch)
          Expected.push_back(Pair);
        break;
      }
    }
    EXPECT_EQ(rejectedPairs(History), Expected) << Sequence;
  }
  std::atomic<bool> Cancel = true;
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  EXPECT_THROW(rejectedPairs({}, Control), ComparisonInterrupted);
  Cancel = false;
  Control.Deadline = std::chrono::steady_clock::now();
  EXPECT_THROW(rejectedPairs({}, Control), ComparisonInterrupted);
}

TEST(BinaryDiff, LibraryCodeDoesNotDecideTheProgramScore) {
  std::vector<Fn> Old = sample(), New = sample();
  Fn Big = leaf("runtime_init", 5000, 400),
     BigNew = leaf("runtime_init", 6000, 400);
  Big.Traits = BigNew.Traits = TraitLibrary;
  Old.push_back(Big);
  New.push_back(BigNew);
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(R.Similarity, 1.0);
  EXPECT_EQ(R.PrimaryCoverage.Library, 1u);
}

TEST(BinaryDiff, ImportsPairBySymbol) {
  std::vector<Fn> Old = sample(), New = sample();
  New.push_back(import("strlen"));
  std::vector<size_t> Order;
  for (size_t I = New.size(); I-- > 0;)
    Order.push_back(I);
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000, Order);
  const ComparisonResult R = compare(P, S, {});
  for (llvm::StringRef Name : {"malloc", "free", "memcpy"}) {
    EXPECT_EQ(counterpart(R, P, S, Name), Name.str());
    EXPECT_EQ(matchOf(R, P, Name).Step, FunctionStep::ImportName);
    EXPECT_TRUE(matchOf(R, P, Name).IdentityOnly);
    EXPECT_FALSE(matchOf(R, P, Name).identical());
  }
  EXPECT_EQ(R.matchOfSecondary(indexOf(S, "strlen")), nullptr);
}

TEST(BinaryDiff, BodilessPairsNeverClaimCodeEquivalence) {
  const auto P = build({import("external"), leaf("body", 3, 5)}, 0x400000);
  const auto S = build({import("external"), leaf("body", 3, 5)}, 0x800000);
  auto R = compare(P, S, {});
  auto D = compareFunctions(P, indexOf(P, "external"), S,
                            indexOf(S, "external"), {});
  EXPECT_TRUE(D.IdentityOnly);
  EXPECT_FALSE(D.AnalysisComplete);
  EXPECT_TRUE(D.Rows.empty());
  OverrideEffect Effect;
  ASSERT_FALSE(applyOverride(R, P, S,
                             {OverrideKind::Match, named(P, "external").Address,
                              named(S, "body").Address},
                             &Effect));
  const auto &M = matchOf(R, P, "external");
  EXPECT_TRUE(M.Measured);
  EXPECT_FALSE(M.IdentityOnly);
  EXPECT_FALSE(M.AnalysisComplete);
  EXPECT_FALSE(M.identical());
  EXPECT_NE(M.Changes & ~MovementChanges, 0u);
  EXPECT_EQ(M.EqualInstructions, 0u);
  EXPECT_LT(M.Similarity, 1.0);
  revertOverride(R, P, S, Effect);
  EXPECT_TRUE(matchOf(R, P, "external").IdentityOnly);
  EXPECT_TRUE(matchOf(R, P, "body").identical());
}

TEST(BinaryDiff, SavedMeasurementsAreRecomputedBeforePublication) {
  const auto P = build({leaf("body", 3, 5), import("external")}, 0x400000);
  const auto S = build({leaf("body", 7, 5), import("external")}, 0x800000);
  auto R = compare(P, S, {});
  const auto Text = saved(R, P, S);
  std::string Error;
  const auto Read = readComparison(Text, Error);
  ASSERT_TRUE(Read) << Error;
  const auto Rejected = [&](auto Mutate) {
    auto Forged = *Read;
    Mutate(Forged.Matches.front().Match);
    BindFailure Why;
    auto Failure = bindComparison(Forged, P, S, R, &Why);
    EXPECT_TRUE(Failure);
    EXPECT_EQ(Why, BindFailure::InvalidEvidence);
    EXPECT_EQ(saved(R, P, S), Text);
  };
  Rejected([](auto &M) { M.Changes = 0; });
  Rejected([](auto &M) { M.AnalysisComplete = false; });
  Rejected([](auto &M) { M.IdentityOnly = true; });
  Rejected([](auto &M) { M.Similarity = 1; });
  Rejected([](auto &M) { ++M.MatchedBlocks; });
  Rejected([](auto &M) { ++M.MatchedEdges; });
  Rejected([](auto &M) { ++M.EqualInstructions; });
  Rejected([](auto &M) { M.Measured = false; });
  // A stopped comparison may retain unmeasured correspondence, without claims.
  auto Partial = *Read;
  Partial.State = ComparisonState::Cancelled;
  auto &M = Partial.Matches.front().Match;
  M = FunctionMatch{};
  ASSERT_FALSE(bindComparison(Partial, P, S, R));
  EXPECT_FALSE(R.Matches.front().Measured);
  EXPECT_FALSE(R.Matches.front().identical());
}

TEST(BinaryDiff, VocabularyRoundTrips) {
  for (unsigned I = 0; I <= unsigned(FunctionStep::Similarity); ++I) {
    const auto Step = static_cast<FunctionStep>(I);
    EXPECT_EQ(functionStepNamed(stepName(Step)), Step);
    EXPECT_FALSE(stepDescription(Step).empty());
  }
  for (unsigned I = 0; I <= unsigned(BlockStep::BlockOrder); ++I)
    EXPECT_EQ(blockStepNamed(stepName(static_cast<BlockStep>(I))),
              static_cast<BlockStep>(I));
  for (uint32_t Bit = 0; Bit < 32; ++Bit) {
    const uint32_t Kind = 1u << Bit;
    if (!(AllChanges & Kind))
      continue;
    const auto Names = changeNames(Kind);
    ASSERT_EQ(Names.size(), 1u);
    EXPECT_EQ(changeNamed(Names[0]), static_cast<ChangeKind>(Kind));
  }
  EXPECT_EQ(changeLetters(0).find_first_not_of('.'), std::string::npos);
  for (RowKind K :
       {RowKind::Identical, RowKind::Moved, RowKind::Constant, RowKind::Operand,
        RowKind::Replaced, RowKind::Inserted, RowKind::Deleted})
    EXPECT_EQ(rowKindNamed(rowKindName(K)), K);
}

TEST(BinaryDiff, CancelledDeltaIsNotMeasuredAsUnchanged) {
  const Program P = build(sample(), 0x400000);
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  const auto D = compareFunctions(P, 0, P, 0, {}, Control);
  EXPECT_EQ(D.State, ComparisonState::Cancelled);
  EXPECT_FALSE(D.ComputationComplete);
  EXPECT_FALSE(D.AnalysisComplete);
  EXPECT_TRUE(D.Rows.empty());
  ComparisonControl Expired;
  Expired.Deadline = std::chrono::steady_clock::now();
  const auto TimedOut = compareFunctions(P, 0, P, 0, {}, Expired);
  EXPECT_EQ(TimedOut.State, ComparisonState::TimedOut);
  EXPECT_FALSE(TimedOut.ComputationComplete);
  Cancel = false;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t Done, uint64_t) {
    if (Phase == "features" && Done == 1)
      Cancel = true;
  };
  const auto R = compare(P, P, {}, {}, Control);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_EQ(R.StoppedPhase, "features");
  EXPECT_TRUE(R.Matches.empty());
  EXPECT_EQ(R.PrimaryCoverage.Unmatched, P.Functions.size());
}

namespace {
RegionSettings enabledRegions() {
  RegionSettings S;
  S.Enabled = true;
  return S;
}
bool explains(const RegionHypothesis &H, RegionKind Kind) {
  return llvm::is_contained(H.Explanations, Kind);
}
} // namespace

TEST(BinaryDiff, RegionSplitAndMergePreserveManyToManyCoordinates) {
  Fn Whole;
  Whole.Name = "whole";
  Whole.Blocks = {body(10, 6), body(20, 7)};
  Whole.Edges = {{0, 1, EdgeKind::Fallthrough}};
  const auto A = build({Whole}, 0x1000);
  const auto B = build({leaf("part_a", 10, 6), leaf("part_b", 20, 7)}, 0x8000);
  const auto R = findRegions(A, B, enabledRegions(), {});
  ASSERT_EQ(R.State, ComparisonState::Complete);
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  const auto &H = R.Hypotheses[0];
  EXPECT_TRUE(explains(H, RegionKind::Split));
  EXPECT_FALSE(explains(H, RegionKind::Clone));
  EXPECT_EQ(H.PrimaryFunctions.size(), 1u);
  EXPECT_EQ(H.SecondaryFunctions.size(), 2u);
  EXPECT_EQ(H.PrimaryInstructions, 13u);
  EXPECT_EQ(H.PrimaryCoverage, 1);
  EXPECT_TRUE(H.Ambiguous);
  EXPECT_FALSE(validateRegions(R, enabledRegions(), A, B));
  const auto Back = findRegions(B, A, enabledRegions(), {});
  ASSERT_EQ(Back.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(Back.Hypotheses[0], RegionKind::Merge));
}

TEST(BinaryDiff, RegionInlineAndOutlineUseObservedInternalCalls) {
  Fn Caller;
  Caller.Name = "caller";
  Caller.Blocks = {body(40, 5)};
  Caller.Blocks[0].push_back({"call", "", InstructionFlow::Call, "callee"});
  const auto After = body(50, 5);
  Caller.Blocks[0].insert(Caller.Blocks[0].end(), After.begin(), After.end());
  Fn Callee = leaf("callee", 60, 6);
  Callee.Blocks[0].back().Flow = InstructionFlow::Return;
  Fn Inlined;
  Inlined.Name = "caller";
  Inlined.Blocks = {body(40, 5)};
  const auto Middle = body(60, 6);
  Inlined.Blocks[0].insert(Inlined.Blocks[0].end(), Middle.begin(),
                           Middle.end());
  Inlined.Blocks[0].insert(Inlined.Blocks[0].end(), After.begin(), After.end());
  const auto A = build({Caller, Callee}, 0x1000), B = build({Inlined}, 0x8000);
  const auto R = findRegions(A, B, enabledRegions(), {});
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::Inline));
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::Merge));
  EXPECT_EQ(R.Hypotheses[0].PrimaryInstructions, 16u);
  const auto Reverse = findRegions(B, A, enabledRegions(), {});
  ASSERT_EQ(Reverse.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(Reverse.Hypotheses[0], RegionKind::Outline));
}

TEST(BinaryDiff, RegionClonesRemainCompetingOverlappingEvidence) {
  const auto A = build({leaf("original", 71, 8)}, 0x1000);
  const auto B =
      build({leaf("clone_a", 71, 8), leaf("clone_b", 71, 8)}, 0x8000);
  const auto R = findRegions(A, B, enabledRegions(), {});
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  const auto &H = R.Hypotheses[0];
  EXPECT_TRUE(explains(H, RegionKind::Clone));
  EXPECT_TRUE(H.Ambiguous);
  EXPECT_EQ(H.PrimaryInstructions, 9u);
  EXPECT_EQ(H.SecondaryInstructions, 18u);
  EXPECT_EQ(H.Anchors.size(), 2u);
  EXPECT_LE(H.Confidence, 0.5);
}

TEST(BinaryDiff, RegionSharedTailsAndModuleMovementAreHypotheses) {
  const auto Tail = [] {
    auto Ops = body(99, 6);
    Ops.push_back({"ret", "", InstructionFlow::Return});
    return Ops;
  }();
  Fn One;
  One.Name = "one";
  One.Module = "old_module";
  One.Blocks = {body(10, 6), Tail};
  Fn Two;
  Two.Name = "two";
  Two.Module = "old_module";
  Two.Blocks = {body(20, 6), Tail};
  Fn Joined;
  Joined.Name = "joined";
  Joined.Module = "new_module";
  Joined.Blocks = {body(10, 6), body(20, 6), Tail};
  const auto R = findRegions(build({One, Two}, 0x1000), build({Joined}, 0x8000),
                             enabledRegions(), {});
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::TailMerge));
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::ModuleMove));
  EXPECT_EQ(R.Hypotheses[0].PrimaryInstructions, 24u);
  EXPECT_EQ(R.Hypotheses[0].SecondaryInstructions, 18u);
}

TEST(BinaryDiff, RegionFrequentCodeAbstainsAndBudgetsStopDeterministically) {
  std::vector<Fn> Repeated;
  for (unsigned I = 0; I < 20; ++I)
    Repeated.push_back(leaf("copy" + std::to_string(I), 1, 8));
  const auto A = build(Repeated, 0x1000), B = build(Repeated, 0x8000);
  const auto R = findRegions(A, B, enabledRegions(), {});
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_TRUE(R.Hypotheses.empty());
  EXPECT_GT(R.FrequentSeedGroups, 0u);
  auto Settings = enabledRegions();
  Settings.MaxSeeds = 2;
  const auto Limited = findRegions(A, B, Settings, {});
  EXPECT_EQ(Limited.State, ComparisonState::ResourceLimited);
  EXPECT_EQ(Limited.PrimarySeeds, 2u);
  EXPECT_TRUE(Limited.Anchors.empty());
  Settings = enabledRegions();
  Settings.MaxWork = 20;
  const auto First = findRegions(A, B, Settings, {}),
             Second = findRegions(A, B, Settings, {});
  EXPECT_EQ(First.State, ComparisonState::ResourceLimited);
  EXPECT_EQ(First.Work, 20u);
  EXPECT_EQ(First.Anchors, Second.Anchors);
}

TEST(BinaryDiff, RegionCancellationDoesNotInventCompletedHypotheses) {
  const auto A = build({leaf("fn", 5, 20)}, 0x1000);
  std::atomic<bool> Cancel{false};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "regions:anchors")
      Cancel = true;
  };
  const auto R = findRegions(A, A, enabledRegions(), Control);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_EQ(R.StoppedPhase, "regions:anchors");
  EXPECT_TRUE(R.Anchors.empty());
  EXPECT_TRUE(R.Hypotheses.empty());
}

TEST(BinaryDiff, RegionRedistributionRetainsMultipleFunctionsOnBothSides) {
  Fn A;
  A.Name = "a";
  A.Blocks = {body(10, 5), body(20, 5)};
  Fn B;
  B.Name = "b";
  B.Blocks = {body(30, 5), body(40, 5)};
  Fn C;
  C.Name = "c";
  C.Blocks = {body(10, 5), body(30, 5)};
  Fn D;
  D.Name = "d";
  D.Blocks = {body(20, 5), body(40, 5)};
  const auto P = build({A, B}, 0x1000), S = build({C, D}, 0x8000);
  const auto R = findRegions(P, S, enabledRegions(), {});
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  const auto &H = R.Hypotheses[0];
  EXPECT_TRUE(explains(H, RegionKind::Redistribution));
  EXPECT_EQ(H.PrimaryFunctions.size(), 2u);
  EXPECT_EQ(H.SecondaryFunctions.size(), 2u);
  EXPECT_EQ(H.Anchors.size(), 4u);
  EXPECT_EQ(H.PrimaryInstructions, 20u);
  EXPECT_EQ(H.SecondaryInstructions, 20u);
  EXPECT_FALSE(validateRegions(R, enabledRegions(), P, S));
}

TEST(BinaryDiff, RegionTailMergingInsideOneFunctionKeepsItsAmbiguity) {
  auto Tail = body(101, 5);
  Tail.push_back({"ret", "", InstructionFlow::Return});
  Fn Before;
  Before.Name = "before";
  Before.Blocks = {Tail, Tail};
  Fn After;
  After.Name = "after";
  After.Blocks = {Tail};
  const auto P = build({Before}, 0x1000), S = build({After}, 0x8000);
  const auto R = findRegions(P, S, enabledRegions(), {});
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::TailMerge));
  EXPECT_TRUE(R.Hypotheses[0].Ambiguous);
  EXPECT_EQ(R.Hypotheses[0].PrimaryInstructions, 10u);
  EXPECT_EQ(R.Hypotheses[0].SecondaryInstructions, 5u);
  auto Partial = P;
  Partial.Identity.DiscoveryComplete = false;
  const auto Uncertain = findRegions(Partial, S, enabledRegions(), {});
  ASSERT_EQ(Uncertain.Hypotheses.size(), 1u);
  EXPECT_FALSE(Uncertain.Hypotheses[0].AnalysisComplete);
  EXPECT_LT(Uncertain.Hypotheses[0].Confidence, R.Hypotheses[0].Confidence);
}

TEST(BinaryDiff, RegionShortRepeatedFragmentsDoNotJoinUnrelatedLargeBodies) {
  Fn A = leaf("a", 10, 40), B = leaf("b", 20, 40);
  const auto Shared = body(99, 4);
  A.Blocks[0].insert(A.Blocks[0].begin() + 20, Shared.begin(), Shared.end());
  B.Blocks[0].insert(B.Blocks[0].begin() + 20, Shared.begin(), Shared.end());
  const auto P = build({A, B}, 0x1000), S = build({A, B}, 0x8000);
  const auto R = findRegions(P, S, enabledRegions(), {});
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_GT(R.WeakCandidates, 0u);
  ASSERT_EQ(R.Hypotheses.size(), 2u);
  for (const auto &H : R.Hypotheses)
    EXPECT_FALSE(H.Ambiguous);
  auto Broad = enabledRegions();
  Broad.MinimumRepeatedInstructions = 4;
  const auto All = findRegions(P, S, Broad, {});
  EXPECT_EQ(All.WeakCandidates, 0u);
  ASSERT_EQ(All.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(All.Hypotheses[0], RegionKind::Redistribution));
}

TEST(BinaryDiff,
     RegionEvidenceKeepsConstantChangesAndRejectsForgedCoordinates) {
  auto Old = leaf("fn", 90, 20), New = Old;
  New.Blocks[0][8].Operands = "r3, 7";
  const auto A = build({Old}, 0x1000), B = build({New}, 0x8000);
  ComparisonSettings Settings;
  Settings.Regions = enabledRegions();
  const auto R = compare(A, B, Settings);
  ASSERT_EQ(R.Matches.size(), 1u);
  EXPECT_FALSE(R.Matches[0].identical());
  ASSERT_EQ(R.Regions.Hypotheses.size(), 1u);
  EXPECT_LT(R.Regions.Hypotheses[0].PrimaryCoverage, 1);
  for (const auto &Anchor : R.Regions.Anchors) {
    const auto &Span = Anchor.Primary;
    EXPECT_TRUE(Span.FirstInstruction > 8 ||
                Span.FirstInstruction + Span.InstructionCount <= 8);
  }
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  writeComparison(OS, R, A, B);
  std::string Error;
  const auto Saved = readComparison(Text, Error);
  ASSERT_TRUE(Saved) << Error;
  ComparisonResult Restored;
  EXPECT_FALSE(bindComparison(*Saved, A, B, Restored));
  EXPECT_EQ(Restored.Regions.Anchors, R.Regions.Anchors);
  auto Forged = *Saved;
  Forged.Regions.Anchors[0].Primary.FirstInstruction = 8;
  EXPECT_TRUE(bindComparison(Forged, A, B, Restored));
  Forged = *Saved;
  Forged.Regions.Hypotheses[0].Confidence = 1;
  EXPECT_TRUE(bindComparison(Forged, A, B, Restored));
  EXPECT_EQ(Restored.Regions.Anchors, R.Regions.Anchors);
}

TEST(BinaryDiff, RegionClaimsRequireUniqueSeedsAndConnectedMembership) {
  const auto Duplicated =
      build({leaf("copy_a", 4, 12), leaf("copy_b", 4, 12)}, 0x1000);
  const auto Copy = build({leaf("copy", 4, 12)}, 0x8000);
  const auto Settings = enabledRegions();
  auto Clones = findRegions(Duplicated, Copy, Settings, {});
  ASSERT_EQ(Clones.Hypotheses.size(), 1u);
  for (auto &Anchor : Clones.Anchors) {
    ASSERT_FALSE(Anchor.UniqueSeed);
    Anchor.UniqueSeed = true;
  }
  // Forge both the source flag and the score derived from it.
  Clones.Hypotheses[0].Confidence = 0.95;
  const auto UniqueError = validateRegions(Clones, Settings, Duplicated, Copy);
  ASSERT_TRUE(UniqueError);
  EXPECT_NE(UniqueError->find("unique seed"), std::string::npos);

  const auto P = build({leaf("first", 10, 12), leaf("second", 50, 12)}, 0x1000);
  const auto R = findRegions(P, P, Settings, {});
  ASSERT_EQ(R.Hypotheses.size(), 2u);
  auto Forged = R;
  Forged.Hypotheses[0].Anchors.insert(Forged.Hypotheses[0].Anchors.end(),
                                      R.Hypotheses[1].Anchors.begin(),
                                      R.Hypotheses[1].Anchors.end());
  llvm::sort(Forged.Hypotheses[0].Anchors);
  Forged.Hypotheses.pop_back();
  const auto ConnectedError = validateRegions(Forged, Settings, P, P);
  ASSERT_TRUE(ConnectedError);
  EXPECT_NE(ConnectedError->find("connected component"), std::string::npos);
  Forged = R;
  Forged.Hypotheses.pop_back();
  EXPECT_TRUE(validateRegions(Forged, Settings, P, P));
}

TEST(BinaryDiff, SavedRestoreStopsTransactionallyAndKeepsHistoricalStates) {
  const Program P = build(sample(), 0x400000, {}, std::string(64, 'b'));
  const Program S = build(sample(), 0x800000, {}, std::string(64, 'c'));
  const auto Original = compare(P, S, {});
  const auto Text = saved(Original, P, S);
  std::string Error;
  const auto Read = readComparison(Text, Error);
  ASSERT_TRUE(Read) << Error;
  auto Retained = Original;
  ASSERT_FALSE(applyOverride(Retained, P, S,
                             {OverrideKind::Confirm, named(P, "leaf0").Address,
                              named(S, "leaf0").Address}));
  const auto Before = saved(Retained, P, S);
  std::atomic<bool> Cancel = true;
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  EXPECT_THROW(readComparison(Text, Error, Control), ComparisonInterrupted);
  EXPECT_THROW(bindComparison(*Read, P, S, Retained, nullptr, Control),
               ComparisonInterrupted);
  EXPECT_EQ(saved(Retained, P, S), Before);
  Cancel = false;
  Control.Deadline = std::chrono::steady_clock::now();
  try {
    (void)bindComparison(*Read, P, S, Retained, nullptr, Control);
    FAIL() << "Expired restore deadline was ignored";
  } catch (const ComparisonInterrupted &Stopped) {
    EXPECT_EQ(Stopped.State, ComparisonState::TimedOut);
  }
  EXPECT_EQ(saved(Retained, P, S), Before);
  Control.Deadline.reset();
  const auto Caller = std::this_thread::get_id();
  for (const auto *Stage :
       {"parse", "records", "identity", "decisions", "dataflow", "reviews",
        "regions", "flow_regions", "measure", "publish"}) {
    SCOPED_TRACE(Stage);
    Cancel = false;
    bool Seen = false;
    Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
      EXPECT_EQ(std::this_thread::get_id(), Caller);
      if (Phase == Stage) {
        Seen = true;
        Cancel = true;
      }
    };
    EXPECT_THROW(
        {
          const auto Next = readComparison(Text, Error, Control);
          ASSERT_TRUE(Next) << Error;
          (void)bindComparison(*Next, P, S, Retained, nullptr, Control);
        },
        ComparisonInterrupted);
    EXPECT_TRUE(Seen);
    EXPECT_EQ(saved(Retained, P, S), Before);
  }
  Cancel = false;
  Control.Progress = [&](llvm::StringRef, uint64_t, uint64_t) {
    EXPECT_EQ(std::this_thread::get_id(), Caller);
  };
  // Exercise measurement workers without invoking a user callback there.
  neverd::setWorkerThreadCount(4);
  const auto ResetThreads =
      llvm::scope_exit([] { neverd::setWorkerThreadCount(0); });
  for (const auto State :
       {ComparisonState::Complete, ComparisonState::Cancelled,
        ComparisonState::TimedOut, ComparisonState::ResourceLimited}) {
    auto Historical = *Read;
    Historical.State = State;
    ASSERT_FALSE(bindComparison(Historical, P, S, Retained, nullptr, Control));
    EXPECT_EQ(Retained.State, State);
    EXPECT_EQ(Retained.Matches.size(), Original.Matches.size());
  }
}

TEST(BinaryDiff, RestoreStopAfterParsingConsumesSuccessAndFailure) {
  const Program P = build(sample(), 0x400000, {}, std::string(64, 'b'));
  const auto Text = saved(compare(P, P, {}), P, P);
  for (const std::string Input : {Text, std::string("{invalid")}) {
    std::atomic<bool> Cancel = false;
    bool Parsed = false;
    ComparisonControl Control;
    Control.Cancel = &Cancel;
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                           uint64_t Total) {
      if (Phase == "parse" && Done == Total && Done) {
        Parsed = true;
        Cancel = true;
      }
    };
    std::string Error;
    EXPECT_THROW(readComparison(Input, Error, Control), ComparisonInterrupted);
    EXPECT_TRUE(Parsed);
  }
}

TEST(BinaryDiff, RestoreDeadlineDuringParallelMeasurementKeepsThePriorResult) {
  std::vector<Fn> Functions;
  for (unsigned I = 0; I < 8; ++I)
    Functions.push_back(leaf("wide" + std::to_string(I), I * 3, 60000));
  const auto P = build(Functions, 0x400000, {}, std::string(64, 'b'));
  neverd::setWorkerThreadCount(4);
  const auto ResetThreads =
      llvm::scope_exit([] { neverd::setWorkerThreadCount(0); });
  const auto Original = compare(P, P, {});
  ASSERT_EQ(Original.State, ComparisonState::Complete);
  const auto Text = saved(Original, P, P);
  std::string Error;
  const auto Saved = readComparison(Text, Error);
  ASSERT_TRUE(Saved) << Error;
  auto Retained = Original;
  bool Started = false, Finished = false;
  const auto Caller = std::this_thread::get_id();
  ComparisonControl Control;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t Done, uint64_t Total) {
    EXPECT_EQ(std::this_thread::get_id(), Caller);
    if (Phase != "measure")
      return;
    if (Done == 0) {
      Started = true;
      // Expire after the coordinator enters measurement; a returned stopped
      // delta must propagate as a typed stop, never as forged saved evidence.
      Control.Deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    } else if (Done == Total) {
      Finished = true;
    }
  };
  try {
    const auto Invalid =
        bindComparison(*Saved, P, P, Retained, nullptr, Control);
    FAIL() << "Expected a typed measurement stop, got "
           << Invalid.value_or("success");
  } catch (const ComparisonInterrupted &Stopped) {
    EXPECT_EQ(Stopped.State, ComparisonState::TimedOut);
  }
  EXPECT_TRUE(Started);
  EXPECT_FALSE(Finished);
  EXPECT_EQ(saved(Retained, P, P), Text);
  EXPECT_EQ(compareFunctions(P, 0, P, 0, {}).State, ComparisonState::Complete);
}
