//===- SignatureMatcher.cpp - FLIRT pattern matching engine ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureMatcher.h"

#include "neverd/support/Parallel.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

using namespace neverd::sigs;

namespace {

constexpr std::array<uint16_t, 256> makeCRC16Table() {
  std::array<uint16_t, 256> Table{};
  for (size_t I = 0; I < Table.size(); ++I) {
    uint16_t Value = static_cast<uint16_t>(I);
    for (unsigned Bit = 0; Bit < 8; ++Bit)
      Value =
          static_cast<uint16_t>((Value >> 1) ^ ((Value & 1) ? 0x8408u : 0u));
    Table[I] = Value;
  }
  return Table;
}

constexpr auto CRC16Table = makeCRC16Table();

bool checkedAdd(size_t Left, size_t Right, size_t &Result) {
  if (Right > std::numeric_limits<size_t>::max() - Left)
    return false;
  Result = Left + Right;
  return true;
}

size_t effectiveLeadingCount(const PatternModule &Mod) {
  return Mod.TotalLen == 0
             ? Mod.LeadingBytes.size()
             : std::min(Mod.LeadingBytes.size(), size_t{Mod.TotalLen});
}

void matchBucket(const std::vector<size_t> &Bucket,
                 const std::vector<PatternModule> &Modules, const uint8_t *Data,
                 size_t Available, uint64_t Address,
                 const SignatureMatcher::MatchCallback &Callback) {
  for (size_t ModuleIndex : Bucket) {
    if (ModuleIndex >= Modules.size())
      continue;
    const PatternModule &Module = Modules[ModuleIndex];
    if (SignatureMatcher::matchPattern(Module, Data, Available))
      Callback(Address, Module);
  }
}

/// The indexed prefix of one module: its bytes, and which of them it states.
struct IndexedPrefix {
  std::array<uint8_t, SignatureMatcher::HashIndex::kIndexedBytes> Bytes{};
  uint32_t Stated = 0;
};
static_assert(SignatureMatcher::HashIndex::kIndexedBytes <= 32,
              "the stated bytes of a prefix are one 32-bit mask");

/// Builds the decision nodes over a permutation of the modules: a node
/// partitions its range of the permutation, stably, into the modules that
/// leave its byte unstated and those stating each value, and its children
/// own those ranges.
class IndexBuilder {
public:
  IndexBuilder(SignatureMatcher::HashIndex &Index,
               const std::vector<PatternModule> &Modules)
      : Index(Index), Prefixes(Modules.size()), Order(Modules.size()),
        Scratch(Modules.size()) {
    // Reading every prefix once, into one array, is what keeps the passes
    // below off the modules' own allocations.
    constexpr size_t Block = 4096;
    neverd::parallelForEach(
        (Modules.size() + Block - 1) / Block, [&](auto Claim, size_t Total) {
          for (size_t B = Claim(); B < Total; B = Claim()) {
            const size_t End = std::min(Modules.size(), (B + 1) * Block);
            for (size_t I = B * Block; I < End; ++I) {
              const PatternModule &Module = Modules[I];
              IndexedPrefix &Prefix = Prefixes[I];
              const size_t Count =
                  std::min(effectiveLeadingCount(Module),
                           SignatureMatcher::HashIndex::kIndexedBytes);
              for (size_t K = 0; K < Count; ++K) {
                if (Module.LeadingBytes[K].IsWildcard)
                  continue;
                Prefix.Bytes[K] = Module.LeadingBytes[K].Value;
                Prefix.Stated |= uint32_t(1) << K;
              }
              Order[I] = I;
            }
          }
        });
  }

  size_t build() { return buildRange(0, Order.size(), 0); }

private:
  static constexpr size_t kUnstated = 256;

  size_t keyAt(size_t Module, size_t Offset) const {
    const IndexedPrefix &Prefix = Prefixes[Module];
    return (Prefix.Stated >> Offset) & 1 ? Prefix.Bytes[Offset] : kUnstated;
  }

  size_t buildRange(size_t Begin, size_t End, size_t Offset) {
    using HashIndex = SignatureMatcher::HashIndex;
    if (Begin == End)
      return HashIndex::kNoNode;

    // A byte every module of the range leaves unstated cannot dispatch a
    // query, so the next one is tried.
    std::array<size_t, kUnstated + 1> Count;
    while (true) {
      if (End - Begin <= HashIndex::kLeafCandidates ||
          Offset >= HashIndex::kIndexedBytes) {
        const size_t NodeIndex = Index.Nodes.size();
        Index.Nodes.emplace_back();
        Index.Nodes[NodeIndex].Candidates.assign(Order.begin() + Begin,
                                                 Order.begin() + End);
        return NodeIndex;
      }
      Count.fill(0);
      for (size_t I = Begin; I < End; ++I)
        ++Count[keyAt(Order[I], Offset)];
      if (Count[kUnstated] != End - Begin)
        break;
      ++Offset;
    }

    std::array<size_t, kUnstated + 1> Start;
    Start[kUnstated] = Begin;
    size_t Next = Begin + Count[kUnstated];
    for (size_t Value = 0; Value < kUnstated; ++Value) {
      Start[Value] = Next;
      Next += Count[Value];
    }
    std::array<size_t, kUnstated + 1> Fill = Start;
    for (size_t I = Begin; I < End; ++I)
      Scratch[Fill[keyAt(Order[I], Offset)]++] = Order[I];
    std::copy(Scratch.begin() + Begin, Scratch.begin() + End,
              Order.begin() + Begin);

    const size_t NodeIndex = Index.Nodes.size();
    Index.Nodes.emplace_back();
    Index.Nodes[NodeIndex].Offset = static_cast<uint8_t>(Offset);
    if (Count[kUnstated] != 0) {
      const size_t Child = buildRange(
          Start[kUnstated], Start[kUnstated] + Count[kUnstated], Offset + 1);
      Index.Nodes[NodeIndex].WildcardChild = Child;
    }
    for (size_t Value = 0; Value < kUnstated; ++Value) {
      if (Count[Value] == 0)
        continue;
      const size_t Child =
          buildRange(Start[Value], Start[Value] + Count[Value], Offset + 1);
      Index.Nodes[NodeIndex].ExactChildren.push_back(
          {static_cast<uint8_t>(Value), Child});
    }
    return NodeIndex;
  }

  SignatureMatcher::HashIndex &Index;
  std::vector<IndexedPrefix> Prefixes;
  std::vector<size_t> Order, Scratch;
};

const SignatureMatcher::HashIndex::Edge *
findExactEdge(const SignatureMatcher::HashIndex::Node &Node, uint8_t Value) {
  auto It = std::lower_bound(Node.ExactChildren.begin(),
                             Node.ExactChildren.end(), Value,
                             [](const SignatureMatcher::HashIndex::Edge &Edge,
                                uint8_t Byte) { return Edge.Value < Byte; });
  return It != Node.ExactChildren.end() && It->Value == Value ? &*It : nullptr;
}

void matchIndexNode(const SignatureMatcher::HashIndex &Index, size_t NodeIndex,
                    const std::vector<PatternModule> &Modules,
                    const uint8_t *Data, size_t Available, uint64_t Address,
                    const SignatureMatcher::MatchCallback &Callback) {
  if (NodeIndex >= Index.Nodes.size())
    return;
  const SignatureMatcher::HashIndex::Node &Node = Index.Nodes[NodeIndex];
  if (Node.isLeaf()) {
    matchBucket(Node.Candidates, Modules, Data, Available, Address, Callback);
    return;
  }

  if (Node.WildcardChild != SignatureMatcher::HashIndex::kNoNode)
    matchIndexNode(Index, Node.WildcardChild, Modules, Data, Available, Address,
                   Callback);
  if (Node.Offset >= Available)
    return;
  if (const auto *Edge = findExactEdge(Node, Data[Node.Offset]))
    matchIndexNode(Index, Edge->Child, Modules, Data, Available, Address,
                   Callback);
}

size_t countIndexNode(const SignatureMatcher::HashIndex &Index,
                      size_t NodeIndex, const uint8_t *Data, size_t Available) {
  if (NodeIndex >= Index.Nodes.size())
    return 0;
  const SignatureMatcher::HashIndex::Node &Node = Index.Nodes[NodeIndex];
  if (Node.isLeaf())
    return Node.Candidates.size();

  size_t Count = 0;
  if (Node.WildcardChild != SignatureMatcher::HashIndex::kNoNode)
    Count = countIndexNode(Index, Node.WildcardChild, Data, Available);
  if (Node.Offset >= Available)
    return Count;
  const auto *Edge = findExactEdge(Node, Data[Node.Offset]);
  if (!Edge)
    return Count;
  const size_t Exact = countIndexNode(Index, Edge->Child, Data, Available);
  return Exact > std::numeric_limits<size_t>::max() - Count
             ? std::numeric_limits<size_t>::max()
             : Count + Exact;
}

/// Report the modules that match at \p Entry, when it lies in the image.
void scanEntry(const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
               uint64_t Entry, const std::vector<PatternModule> &Modules,
               const SignatureMatcher::HashIndex &Index,
               const SignatureMatcher::MatchCallback &Callback) {
  if (Entry < BaseVA)
    return;
  const uint64_t Offset = Entry - BaseVA;
  if (Offset >= ImageSize)
    return;
  const size_t Available = ImageSize - static_cast<size_t>(Offset);
  matchIndexNode(Index, Index.Root, Modules, ImageBase + Offset, Available,
                 Entry, Callback);
}

} // namespace

uint16_t SignatureMatcher::computeCRC16(const uint8_t *Data, size_t Len) {
  uint16_t CRC = 0xFFFF;
  for (size_t I = 0; I < Len; ++I) {
    uint8_t Idx = static_cast<uint8_t>(CRC ^ Data[I]);
    CRC = (CRC >> 8) ^ CRC16Table[Idx];
  }
  // Pattern files print the complemented remainder in byte-stream order.
  CRC = static_cast<uint16_t>(~CRC);
  return static_cast<uint16_t>((CRC << 8) | (CRC >> 8));
}

bool SignatureMatcher::matchLeading(const std::vector<PatternByte> &Pattern,
                                    const uint8_t *Data, size_t Count) {
  if (Count > Pattern.size())
    return false;
  for (size_t I = 0; I < Count; ++I) {
    if (Pattern[I].IsWildcard)
      continue;
    if (Data[I] != Pattern[I].Value)
      return false;
  }
  return true;
}

bool SignatureMatcher::verifyCRC(const PatternModule &Mod, const uint8_t *Data,
                                 size_t MatchLimit) {
  const size_t CRCStart = Mod.LeadingBytes.size();
  if (Mod.CRCLen == 0)
    return true;
  if (CRCStart > MatchLimit)
    return false;
  size_t CRCEnd = 0;
  if (!checkedAdd(CRCStart, Mod.CRCLen, CRCEnd) || CRCEnd > MatchLimit)
    return false;
  return computeCRC16(Data + CRCStart, Mod.CRCLen) == Mod.CRC16;
}

bool SignatureMatcher::matchTail(const PatternModule &Mod, const uint8_t *Data,
                                 size_t MatchLimit) {
  size_t TailStart = 0;
  if (!checkedAdd(Mod.LeadingBytes.size(), Mod.CRCLen, TailStart))
    return false;
  if (TailStart >= MatchLimit)
    return true;
  const size_t TailCount =
      std::min(Mod.TailBytes.size(), MatchLimit - TailStart);
  for (size_t I = 0; I < TailCount; ++I) {
    if (Mod.TailBytes[I].IsWildcard)
      continue;
    if (Data[TailStart + I] != Mod.TailBytes[I].Value)
      return false;
  }
  return true;
}

bool SignatureMatcher::matchPattern(const PatternModule &Mod,
                                    const uint8_t *Data, size_t Available) {
  size_t MatchLimit = Mod.TotalLen;
  if (MatchLimit == 0) {
    if (!checkedAdd(Mod.LeadingBytes.size(), Mod.CRCLen, MatchLimit) ||
        !checkedAdd(MatchLimit, Mod.TailBytes.size(), MatchLimit))
      return false;
  }
  if (MatchLimit > Available || (MatchLimit != 0 && !Data))
    return false;
  const size_t LeadingCount = std::min(Mod.LeadingBytes.size(), MatchLimit);
  if (!matchLeading(Mod.LeadingBytes, Data, LeadingCount))
    return false;
  if (!verifyCRC(Mod, Data, MatchLimit))
    return false;
  return matchTail(Mod, Data, MatchLimit);
}

size_t SignatureMatcher::fixedByteCount(const PatternModule &Mod) {
  size_t Fixed = 0;
  const size_t MatchLimit =
      Mod.TotalLen == 0 ? std::numeric_limits<size_t>::max() : Mod.TotalLen;
  const size_t LeadingCount = std::min(Mod.LeadingBytes.size(), MatchLimit);
  for (size_t I = 0; I < LeadingCount; ++I)
    Fixed += Mod.LeadingBytes[I].IsWildcard ? 0 : 1;

  size_t TailStart = 0;
  if (!checkedAdd(Mod.LeadingBytes.size(), Mod.CRCLen, TailStart) ||
      TailStart > MatchLimit)
    return Fixed;
  const size_t TailCount =
      std::min(Mod.TailBytes.size(), MatchLimit - TailStart);
  for (size_t I = 0; I < TailCount; ++I)
    Fixed += Mod.TailBytes[I].IsWildcard ? 0 : 1;
  return Fixed;
}

bool SignatureMatcher::isFullyVerified(const PatternModule &Mod) {
  if (Mod.TotalLen == 0)
    return false;
  if (Mod.LeadingBytes.size() >= Mod.TotalLen)
    return Mod.CRCLen == 0;
  size_t Verified = Mod.LeadingBytes.size();
  if (!checkedAdd(Verified, Mod.CRCLen, Verified) || Verified > Mod.TotalLen)
    return false;
  if (!checkedAdd(Verified, Mod.TailBytes.size(), Verified))
    return false;
  return Verified >= Mod.TotalLen;
}

void SignatureMatcher::scanRegion(const uint8_t *Data, size_t Size,
                                  const std::vector<PatternModule> &Modules,
                                  MatchCallback Callback) {
  if ((Size != 0 && !Data) || !Callback)
    return;
  HashIndex Idx;
  Idx.build(Modules);

  for (size_t Offset = 0; Offset < Size; ++Offset) {
    const size_t Available = Size - Offset;
    matchIndexNode(Idx, Idx.Root, Modules, Data + Offset, Available, Offset,
                   Callback);
  }
}

void SignatureMatcher::HashIndex::build(
    const std::vector<PatternModule> &Modules) {
  Nodes.clear();
  Root = kNoNode;
  if (Modules.empty())
    return;

  Root = IndexBuilder(*this, Modules).build();
}

uint16_t SignatureMatcher::HashIndex::keyOf(const PatternModule &Mod) const {
  if (effectiveLeadingCount(Mod) < 2)
    return 0;
  return (static_cast<uint16_t>(Mod.LeadingBytes[0].Value) << 8) |
         Mod.LeadingBytes[1].Value;
}

bool SignatureMatcher::HashIndex::isWildcardKey(
    const PatternModule &Mod) const {
  const size_t LeadingCount = effectiveLeadingCount(Mod);
  return LeadingCount < 2 || Mod.LeadingBytes[0].IsWildcard ||
         Mod.LeadingBytes[1].IsWildcard;
}

size_t SignatureMatcher::HashIndex::candidateCount(const uint8_t *Data,
                                                   size_t Available) const {
  if ((Available != 0 && !Data) || Root == kNoNode)
    return 0;
  return countIndexNode(*this, Root, Data, Available);
}

void SignatureMatcher::scanAtAddresses(
    const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
    const std::vector<uint64_t> &FuncEntries,
    const std::vector<PatternModule> &Modules, MatchCallback Callback) {
  HashIndex Idx;
  Idx.build(Modules);
  scanAtAddresses(ImageBase, ImageSize, BaseVA, FuncEntries, Modules, Idx,
                  std::move(Callback));
}

void SignatureMatcher::scanAtAddresses(
    const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
    const std::vector<uint64_t> &FuncEntries,
    const std::vector<PatternModule> &Modules, const HashIndex &Index,
    MatchCallback Callback) {
  if ((ImageSize != 0 && !ImageBase) || !Callback)
    return;

  for (uint64_t Entry : FuncEntries)
    scanEntry(ImageBase, ImageSize, BaseVA, Entry, Modules, Index, Callback);
}

std::vector<SignatureMatcher::Hit> SignatureMatcher::findAtAddresses(
    const uint8_t *ImageBase, size_t ImageSize, uint64_t BaseVA,
    const std::vector<uint64_t> &FuncEntries,
    const std::vector<PatternModule> &Modules, const HashIndex &Index) {
  if (ImageSize != 0 && !ImageBase)
    return {};

  // Each block of entries keeps its hits apart, so that joining the blocks
  // in order reports them in the order a sequential scan does.
  constexpr size_t BlockEntries = 16;
  const size_t Blocks = (FuncEntries.size() + BlockEntries - 1) / BlockEntries;
  std::vector<std::vector<Hit>> BlockHits(Blocks);
  neverd::parallelForEach(Blocks, [&](auto Claim, size_t Total) {
    for (size_t B = Claim(); B < Total; B = Claim()) {
      std::vector<Hit> &Hits = BlockHits[B];
      const MatchCallback Record = [&](uint64_t Address,
                                       const PatternModule &Module) {
        Hits.push_back(
            {Address, static_cast<size_t>(&Module - Modules.data())});
      };
      const size_t End = std::min(FuncEntries.size(), (B + 1) * BlockEntries);
      for (size_t I = B * BlockEntries; I < End; ++I)
        scanEntry(ImageBase, ImageSize, BaseVA, FuncEntries[I], Modules, Index,
                  Record);
    }
  });

  std::vector<Hit> Hits;
  for (const std::vector<Hit> &Block : BlockHits)
    Hits.insert(Hits.end(), Block.begin(), Block.end());
  return Hits;
}
