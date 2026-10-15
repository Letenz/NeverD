//===- KernelModelSLists.cpp - Sequenced singly linked lists --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lock-free x64 SLIST_HEADER updates as the Windows 10.0.19041.6456 kernel
/// performs them with cmpxchg16b, which is atomic on this single processor.
///
//===----------------------------------------------------------------------===//

#include "KernelException.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
llvm::Error sListError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}

uint64_t firstEntry(uint64_t Region) {
  return Region & ~windows::SListReservedMask;
}
} // namespace

llvm::Error KernelModel::validateSListStorage(uint64_t Address, uint32_t Size,
                                              bool IsWrite) const {
  if (auto E = validateGuestAccess(Address, Size, IsWrite))
    return E;
  if (!IsWrite)
    return llvm::Error::success();
  auto Writable = Memory.canAccess(Address, Size, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return sListError("sequenced list storage requires writable memory");
  return llvm::Error::success();
}

llvm::Expected<std::pair<uint64_t, uint64_t>>
KernelModel::readSListHeader(uint64_t Header, bool IsWrite) const {
  // cmpxchg16b faults on a header that is not 16-byte aligned.
  if (Header & (windows::SListAlignment - 1))
    return sListError("sequenced list header requires 16-byte alignment");
  if (auto E = validateSListStorage(Header, windows::SListHeaderSize, IsWrite))
    return E;
  auto Alignment = Memory.readInteger(Header, 8);
  if (!Alignment)
    return Alignment.takeError();
  auto Region = Memory.readInteger(Header + windows::SListHeaderEntryOffset, 8);
  if (!Region)
    return Region.takeError();
  return std::make_pair(*Alignment, *Region);
}

llvm::Expected<uint64_t> KernelModel::initializeSList(uint64_t Header) {
  // InitializeSListHead (RVA 0x352660) raises STATUS_DATATYPE_MISALIGNMENT for
  // an unaligned header and otherwise zeroes it.
  if (Header & (windows::SListAlignment - 1))
    return llvm::make_error<KernelGuestException>(
        exceptions::StatusDatatypeMisalignment);
  if (auto E = validateSListStorage(Header, windows::SListHeaderSize, true))
    return E;
  if (auto E = Memory.writeInteger(Header, 0, 8))
    return E;
  if (auto E =
          Memory.writeInteger(Header + windows::SListHeaderEntryOffset, 0, 8))
    return E;
  return 0;
}

llvm::Expected<uint64_t> KernelModel::pushEntrySList(uint64_t Header,
                                                     uint64_t Entry) {
  // ExpInterlockedPushEntrySList (RVA 0x407040) links the entry to the old
  // first entry and adds one to Depth and Sequence as a single quadword.
  auto State = readSListHeader(Header, true);
  if (!State)
    return State.takeError();
  if (Entry & (windows::SListAlignment - 1))
    return sListError("sequenced list entries require 16-byte alignment");
  if (Entry == Header)
    return sListError("sequenced list entry overlaps its header");
  if (auto E = validateSListStorage(Entry, 8, true))
    return E;
  const uint64_t First = firstEntry(State->second);
  if (auto E = Memory.writeInteger(Entry, First, 8))
    return E;
  if (auto E = Memory.writeInteger(
          Header, State->first + windows::SListSequenceUnit + 1, 8))
    return E;
  if (auto E = Memory.writeInteger(Header + windows::SListHeaderEntryOffset,
                                   Entry, 8))
    return E;
  return First;
}

llvm::Expected<uint64_t> KernelModel::popEntrySList(uint64_t Header) {
  // ExpInterlockedPopEntrySList (RVA 0x407000) decrements only the 16-bit
  // Depth, keeps Sequence, and makes the popped entry's link, unchanged, the
  // new first entry. An empty list stays as it was.
  auto State = readSListHeader(Header, true);
  if (!State)
    return State.takeError();
  const uint64_t First = firstEntry(State->second);
  if (!First)
    return 0;
  if (auto E = validateSListStorage(First, 8, false))
    return E;
  auto Next = Memory.readInteger(First, 8);
  if (!Next)
    return Next.takeError();
  const uint64_t Depth = (State->first - 1) & windows::SListDepthMask;
  if (auto E = Memory.writeInteger(
          Header, (State->first & ~windows::SListDepthMask) | Depth, 8))
    return E;
  if (auto E = Memory.writeInteger(Header + windows::SListHeaderEntryOffset,
                                   *Next, 8))
    return E;
  return First;
}

llvm::Expected<uint64_t> KernelModel::flushSList(uint64_t Header) {
  // ExpInterlockedFlushSList (RVA 0x407080) clears Depth and the first entry,
  // keeps Sequence and leaves every entry's link in place.
  auto State = readSListHeader(Header, true);
  if (!State)
    return State.takeError();
  if (auto E = Memory.writeInteger(Header,
                                   State->first & ~windows::SListDepthMask, 8))
    return E;
  if (auto E =
          Memory.writeInteger(Header + windows::SListHeaderEntryOffset, 0, 8))
    return E;
  return firstEntry(State->second);
}

llvm::Expected<uint64_t> KernelModel::querySList(uint64_t Header,
                                                 bool FirstEntry) {
  // ExQueryDepthSList (RVA 0x2524e0) and FirstEntrySList (RVA 0x406ff0) only
  // read the header.
  auto State = readSListHeader(Header, false);
  if (!State)
    return State.takeError();
  return FirstEntry ? firstEntry(State->second)
                    : State->first & windows::SListDepthMask;
}
} // namespace neverd::emulation
