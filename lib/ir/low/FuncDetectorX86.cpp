//===- FuncDetectorX86.cpp - x86 / x86-64 function-entry scanning ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 and x86-64 scanners used by FuncDetector: direct CALL targets via a
/// lightweight decode walk, and VC6 / MSVC unsymbolized prologue recovery.
///
//===----------------------------------------------------------------------===//

#include "FuncDetectorDetail.h"

#include "neverd/ir/low/FuncDetector.h"
#include "neverd/support/ProloguePatterns.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <capstone/x86.h>
#include <cstring>
#include <set>
#include <vector>

namespace neverd {
namespace func_detect_detail {

CodeInterval codeIntervalAround(const BinaryImage &Img, va_t Addr) {
  if (Img.Arch != Arch::X86 && Img.Arch != Arch::X64)
    return {};
  const Segment *Seg = Img.getSegmentFor(Addr);
  if (!Seg || !Seg->isExecutable() || !Img.isCodeAddress(Addr) ||
      Seg->Size > InvalidVA - Seg->VA)
    return {};
  CodeInterval Known{Seg->VA, Seg->VA + Seg->Size};
  // The first segment and section containing an address own it, so every
  // earlier one that overlaps the stretch cuts it short.
  bool Inside = false;
  auto Exclude = [&](va_t Begin, uint64_t Size) {
    const va_t End = Size > InvalidVA - Begin ? InvalidVA : Begin + Size;
    if (End <= Known.Lo || Begin >= Known.Hi)
      return;
    if (Addr < Begin)
      Known.Hi = Begin;
    else if (Addr >= End)
      Known.Lo = End;
    else
      Inside = true;
  };
  for (const Segment &Other : Img.Segments) {
    if (&Other == Seg)
      break;
    Exclude(Other.VA, Other.Size);
  }
  if (const Section *Sec = Img.getSectionFor(Addr)) {
    if (Sec->Size > InvalidVA - Sec->VA)
      return {};
    Known.Lo = std::max(Known.Lo, Sec->VA);
    Known.Hi = std::min(Known.Hi, Sec->VA + Sec->Size);
    for (const Section &Other : Img.Sections) {
      if (&Other == Sec)
        break;
      if (Other.isReadable())
        Exclude(Other.VA, Other.Size);
    }
  } else if (Img.segmentHasReadableSectionMetadata(*Seg)) {
    return {};
  }
  if (Inside || Known.Lo >= Known.Hi)
    return {};
  return Known;
}

std::optional<CallScanStep> stepCallsX86(const BinaryImage &Img, Decoder &Dec,
                                         const Segment *Seg, va_t Cur, va_t End,
                                         CodeInterval &Known) {
  const size_t Off = static_cast<size_t>(Cur - Seg->VA);
  if (Off >= Seg->Data.size())
    return std::nullopt;
  CallScanStep Step;
  Step.Addr = Cur;
  DecodedInsn DI;
  const size_t Remain =
      static_cast<size_t>(std::min<va_t>(Seg->Data.size() - Off, End - Cur));
  const int Sz = Dec.decodeOneLight(Seg->Data.data() + Off, Remain, Cur, DI);
  if (Sz <= 0) {
    Step.Next = Cur + 1;
    return Step;
  }
  Step.Next = Cur + static_cast<va_t>(Sz);
  if (Known.Lo != Known.Hi && Cur >= Known.Hi)
    Known = codeIntervalAround(Img, Cur);
  const bool Owned = Known.Lo != Known.Hi && Cur >= Known.Lo &&
                     Step.Next <= Known.Hi && Step.Next > Cur;
  if (!Owned &&
      !Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz)))
    return Step;
  // Only a call can have a direct target, and an instruction's size and id do
  // not depend on operand detail, so a sweep run without detail decodes the
  // calls again with it.
  if (!Dec.detailEnabled()) {
    if (DI.Id != X86_INS_CALL)
      return Step;
    Dec.setDetail(true);
    const int Again =
        Dec.decodeOneLight(Seg->Data.data() + Off, Remain, Cur, DI);
    Dec.setDetail(false);
    if (Again != Sz)
      return Step;
  }
  const va_t Tgt = Dec.directCallTarget(DI);
  if (Tgt != InvalidVA && Img.hasExecutableCodeOwnerAt(Tgt))
    Step.Target = Tgt;
  return Step;
}

void scanSegmentCallsX86(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out) {
  CodeInterval Known = codeIntervalAround(Img, Start);
  for (va_t Cur = Start; Cur < End;) {
    const std::optional<CallScanStep> Step =
        stepCallsX86(Img, Dec, Seg, Cur, End, Known);
    if (!Step)
      break;
    if (Step->Target != InvalidVA)
      Out.insert(Step->Target);
    Cur = Step->Next;
  }
}

} // namespace func_detect_detail

namespace {

bool isX86StrongFramePrologue(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < 2)
    return false;
  // push ebp / mov ebp, esp  (55 8B EC or 55 89 E5)
  if (Bytes[0] == 0x55 && (Bytes[1] == 0x8B || Bytes[1] == 0x89))
    return true;
  // hotpatch: mov edi, edi; push ebp
  if (Bytes.size() >= 3 && Bytes[0] == 0x8B && Bytes[1] == 0xFF &&
      Bytes[2] == 0x55)
    return true;
  return false;
}

bool isX86StackAdjustPrologue(llvm::ArrayRef<uint8_t> Bytes) {
  return Bytes.size() >= 2 && Bytes[1] == 0xEC &&
         (Bytes[0] == 0x81 || Bytes[0] == 0x83);
}

bool isX86MsvcPrologue(llvm::ArrayRef<uint8_t> Bytes) {
  if (isX86StrongFramePrologue(Bytes) || isX86StackAdjustPrologue(Bytes))
    return true;
  if (Bytes.size() < 2)
    return false;
  // SEH: push -1; push handler
  if (Bytes[0] == 0x6A && Bytes[1] == 0xFF)
    return true;
  // Callee-saved push then another push or a mov (VC6 cdecl / thiscall).
  if (Bytes[0] == 0x51 || Bytes[0] == 0x53 || Bytes[0] == 0x56 ||
      Bytes[0] == 0x57) {
    const uint8_t Next = Bytes[1];
    if (Next == 0x51 || Next == 0x53 || Next == 0x55 || Next == 0x56 ||
        Next == 0x57 || Next == 0x8B)
      return true;
  }
  // mov r32, [esp+disp8]
  if (Bytes.size() >= 3 && Bytes[0] == 0x8B && Bytes[2] == 0x24)
    return true;
  // MSVC thiscall vtable-store thunk: mov dword ptr [ecx], imm32
  if (Bytes.size() >= 6 && Bytes[0] == 0xC7 && Bytes[1] == 0x01)
    return true;
  return false;
}

void considerX86Prologue(const BinaryImage &Img, Decoder &Dec,
                         const Segment &Seg, va_t Addr,
                         const std::vector<std::pair<va_t, va_t>> &Occupied,
                         std::set<va_t> &Out,
                         std::set<va_t> &UnsymbolizedX86Entries) {
  auto InteriorOfSizedSymbol = [&](va_t A) {
    auto It =
        std::upper_bound(Occupied.begin(), Occupied.end(), A,
                         [](va_t Needle, const std::pair<va_t, va_t> &Range) {
                           return Needle < Range.first;
                         });
    if (It == Occupied.begin())
      return false;
    --It;
    return A > It->first && A < It->second;
  };
  // The push after MSVC's hotpatch no-op is the second instruction of a
  // function that begins at the no-op, never an entry of its own.
  if (Addr >= Seg.VA + 2 &&
      isX86HotpatchEntryAt(Seg.Data.data(), Seg.Data.size(),
                           static_cast<size_t>(Addr - Seg.VA) - 2))
    Addr -= 2;
  if (InteriorOfSizedSymbol(Addr) || !Img.hasExecutableCodeOwnerAt(Addr))
    return;
  if (Addr < Seg.VA)
    return;
  const size_t Off = static_cast<size_t>(Addr - Seg.VA);
  if (Off >= Seg.Data.size())
    return;
  const size_t Remain = Seg.Data.size() - Off;
  DecodedInsn DI;
  const int Sz = Dec.decodeOneLight(Seg.Data.data() + Off, Remain, Addr, DI);
  if (Sz <= 0 || !DI.Raw)
    return;
  Out.insert(Addr);
  UnsymbolizedX86Entries.insert(Addr);
}

} // namespace

void FuncDetector::scanX86UnsymbolizedEntries(const BinaryImage &Img,
                                              Decoder &Dec,
                                              std::set<va_t> &Out) {
  std::vector<std::pair<va_t, va_t>> Occupied;
  for (const Symbol &Sym : Img.Symbols) {
    if (!Sym.IsFunc || Sym.Size == 0 || Sym.Size > InvalidVA - Sym.Addr)
      continue;
    Occupied.emplace_back(Sym.Addr, Sym.Addr + Sym.Size);
  }
  std::sort(Occupied.begin(), Occupied.end());

  const bool PrevDetail = Dec.detailEnabled();
  Dec.setDetail(false);
  for (const Section &Sec : Img.Sections) {
    if (Sec.Size < 4 || !Sec.isExecutable() || !Img.isCodeAddress(Sec.VA))
      continue;
    const Segment *Seg = Img.getSegmentFor(Sec.VA);
    if (!Seg || Seg->Data.empty() || Sec.VA < Seg->VA)
      continue;
    const va_t SecEnd = Sec.VA + Sec.Size;
    const llvm::ArrayRef<uint8_t> SecBytes(Seg->Data.data(), Seg->Data.size());

    auto At = [&](va_t Addr) {
      const size_t Off = static_cast<size_t>(Addr - Seg->VA);
      if (Off >= Seg->Data.size())
        return llvm::ArrayRef<uint8_t>();
      return llvm::ArrayRef<uint8_t>(Seg->Data.data() + Off,
                                     Seg->Data.size() - Off);
    };

    // 16-byte MSVC alignment: SEH, callee-saved pushes, vtable thunks, frames.
    va_t Aligned = (Sec.VA + 15u) & ~static_cast<va_t>(15u);
    for (; Aligned + 3 < SecEnd; Aligned += 16) {
      if (!isX86MsvcPrologue(At(Aligned)))
        continue;
      considerX86Prologue(Img, Dec, *Seg, Aligned, Occupied, Out,
                          UnsymbolizedX86Entries);
    }

    // Unaligned EBP frames: only visit 0x55 bytes instead of every address.
    const size_t DataN = static_cast<size_t>(std::min<va_t>(
        Seg->Data.size(), SecEnd > Seg->VA ? SecEnd - Seg->VA : 0));
    const uint8_t *Base = Seg->Data.data();
    const uint8_t *Cur = Base;
    const uint8_t *Limit = Base + DataN;
    while (Cur + 3 < Limit) {
      const void *Hit =
          std::memchr(Cur, 0x55, static_cast<size_t>(Limit - Cur));
      if (!Hit)
        break;
      const size_t Off =
          static_cast<size_t>(static_cast<const uint8_t *>(Hit) - Base);
      const va_t Addr = Seg->VA + Off;
      Cur = static_cast<const uint8_t *>(Hit) + 1;
      if ((Addr & 15u) == 0)
        continue;
      if (!isX86StrongFramePrologue(At(Addr)))
        continue;
      considerX86Prologue(Img, Dec, *Seg, Addr, Occupied, Out,
                          UnsymbolizedX86Entries);
    }

    // Packed `sub esp` immediately after ret / int3 / nop.
    for (size_t Off = 1; Off + 3 < DataN; ++Off) {
      if (!isX86FunctionEndBefore(SecBytes.data(), Off))
        continue;
      const va_t Addr = Seg->VA + Off;
      if ((Addr & 15u) == 0)
        continue;
      if (!isX86StackAdjustPrologue(At(Addr)))
        continue;
      considerX86Prologue(Img, Dec, *Seg, Addr, Occupied, Out,
                          UnsymbolizedX86Entries);
    }
  }
  Dec.setDetail(PrevDetail);
}

} // namespace neverd
