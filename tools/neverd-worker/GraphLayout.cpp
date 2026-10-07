#include "GraphLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <utility>

namespace neverd::worker {
namespace {
constexpr std::size_t MaxDummies = 200000;
constexpr int OrderingSweeps = 12;
constexpr int PlacementRounds = 8;
constexpr double DummySeparation = 14;
constexpr double DummyNodeSeparation = 24;
constexpr double StraightLinkWeight = 8;
constexpr double PortSpacing = 12;
constexpr double SelfLoopReach = 16;
constexpr std::size_t None = std::numeric_limits<std::size_t>::max();

/// A layout vertex: a real node or a dummy that reserves a column for an edge
/// passing through a layer.
struct Vertex {
  std::size_t node = None; // Real node index, or None for a dummy.
  std::size_t layer = 0;
  double width = 0, height = 0;
  double center = 0;
  double order = 0;                  // Barycenter key.
  std::vector<std::size_t> up, down; // Neighbors in adjacent layers.
};

/// Count crossings between two adjacent ordered layers (Fenwick tree).
std::size_t crossingsBetween(const std::vector<Vertex> &vertices,
                             const std::vector<std::size_t> &upper,
                             const std::vector<std::size_t> &lowerPosition) {
  std::vector<std::pair<std::size_t, std::size_t>> pairs;
  for (std::size_t i = 0; i < upper.size(); ++i)
    for (const auto next : vertices[upper[i]].down)
      pairs.emplace_back(i, lowerPosition[next]);
  std::sort(pairs.begin(), pairs.end());
  std::size_t size = 1;
  for (const auto &pair : pairs)
    size = std::max(size, pair.second + 2);
  std::vector<std::size_t> tree(size + 1, 0);
  std::size_t crossings = 0, seen = 0;
  for (const auto &[first, second] : pairs) {
    // Count earlier edges ending strictly right of this one.
    std::size_t notGreater = 0;
    for (std::size_t i = second + 1; i > 0; i -= i & (~i + 1))
      notGreater += tree[i];
    crossings += seen - notGreater;
    for (std::size_t i = second + 1; i <= size; i += i & (~i + 1))
      ++tree[i];
    ++seen;
  }
  return crossings;
}
} // namespace

LayoutResult layoutGraph(const LayoutInput &input) {
  LayoutResult result;
  const std::size_t n = input.nodes.size();
  result.positions.resize(n);
  result.routes.resize(input.edges.size());
  if (!n)
    return result;
  const std::size_t entry = input.entry < n ? input.entry : 0;

  // Back edges by depth-first search from the entry.
  std::vector<std::vector<std::size_t>> outgoing(n);
  for (std::size_t i = 0; i < input.edges.size(); ++i) {
    const auto &edge = input.edges[i];
    if (edge.from < n && edge.to < n && edge.from != edge.to)
      outgoing[edge.from].push_back(i);
  }
  std::vector<char> state(n, 0);
  std::vector<std::size_t> preorder(n, None);
  std::vector<char> reversed(input.edges.size(), 0);
  std::size_t counter = 0;
  const auto search = [&](std::size_t root) {
    if (state[root])
      return;
    std::vector<std::pair<std::size_t, std::size_t>> stack{{root, 0}};
    state[root] = 1;
    preorder[root] = counter++;
    while (!stack.empty()) {
      auto &[vertex, next] = stack.back();
      if (next < outgoing[vertex].size()) {
        const auto edgeIndex = outgoing[vertex][next++];
        const auto target = input.edges[edgeIndex].to;
        if (state[target] == 1) {
          reversed[edgeIndex] = 1;
        } else if (!state[target]) {
          state[target] = 1;
          preorder[target] = counter++;
          stack.emplace_back(target, 0);
        }
      } else {
        state[vertex] = 2;
        stack.pop_back();
      }
    }
  };
  search(entry);
  for (std::size_t v = 0; v < n; ++v)
    search(v);

  // Longest-path layering of the acyclic orientation.
  std::vector<std::vector<std::size_t>> dagOut(n);
  std::vector<std::size_t> indegree(n, 0);
  for (std::size_t i = 0; i < input.edges.size(); ++i) {
    const auto &edge = input.edges[i];
    if (edge.from >= n || edge.to >= n || edge.from == edge.to)
      continue;
    const auto a = reversed[i] ? edge.to : edge.from;
    const auto b = reversed[i] ? edge.from : edge.to;
    dagOut[a].push_back(b);
    ++indegree[b];
  }
  std::vector<std::size_t> layer(n, 0);
  {
    std::priority_queue<std::pair<std::size_t, std::size_t>,
                        std::vector<std::pair<std::size_t, std::size_t>>,
                        std::greater<>>
        ready;
    for (std::size_t v = 0; v < n; ++v)
      if (!indegree[v])
        ready.emplace(preorder[v], v);
    while (!ready.empty()) {
      const auto v = ready.top().second;
      ready.pop();
      for (const auto w : dagOut[v]) {
        layer[w] = std::max(layer[w], layer[v] + 1);
        if (!--indegree[w])
          ready.emplace(preorder[w], w);
      }
    }
  }
  std::size_t layers = 0;
  for (const auto l : layer)
    layers = std::max(layers, l + 1);
  result.layers = layers;

  // Vertices: real nodes, then dummy chains.  A forward edge spanning several
  // layers gets one dummy per skipped layer; a back edge gets one per layer it
  // climbs, including its source and target layers, so its upward column never
  // crosses a node.
  std::vector<Vertex> vertices(n);
  for (std::size_t v = 0; v < n; ++v) {
    vertices[v].node = v;
    vertices[v].layer = layer[v];
    vertices[v].width = input.nodes[v].width;
    vertices[v].height = input.nodes[v].height;
  }
  std::vector<std::vector<std::size_t>> chains(input.edges.size());
  std::size_t dummies = 0;
  const auto link = [&](std::size_t upper, std::size_t lower) {
    vertices[upper].down.push_back(lower);
    vertices[lower].up.push_back(upper);
  };
  const auto addDummy = [&](std::size_t l) {
    Vertex dummy;
    dummy.layer = l;
    vertices.push_back(std::move(dummy));
    ++dummies;
    return vertices.size() - 1;
  };
  for (std::size_t i = 0; i < input.edges.size(); ++i) {
    const auto &edge = input.edges[i];
    if (edge.from >= n || edge.to >= n || edge.from == edge.to)
      continue;
    auto &chain = chains[i];
    if (!reversed[i]) {
      const auto a = edge.from, b = edge.to;
      const auto span = layer[b] - layer[a];
      chain.push_back(a);
      if (span > 1 && dummies + span - 1 <= MaxDummies)
        for (auto l = layer[a] + 1; l < layer[b]; ++l)
          chain.push_back(addDummy(l));
      chain.push_back(b);
      for (std::size_t k = 0; k + 1 < chain.size(); ++k)
        if (vertices[chain[k]].layer + 1 == vertices[chain[k + 1]].layer)
          link(chain[k], chain[k + 1]);
    } else {
      // u (lower) climbs to v (upper).
      const auto u = edge.from, v = edge.to;
      chain.push_back(u);
      const auto climb = layer[u] - layer[v] + 1;
      if (dummies + climb <= MaxDummies) {
        for (auto l = layer[u] + 1; l-- > layer[v];)
          chain.push_back(addDummy(l));
        for (std::size_t k = 1; k + 1 < chain.size(); ++k)
          link(chain[k + 1], chain[k]);
      }
      chain.push_back(v);
    }
  }

  // Initial order: depth-first discovery; dummies follow their source.
  std::vector<std::vector<std::size_t>> ordered(layers);
  for (std::size_t v = 0; v < n; ++v)
    vertices[v].order = static_cast<double>(preorder[v]);
  for (std::size_t i = 0; i < input.edges.size(); ++i) {
    const auto &chain = chains[i];
    if (chain.size() < 3)
      continue;
    const double base = vertices[chain.front()].order;
    for (std::size_t k = 1; k + 1 < chain.size(); ++k)
      vertices[chain[k]].order = base + 0.5 + 0.001 * static_cast<double>(k);
  }
  for (std::size_t v = 0; v < vertices.size(); ++v)
    ordered[vertices[v].layer].push_back(v);
  const auto sortLayer = [&](std::vector<std::size_t> &row) {
    std::stable_sort(row.begin(), row.end(), [&](std::size_t a, std::size_t b) {
      return vertices[a].order < vertices[b].order;
    });
  };
  for (auto &row : ordered)
    sortLayer(row);

  // Barycentric crossing reduction; keep the best ordering seen.
  std::vector<std::size_t> position(vertices.size(), 0);
  const auto renumber = [&] {
    for (const auto &row : ordered)
      for (std::size_t i = 0; i < row.size(); ++i)
        position[row[i]] = i;
  };
  const auto countCrossings = [&] {
    std::size_t total = 0;
    for (std::size_t l = 0; l + 1 < layers; ++l)
      total += crossingsBetween(vertices, ordered[l], position);
    return total;
  };
  renumber();
  std::size_t best = countCrossings();
  auto bestOrder = ordered;
  for (int sweep = 0; sweep < OrderingSweeps && best; ++sweep) {
    const bool downward = sweep % 2 == 0;
    for (std::size_t step = 1; step < layers; ++step) {
      const std::size_t l = downward ? step : layers - 1 - step;
      auto &row = ordered[l];
      for (const auto v : row) {
        const auto &neighbors = downward ? vertices[v].up : vertices[v].down;
        if (neighbors.empty()) {
          vertices[v].order = static_cast<double>(position[v]);
          continue;
        }
        double sum = 0;
        for (const auto w : neighbors)
          sum += static_cast<double>(position[w]);
        vertices[v].order = sum / static_cast<double>(neighbors.size());
      }
      sortLayer(row);
      for (std::size_t i = 0; i < row.size(); ++i)
        position[row[i]] = i;
    }
    const auto crossings = countCrossings();
    if (crossings < best) {
      best = crossings;
      bestOrder = ordered;
    }
  }
  ordered = std::move(bestOrder);
  renumber();
  result.crossings = best;

  // Horizontal placement: pull every vertex toward its neighbors' centers
  // while keeping the order and minimum separations.
  const auto separation = [&](std::size_t a, std::size_t b) {
    const bool dummyA = vertices[a].node == None;
    const bool dummyB = vertices[b].node == None;
    const double gap = dummyA && dummyB   ? DummySeparation
                       : dummyA || dummyB ? DummyNodeSeparation
                                          : input.horizontalGap;
    return vertices[a].width / 2 + gap + vertices[b].width / 2;
  };
  for (const auto &row : ordered) {
    double cursor = 0;
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i)
        cursor += separation(row[i - 1], row[i]);
      vertices[row[i]].center = cursor;
    }
  }
  std::vector<double> desired, left, right;
  for (int round = 0; round < PlacementRounds; ++round) {
    const bool downward = round % 2 == 0;
    for (std::size_t step = 0; step < layers; ++step) {
      const std::size_t l = downward ? step : layers - 1 - step;
      const auto &row = ordered[l];
      if (row.empty())
        continue;
      desired.assign(row.size(), 0);
      for (std::size_t i = 0; i < row.size(); ++i) {
        const auto v = row[i];
        const auto &neighbors = downward ? vertices[v].up : vertices[v].down;
        double sum = 0, weight = 0;
        for (const auto w : neighbors) {
          const double strength =
              vertices[v].node == None && vertices[w].node == None
                  ? StraightLinkWeight
                  : 1;
          sum += vertices[w].center * strength;
          weight += strength;
        }
        desired[i] = weight ? sum / weight : vertices[v].center;
      }
      left.assign(row.size(), 0);
      right.assign(row.size(), 0);
      for (std::size_t i = 0; i < row.size(); ++i)
        left[i] = i ? std::max(desired[i],
                               left[i - 1] + separation(row[i - 1], row[i]))
                    : desired[i];
      for (std::size_t i = row.size(); i-- > 0;)
        right[i] = i + 1 < row.size()
                       ? std::min(desired[i],
                                  right[i + 1] - separation(row[i], row[i + 1]))
                       : desired[i];
      for (std::size_t i = 0; i < row.size(); ++i)
        vertices[row[i]].center = (left[i] + right[i]) / 2;
    }
  }
  double minimumLeft = std::numeric_limits<double>::max();
  for (const auto &vertex : vertices)
    minimumLeft = std::min(minimumLeft, vertex.center - vertex.width / 2);
  for (auto &vertex : vertices)
    vertex.center += input.margin - minimumLeft;

  // Ports: spread a node's outgoing and incoming routes across its bottom and
  // top edges, ordered by where each route heads.
  struct Port {
    std::size_t edge;
    double toward;
  };
  std::vector<std::vector<Port>> outPorts(n), inPorts(n);
  const auto waypointX = [&](std::size_t vertex) {
    return vertices[vertex].center;
  };
  for (std::size_t i = 0; i < input.edges.size(); ++i) {
    const auto &chain = chains[i];
    if (chain.size() < 2)
      continue;
    const auto source = chain.front(), target = chain.back();
    outPorts[source].push_back({i, waypointX(chain[1])});
    inPorts[target].push_back({i, waypointX(chain[chain.size() - 2])});
  }
  std::vector<double> outX(input.edges.size(), 0), inX(input.edges.size(), 0);
  const auto spread = [&](std::vector<Port> &ports, std::size_t node,
                          std::vector<double> &out) {
    std::stable_sort(
        ports.begin(), ports.end(),
        [](const Port &a, const Port &b) { return a.toward < b.toward; });
    const double width = vertices[node].width;
    const double spacing =
        std::min(PortSpacing, width / static_cast<double>(ports.size() + 1));
    const double first = vertices[node].center -
                         spacing * static_cast<double>(ports.size() - 1) / 2;
    for (std::size_t k = 0; k < ports.size(); ++k)
      out[ports[k].edge] = first + spacing * static_cast<double>(k);
  };
  for (std::size_t v = 0; v < n; ++v) {
    if (!outPorts[v].empty())
      spread(outPorts[v], v, outX);
    if (!inPorts[v].empty())
      spread(inPorts[v], v, inX);
  }

  // Horizontal hops of every route, keyed by the gap they cross.  Gap g + 1
  // lies below layer g; gap 0 is above the first layer.
  struct Hop {
    std::size_t edge, index;
    double from, to;
    double y = 0;
  };
  std::vector<std::vector<Hop>> gaps(layers + 1);
  struct Plan {
    std::vector<double> columns;    // x of each vertical run
    std::vector<std::size_t> gapOf; // gap of each hop between runs
  };
  std::vector<Plan> plans(input.edges.size());
  for (std::size_t i = 0; i < input.edges.size(); ++i) {
    const auto &chain = chains[i];
    if (chain.size() < 2)
      continue;
    auto &plan = plans[i];
    plan.columns.push_back(outX[i]);
    if (!reversed[i]) {
      for (std::size_t k = 1; k < chain.size(); ++k) {
        plan.gapOf.push_back(vertices[chain[k - 1]].layer + 1);
        plan.columns.push_back(
            k + 1 == chain.size() ? inX[i] : vertices[chain[k]].center);
      }
    } else {
      // Down into the gap below the source, then up the dummy columns.
      const auto sourceLayer = vertices[chain.front()].layer;
      if (chain.size() > 2) {
        plan.gapOf.push_back(sourceLayer + 1);
        for (std::size_t k = 1; k + 1 < chain.size(); ++k) {
          plan.columns.push_back(vertices[chain[k]].center);
          if (k + 2 < chain.size())
            plan.gapOf.push_back(vertices[chain[k]].layer);
        }
        plan.gapOf.push_back(vertices[chain[chain.size() - 2]].layer);
      } else {
        plan.gapOf.push_back(sourceLayer + 1);
      }
      plan.columns.push_back(inX[i]);
    }
    for (std::size_t k = 0; k < plan.gapOf.size(); ++k)
      gaps[plan.gapOf[k]].push_back(
          {i, k, plan.columns[k], plan.columns[k + 1]});
  }

  // Tracks: hops in one gap share a track only when their spans are apart.
  std::vector<std::size_t> trackCount(layers + 1, 0);
  for (auto &hops : gaps) {
    std::sort(hops.begin(), hops.end(), [](const Hop &a, const Hop &b) {
      return std::min(a.from, a.to) < std::min(b.from, b.to);
    });
  }
  std::vector<std::vector<std::size_t>> trackOf(layers + 1);
  for (std::size_t g = 0; g <= layers; ++g) {
    std::vector<double> trackEnd;
    trackOf[g].resize(gaps[g].size());
    for (std::size_t h = 0; h < gaps[g].size(); ++h) {
      const auto &hop = gaps[g][h];
      const double lo = std::min(hop.from, hop.to) - input.channelSpacing;
      const double hi = std::max(hop.from, hop.to);
      std::size_t track = 0;
      while (track < trackEnd.size() && trackEnd[track] >= lo)
        ++track;
      if (track == trackEnd.size())
        trackEnd.push_back(hi);
      else
        trackEnd[track] = hi;
      trackOf[g][h] = track;
    }
    trackCount[g] = trackEnd.size();
  }

  // Vertical placement: layer heights plus room for each gap's tracks.
  std::vector<double> layerHeight(layers, 0), gapHeight(layers + 1, 0);
  for (std::size_t v = 0; v < n; ++v)
    layerHeight[layer[v]] =
        std::max(layerHeight[layer[v]], input.nodes[v].height);
  for (std::size_t g = 0; g <= layers; ++g)
    gapHeight[g] = trackCount[g]
                       ? std::max(input.layerGap,
                                  (trackCount[g] + 1) * input.channelSpacing)
                       : (g == 0 || g == layers ? 0 : input.layerGap);
  std::vector<double> layerTop(layers, 0);
  double y = input.margin + gapHeight[0];
  for (std::size_t l = 0; l < layers; ++l) {
    layerTop[l] = y;
    y += layerHeight[l] + gapHeight[l + 1];
  }
  const auto gapTrackY = [&](std::size_t g, std::size_t track) {
    const double top =
        g == 0 ? input.margin : layerTop[g - 1] + layerHeight[g - 1];
    const double used = (trackCount[g] - 1) * input.channelSpacing;
    return top + (gapHeight[g] - used) / 2 +
           static_cast<double>(track) * input.channelSpacing;
  };
  for (std::size_t g = 0; g <= layers; ++g)
    for (std::size_t h = 0; h < gaps[g].size(); ++h) {
      const auto &hop = gaps[g][h];
      plans[hop.edge].gapOf[hop.index] = g; // unchanged; y recorded below
      gaps[g][h].y = gapTrackY(g, trackOf[g][h]);
    }
  std::vector<std::vector<double>> hopY(input.edges.size());
  for (std::size_t i = 0; i < input.edges.size(); ++i)
    hopY[i].resize(plans[i].gapOf.size(), 0);
  for (const auto &hops : gaps)
    for (const auto &hop : hops)
      hopY[hop.edge][hop.index] = hop.y;

  for (std::size_t v = 0; v < n; ++v)
    result.positions[v] = {vertices[v].center - vertices[v].width / 2,
                           layerTop[layer[v]]};

  // Routes.
  for (std::size_t i = 0; i < input.edges.size(); ++i) {
    const auto &edge = input.edges[i];
    auto &route = result.routes[i];
    if (edge.from >= n || edge.to >= n)
      continue;
    const auto &node = result.positions[edge.from];
    const double width = input.nodes[edge.from].width;
    const double height = input.nodes[edge.from].height;
    if (edge.from == edge.to) {
      const double x = node.x + width - SelfLoopReach;
      const double bottom = node.y + height, top = node.y;
      route = {{x, bottom},
               {x, bottom + SelfLoopReach},
               {node.x + width + SelfLoopReach, bottom + SelfLoopReach},
               {node.x + width + SelfLoopReach, top - SelfLoopReach},
               {x, top - SelfLoopReach},
               {x, top}};
      continue;
    }
    const auto &plan = plans[i];
    if (plan.columns.empty())
      continue;
    route.push_back({plan.columns[0], node.y + height});
    for (std::size_t k = 0; k < plan.gapOf.size(); ++k) {
      const double trackY = hopY[i][k];
      route.push_back({plan.columns[k], trackY});
      route.push_back({plan.columns[k + 1], trackY});
    }
    route.push_back({plan.columns.back(), result.positions[edge.to].y});
    // Drop zero-length steps.
    std::vector<LayoutPoint> compact;
    for (const auto &point : route)
      if (compact.empty() || std::abs(compact.back().x - point.x) > 1e-6 ||
          std::abs(compact.back().y - point.y) > 1e-6)
        compact.push_back(point);
    route = std::move(compact);
  }

  for (std::size_t v = 0; v < n; ++v) {
    result.width =
        std::max(result.width, result.positions[v].x + input.nodes[v].width);
    result.height =
        std::max(result.height, result.positions[v].y + input.nodes[v].height);
  }
  for (const auto &route : result.routes)
    for (const auto &point : route) {
      result.width = std::max(result.width, point.x);
      result.height = std::max(result.height, point.y);
    }
  result.width += input.margin;
  result.height += input.margin;
  return result;
}

} // namespace neverd::worker
