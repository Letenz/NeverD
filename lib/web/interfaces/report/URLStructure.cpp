//===- URLStructure.cpp - Lexical request URL components ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private decoded segments and expression anchors, never resolved routes.
///
//===----------------------------------------------------------------------===//

#include "SourceStructure.h"

#include "neverd/web/Error.h"

namespace neverd::web::passive_report {
namespace {
/// A spelling scanner, deliberately independent of URL normalization/matching.
/// An interpolation may add delimiters; all later roles stay lexical syntax.
struct URLSpelling {
  std::string Part = "unknown";
  uint64_t Path = 0, Query = 0;
  bool AfterHole = false;

  uint64_t begin(std::u16string_view Text) {
    for (const auto Prefix : {u"https://", u"http://", u"wss://", u"ws://"})
      if (Text.starts_with(Prefix)) {
        Part = "authority";
        return std::u16string_view(Prefix).size();
      }
    if (Text.starts_with(u"//")) {
      Part = "authority";
      return 2;
    }
    // Unknown schemes and absolute-looking expressions are not path proof.
    if (!Text.empty() && Text.find(u':') == std::u16string_view::npos)
      Part = "path_segment";
    return 0;
  }

  std::string delimiter(char16_t C) {
    if (Part == "fragment")
      return {};
    if (C == u'#') {
      Part = "fragment";
      return "fragment_separator";
    }
    if (C == u'?' && Part != "query_name" && Part != "query_value") {
      Part = "query_name";
      Query = 0;
      return "query_separator";
    }
    if (Part == "query_name" || Part == "query_value") {
      if (C == u'&') {
        ++Query;
        Part = "query_name";
        return "query_separator";
      }
      if (C == u'=' && Part == "query_name") {
        Part = "query_value";
        return "query_assignment";
      }
    } else if (C == u'/') {
      Part = "path_segment";
      ++Path;
      return "path_separator";
    }
    return {};
  }

  std::optional<uint64_t> index() const {
    if (Part == "path_segment")
      return Path;
    if (Part == "query_name" || Part == "query_value")
      return Query;
    return {};
  }
};
} // namespace

void RequestStructure::url() {
  const auto Node = Request.URLNode;
  const auto &N = Source.Nodes.at(Node);
  auto Root = record(Node, "request_url", "root", "", 0);
  Root.Reason = Request.URLStatus;
  const auto RootID = publish(std::move(Root));
  std::vector<uint32_t> Segments, Expressions;
  if (N.Kind == "TemplateLiteral") {
    for (const auto &C : N.Children) {
      step();
      if (C.Field == "quasis")
        Segments.push_back(C.Index);
      else if (C.Field == "expressions")
        Expressions.push_back(C.Index);
    }
    if (Segments.size() != Expressions.size() + 1)
      throw Error("invalid_source_model");
  } else if (N.Kind == "StringLiteral")
    Segments.push_back(Node);
  else
    return;

  URLSpelling Spelling;
  for (uint64_t I = 0; I < Segments.size(); ++I) {
    const auto At = Segments[I];
    const auto &Segment = Source.Nodes[At];
    const auto *Text =
        Segment.text(N.Kind == "TemplateLiteral" ? "cooked" : "value");
    auto Part = record(At, "request_url",
                       N.Kind == "TemplateLiteral" ? "template_segment"
                                                   : "literal_segment",
                       RootID, I);
    Part.ShapeKind = "string";
    Part.Reason = Text ? "static_spelling_withheld" : "uncooked_segment";
    const auto SegmentID = publish(std::move(Part));
    if (!Text)
      Spelling.Part = "unknown";
    else {
      step(Text->size());
      uint64_t Position = 0;
      auto Emit = [&](uint64_t Start, uint64_t Length, std::string Component) {
        auto R = record(At, "request_url", "lexical_component", SegmentID,
                        Position++);
        R.ShapeKind = "string";
        R.Reason = Spelling.AfterHole
                       ? "lexical_component_after_unresolved_interpolation"
                       : "lexical_component_spelling";
        R.Structure->Component = std::move(Component);
        R.Structure->ComponentIndex = Spelling.index();
        R.Structure->DecodedOffset = Start;
        R.Structure->DecodedLength = Length;
        publish(std::move(R));
      };
      uint64_t Start = I == 0 ? Spelling.begin(*Text) : 0;
      if (Start)
        Emit(0, Start, "scheme_or_network_prefix");
      for (uint64_t J = Start; J < Text->size(); ++J) {
        const auto Previous = Spelling;
        auto Delimiter = Spelling.delimiter((*Text)[J]);
        if (Delimiter.empty())
          continue;
        const auto Next = Spelling;
        Spelling = Previous;
        if (J > Start)
          Emit(Start, J - Start, Spelling.Part);
        Spelling = Next;
        Emit(J, 1, std::move(Delimiter));
        Start = J + 1;
      }
      if (Start < Text->size())
        Emit(Start, Text->size() - Start, Spelling.Part);
    }
    if (I < Expressions.size()) {
      auto R = record(Expressions[I], "request_url", "template_expression",
                      RootID, I);
      R.Reason = "interpolation_value_and_delimiters_unknown";
      R.Structure->Component = Spelling.Part;
      R.Structure->ComponentIndex = Spelling.index();
      publish(std::move(R));
      Spelling.AfterHole = true;
    }
  }
}
} // namespace neverd::web::passive_report
