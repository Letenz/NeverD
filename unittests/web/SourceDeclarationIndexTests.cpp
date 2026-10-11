//===- SourceDeclarationIndexTests.cpp - Declaration cardinality -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep multiple declaration identities distinct and independent of order.
///
//===----------------------------------------------------------------------===//

#include "SourceModel.h"
#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/SourceBindings.h"

#include <algorithm>

namespace {
using namespace neverd::web;

TEST(WebSourceDeclarations, ClassPairsAndSingleVariablesAreOrderIndependent) {
  constexpr auto Text = "class C { self(){return C;} } const x=1;";
  const auto S =
      inspectJavaScript(identity("declaration-test", {Text}), Text, "script");
  auto B = analyzeSourceBindings(S);
  ASSERT_EQ(B.Status, "ok");
  const auto Index = indexSourceDeclarations(S, B);
  bool SawClass = false, SawVariable = false;
  for (const auto &D : B.Declarations) {
    if (D.Kind == "class" || D.Kind == "class_name") {
      EXPECT_EQ(Index.UniqueBindings.at(D.Node), NoSourceIndex);
      SawClass = true;
    } else if (D.Kind == "const") {
      EXPECT_EQ(Index.UniqueBindings.at(D.Node), D.Binding);
      SawVariable = true;
    }
  }
  EXPECT_TRUE(SawClass);
  EXPECT_TRUE(SawVariable);
  std::reverse(B.Declarations.begin(), B.Declarations.end());
  EXPECT_EQ(indexSourceDeclarations(S, B).UniqueBindings, Index.UniqueBindings);
  // A third repeated occurrence must not reset the already multiple state.
  for (const auto &D : B.Declarations)
    if (D.Kind == "class_name") {
      const auto Extra = D;
      B.Declarations.push_back(Extra);
      break;
    }
  EXPECT_EQ(indexSourceDeclarations(S, B).UniqueBindings, Index.UniqueBindings);
}

TEST(WebSourceDeclarations, ConflictingAndInvalidModelsStayUnavailable) {
  constexpr auto Text = "const x=1;";
  const auto S =
      inspectJavaScript(identity("declaration-test", {Text}), Text, "script");
  auto B = analyzeSourceBindings(S);
  const auto D = B.Declarations.front();
  B.Bindings[D.Binding].Conflicting = true;
  EXPECT_EQ(indexSourceDeclarations(S, B).UniqueBindings.at(D.Node),
            NoSourceIndex);
  B.Declarations.front().Binding = UINT32_MAX;
  EXPECT_THROW(indexSourceDeclarations(S, B), Error);
  B = analyzeSourceBindings(S);
  B.Declarations.front().Node = S.Nodes.size();
  EXPECT_THROW(indexSourceDeclarations(S, B), Error);
  B = analyzeSourceBindings(S);
  B.SourceID = "different";
  EXPECT_THROW(indexSourceDeclarations(S, B), Error);
}
} // namespace
