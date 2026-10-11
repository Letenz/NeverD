//===- SourceModel.cpp - Validated JavaScript source models ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validated JavaScript source models.
///
//===----------------------------------------------------------------------===//

#include "SourceModel.h"

#include "neverd/web/Error.h"
#include "neverd/web/SourceBindings.h"

namespace neverd::web {

uint64_t validateSourceModel(const SourceAnalysis &Source) {
  if (Source.ParseStatus != "parsed" || Source.Nodes.empty() ||
      Source.Nodes.size() > MaxJavaScriptNodes ||
      Source.Nodes[0].Kind != "Program" || !Source.Nodes[0].ParentID.empty() ||
      (Source.SourceType != "script" && Source.SourceType != "module" &&
       Source.SourceType != "commonjs"))
    throw Error("invalid_source_model");
  std::vector<unsigned> Parents(Source.Nodes.size()),
      Depths(Source.Nodes.size());
  uint64_t Edges = 0;
  for (uint32_t I = 0; I < Source.Nodes.size(); ++I) {
    const auto &N = Source.Nodes[I];
    if (N.Start > N.End || N.ID.empty())
      throw Error("invalid_source_model");
    for (const auto &C : N.Children) {
      ++Edges;
      if (C.Index <= I || C.Index >= Source.Nodes.size() ||
          ++Parents[C.Index] != 1)
        throw Error("invalid_source_model");
      const auto &Child = Source.Nodes[C.Index];
      if (Child.ParentID != N.ID || Child.Start < N.Start || Child.End > N.End)
        throw Error("invalid_source_model");
      Depths[C.Index] = Depths[I] + 1;
      if (Depths[C.Index] >= 256)
        throw Error("source_model_depth_exceeded");
    }
    if (I && Parents[I] != 1)
      throw Error("invalid_source_model");
  }
  return Edges;
}

SourceDeclarationIndex indexSourceDeclarations(const SourceAnalysis &S,
                                               const SourceBindingAnalysis &B) {
  if (B.SourceID != S.ID || S.Nodes.size() > MaxJavaScriptNodes ||
      B.Declarations.size() > 2 * MaxJavaScriptNodes ||
      B.Bindings.size() > 2 * MaxJavaScriptNodes)
    throw Error("invalid_source_binding_model");
  SourceDeclarationIndex Result;
  Result.UniqueBindings.assign(S.Nodes.size(), NoSourceIndex);
  std::vector<bool> Multiple(S.Nodes.size());
  Result.Steps = S.Nodes.size();
  for (const auto &D : B.Declarations) {
    ++Result.Steps;
    if (D.Binding >= B.Bindings.size() ||
        (D.Node != NoSourceIndex && D.Node >= S.Nodes.size()))
      throw Error("invalid_source_binding_model");
    if (D.Node == NoSourceIndex)
      continue;
    auto &Unique = Result.UniqueBindings[D.Node];
    if (B.Bindings[D.Binding].Conflicting ||
        (Unique != NoSourceIndex && Unique != D.Binding)) {
      Multiple[D.Node] = true;
      Unique = NoSourceIndex;
    } else if (!Multiple[D.Node]) {
      Unique = D.Binding;
    }
  }
  return Result;
}

} // namespace neverd::web
