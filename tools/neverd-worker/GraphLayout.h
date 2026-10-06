#pragma once

#include <cstddef>
#include <vector>

namespace neverd::worker {

/// Layered (Sugiyama-style) layout of a control-flow graph with orthogonal
/// edge routes.  Back edges are reversed for layering and routed upward on
/// dummy columns, so loops read top to bottom like a disassembler graph.
struct LayoutPoint {
  double x = 0, y = 0;
};

struct LayoutInput {
  struct Node {
    double width = 0, height = 0;
  };
  struct Edge {
    std::size_t from = 0, to = 0;
  };
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::size_t entry = 0;
  double horizontalGap = 40;
  double layerGap = 48;
  double channelSpacing = 7;
  double margin = 40;
};

struct LayoutResult {
  /// Top-left corner of each node.
  std::vector<LayoutPoint> positions;
  /// Polyline of each input edge, in input order, from source to target.
  std::vector<std::vector<LayoutPoint>> routes;
  double width = 0, height = 0;
  std::size_t layers = 0, crossings = 0;
};

LayoutResult layoutGraph(const LayoutInput &input);

} // namespace neverd::worker
