//===- RegistrationCallbackGraph.h - Callback execution states -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONCALLBACKGRAPH_H
#define NEVERD_IR_LOW_X86_REGISTRATIONCALLBACKGRAPH_H

#include "RegistrationFrame.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <deque>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd::registration_state {

/// A CFG backedge can advance a bounded loop. Only a repeated complete frame
/// at the same instruction denotes a recurrent execution configuration.
class RegistrationCallbackGraph {
public:
  struct Node {
    size_t Block;
    size_t FirstOp;
    FrameState Frame;
  };
  using Charge = llvm::function_ref<bool(size_t)>;

  std::optional<size_t> add(size_t Block, size_t FirstOp,
                            const FrameState &Frame,
                            std::optional<size_t> Parent, Charge Work);
  bool empty() const { return Pending.empty(); }
  size_t take();
  const Node &node(size_t Index) const { return Nodes[Index]; }
  bool closes(Charge Work) const;

private:
  std::map<std::tuple<size_t, size_t, size_t>, std::vector<size_t>> Buckets;
  std::vector<Node> Nodes;
  std::vector<std::set<size_t>> Edges;
  std::deque<size_t> Pending;
};

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONCALLBACKGRAPH_H
