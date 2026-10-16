//===- RegistrationStateCookies.cpp - x86 EH cookie proof -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <utility>

namespace neverd::registration_state {

void RegistrationStateSolver::initializeCookies() {
  CompleteCookies =
      EH4 && SecurityCookieVA && SecurityCookieVA <= UINT32_MAX - 3 &&
      Chain.ScopeTableVA <= UINT32_MAX - 16 && Chain.EHCookieOffset < -2;
  EHCookieSlot = cookieSlot(Chain.EHCookieOffset);
  GSCookieSlot = Chain.GSCookieOffset != -2 ? cookieSlot(Chain.GSCookieOffset)
                                            : std::optional<int32_t>{};
  if (!EH4 || !GSCookieSlot || CookieCheckVA > UINT32_MAX)
    CookieCheckVA = 0;
  CompleteCookies &= EHCookieSlot.has_value() &&
                     (Chain.GSCookieOffset == -2 || GSCookieSlot.has_value());
}

std::optional<int32_t>
RegistrationStateSolver::cookieSlot(int32_t Displacement) const {
  const int64_t Offset = int64_t(*Chain.RegistrationOffset) + 16 + Displacement;
  if (Offset < INT32_MIN || Offset > INT32_MAX ||
      Offset + 4 > int64_t(*Chain.RegistrationOffset) - 8)
    return std::nullopt;
  return int32_t(Offset);
}

bool RegistrationStateSolver::cookiesReady(const FrameState &Frame) const {
  const auto SP =
      Frame.CallbackEntry
          ? Frame.load(*Chain.RegistrationOffset - 8, 4).Offset
          : Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  auto Matches = [&](std::optional<int32_t> Slot, int32_t XOROffset) {
    if (!Slot || !SP || *Slot < *SP)
      return false;
    const int64_t ExpectedFrame =
        int64_t(*Chain.RegistrationOffset) + 16 + XOROffset;
    const auto It = Frame.Cells.find(*Slot);
    return ExpectedFrame >= INT32_MIN && ExpectedFrame <= INT32_MAX &&
           It != Frame.Cells.end() && It->second.SecurityCookie &&
           It->second.CookieFrameOffset == int32_t(ExpectedFrame) &&
           It->second.CookieXOR == 0;
  };
  const auto Table = Frame.Cells.find(*Chain.RegistrationOffset + 8);
  return Table != Frame.Cells.end() && Table->second.SecurityCookie &&
         !Table->second.CookieFrameOffset &&
         Table->second.CookieXOR == Chain.ScopeTableVA &&
         Matches(EHCookieSlot, Chain.EHCookieXOROffset) &&
         (Chain.GSCookieOffset == -2 ||
          Matches(GSCookieSlot, Chain.GSCookieXOROffset));
}

bool RegistrationStateSolver::hasCompleteCookieChecks() {
  size_t Checked = 0;
  for (const auto &Identity : CookieCheckOccurrences) {
    if (!charge(1))
      return false;
    if (CookieChecks.count(Identity)) {
      ++Checked;
      continue;
    }
    // A preserved helper is proved for these invocation inputs. Its unchanged
    // unreachable code supplies no cookie observation on this call. A parent
    // being reconstructed still needs a receipt for every source occurrence.
    if (!Conditional || !Conditional->Complete)
      return false;
    const auto Boundary = Boundaries.find(Identity.first);
    if (Boundary == Boundaries.end())
      return false;
    const auto Block = Index.find(Boundary->second.first);
    if (Block == Index.end() || Block->second >= Result.Blocks.size() ||
        Result.Blocks[Block->second].Reached)
      return false;
  }
  return Checked == CookieChecks.size();
}

} // namespace neverd::registration_state
