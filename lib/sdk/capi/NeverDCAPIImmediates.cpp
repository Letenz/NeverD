//===- NeverDCAPIImmediates.cpp - Decoded immediate search ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decoded immediate search.
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/decode/NativeOperands.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/NumericInput.h"

#include "llvm/ADT/StringExtras.h"

using namespace neverd;
using namespace neverd::sdk;

llvm::json::Object neverd::sdk::searchNativeImmediates(Session &S,
                                                       llvm::StringRef Pattern,
                                                       std::optional<va_t> From,
                                                       bool Backward,
                                                       size_t Limit) {
  if (S.Img.Arch != Arch::X64 && S.Img.Arch != Arch::X86 &&
      S.Img.Arch != Arch::AArch64 && S.Img.Arch != Arch::ARM)
    throw std::invalid_argument(
        "Immediate search requires a native x86 or ARM image");
  bool Signed = Pattern.starts_with("-");
  if (Signed)
    Pattern = Pattern.drop_front();
  const auto Magnitude = parseUnsignedDecimalOrHex(
      std::string_view(Pattern.data(), Pattern.size()));
  if (!Magnitude || (Signed && *Magnitude > (uint64_t(1) << 63)))
    throw std::invalid_argument(
        "Enter a signed decimal or hexadecimal integer within 64 bits");
  const uint64_t Wanted = *Magnitude;
  Signed = Signed && Wanted != 0;
  auto &Index = S.ImmediateIndex;
  if (!S.synchronizeFunctions())
    throw std::invalid_argument("Cannot synchronize native function entries");
  if (!Index.Initialized || Index.FunctionRevision != S.FunctionListRevision) {
    std::vector<va_t> Seeds;
    if (S.Img.Entry)
      Seeds.push_back(S.Img.Entry);
    for (const auto &F : S.Functions)
      Seeds.push_back(F.Entry);
    std::sort(Seeds.begin(), Seeds.end());
    Seeds.erase(std::unique(Seeds.begin(), Seeds.end()), Seeds.end());
    // Analysis and navigation can publish entries after an earlier search
    // skipped an undecodable range. Names and extents alone do not change
    // decode boundaries, so they keep the warm index and match progress.
    if (Index.Initialized && Seeds != Index.Seeds)
      Index = {};
    Index.Seeds = std::move(Seeds);
    Index.FunctionRevision = S.FunctionListRevision;
  }
  if (!Index.Initialized) {
    for (size_t N = 0; N < S.Img.Segments.size(); ++N)
      if (S.Img.Segments[N].isExecutable() && !S.Img.Segments[N].Data.empty())
        Index.Segments.push_back(N);
    std::stable_sort(Index.Segments.begin(), Index.Segments.end(),
                     [&](auto A, auto B) {
                       return S.Img.Segments[A].VA < S.Img.Segments[B].VA;
                     });
    Index.Initialized = true;
  }
  auto NextPC = [&]() -> std::optional<va_t> {
    if (Index.Segment >= Index.Segments.size())
      return {};
    return S.Img.Segments[Index.Segments[Index.Segment]].VA + Index.Offset;
  };
  Decoder D;
  if (!D.init(S.Img))
    throw std::invalid_argument("The native decoder is unavailable");
  size_t Work = 0, Bytes = 0;
  while (Index.Segment < Index.Segments.size() &&
         Work < kImmediateSearchStepInstructions &&
         Bytes < kImmediateSearchStepBytes) {
    const auto &Seg = S.Img.Segments[Index.Segments[Index.Segment]];
    const size_t End =
        static_cast<size_t>(std::min<uint64_t>(Seg.Size, Seg.Data.size()));
    if (Index.Offset >= End) {
      ++Index.Segment;
      Index.Offset = 0;
      continue;
    }
    const va_t PC = Seg.VA + Index.Offset;
    if (Backward && From && PC >= *From)
      break;
    const auto Seed =
        std::upper_bound(Index.Seeds.begin(), Index.Seeds.end(), PC);
    const size_t Available =
        Seed != Index.Seeds.end() && *Seed < Seg.VA + End
            ? std::min<size_t>(End - Index.Offset, *Seed - PC)
            : End - Index.Offset;
    DecodedInsn I{};
    const bool Mode = D.selectMode(S.Img, PC);
    const int Size = Mode ? D.decodeOne(Seg.Data.data() + Index.Offset,
                                        std::min<size_t>(16, Available), PC, I)
                          : 0;
    if (Size <= 0 || !S.Img.isCodeRange(PC, Size)) {
      // Resume only at an independently known function entry. Variable-length
      // instruction boundaries are not guessed by stepping one byte backwards
      // or by resynchronizing on arbitrary bytes after a failed decode.
      auto Next = std::upper_bound(Index.Seeds.begin(), Index.Seeds.end(), PC);
      const auto Resume = Next != Index.Seeds.end() && *Next < Seg.VA + End
                              ? *Next
                              : Seg.VA + End;
      if (Index.Gaps.size() < kMaxImmediateSearchGaps)
        Index.Gaps.push_back(
            {PC, Resume, Mode ? "decode_failed" : "unknown_instruction_mode"});
      Index.Offset = static_cast<size_t>(Resume - Seg.VA);
      ++Work;
      continue;
    }
    const auto Operands = describeNativeOperands(D, I, S.Img.Arch, false);
    for (const auto &O : Operands.Immediates) {
      if (Index.Items.size() >= kMaxImmediateSearchOperands)
        throw std::invalid_argument(
            "Immediate index exceeds two million operands");
      Index.Items.push_back({PC, O.Value, O.Operand, O.Bits});
    }
    Index.Offset += Size;
    Bytes += Size;
    Index.Bytes += Size;
    ++Work;
    if (Index.Bytes > kMaxImmediateSearchCodeBytes)
      throw std::invalid_argument(
          "Immediate index exceeds 512 MiB of decoded code");
  }
  if (Index.Segment >= Index.Segments.size())
    Index.Complete = true;
  const auto Progress = NextPC();
  const bool BuildingBackward =
      Backward && !Index.Complete && (!From || (Progress && *Progress < *From));
  const std::string Key = (Signed ? "-" : "+") + Pattern.str();
  if (Index.MatchPattern != Key) {
    Index.MatchPattern = Key;
    Index.MatchProgress = 0;
    Index.Matches.clear();
  }
  for (; Index.MatchProgress < Index.Items.size(); ++Index.MatchProgress) {
    const auto &O = Index.Items[Index.MatchProgress];
    const auto Mask = O.Bits == 64 ? UINT64_MAX : (uint64_t(1) << O.Bits) - 1;
    const bool Match =
        Signed ? (Wanted && Wanted <= (uint64_t(1) << (O.Bits - 1)) &&
                  O.Value == ((uint64_t(0) - Wanted) & Mask))
               : Wanted <= Mask && O.Value == Wanted;
    if (Match)
      Index.Matches[O.PC].push_back(O);
  }
  llvm::json::Array Items;
  auto Add = [&](const auto &H) {
    llvm::json::Array Operands;
    for (const auto &O : H.second)
      Operands.push_back(
          llvm::json::Object{{"operand", O.Operand},
                             {"value", "0x" + llvm::utohexstr(O.Value)},
                             {"bits", O.Bits}});
    Items.push_back(
        llvm::json::Object{{"addr", vaHex(H.first)},
                           {"context", std::string("Immediate ") +
                                           (Signed ? "-" : "") + Pattern.str()},
                           {"operands", std::move(Operands)}});
  };
  bool More = false;
  const auto &Hits = Index.Matches;
  if (!BuildingBackward && Backward) {
    auto I = From ? Hits.lower_bound(*From) : Hits.end();
    while (I != Hits.begin() && Items.size() < Limit)
      Add(*--I);
    More = I != Hits.begin();
  } else if (!BuildingBackward) {
    auto I = From ? Hits.upper_bound(*From) : Hits.begin();
    while (I != Hits.end() && Items.size() < Limit)
      Add(*I++);
    More = I != Hits.end();
  }
  const bool Building =
      BuildingBackward || (!Backward && Items.empty() && !Index.Complete);
  const bool Complete = !Building && (Backward || Index.Complete) && !More;
  llvm::json::Array Gaps;
  for (const auto &G : Index.Gaps)
    Gaps.push_back(llvm::json::Object{
        {"from", vaHex(G.From)}, {"to", vaHex(G.To)}, {"reason", G.Reason}});
  llvm::json::Object Reply{{"complete", Complete},
                           {"building", Building},
                           {"decoded_bytes", Index.Bytes},
                           {"coverage", Index.Gaps.empty()
                                            ? "decoded_executable_ranges"
                                            : "partial"},
                           {"undecoded_ranges", std::move(Gaps)}};
  Reply["next_address"] =
      !Items.empty() && !Complete
          ? llvm::json::Value(
                Items.back().getAsObject()->getString("addr")->str())
          : llvm::json::Value(nullptr);
  Reply["progress_address"] = Progress ? llvm::json::Value(vaHex(*Progress))
                                       : llvm::json::Value(nullptr);
  Reply["items"] = std::move(Items);
  return Reply;
}
