//===- WindowsProcessSEH.cpp - User C exception search and unwind --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessContext.h"
#include "WindowsProcessExceptions.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::windows_process {
using namespace value;

bool ExceptionDispatcher::hasFrameHandlers() const {
  return Modules && CPU.architecture() == GuestArchitecture::X64 &&
         llvm::any_of(Modules->Modules, [](const auto &M) {
           if (!resident(M) || !M.Loaded.Exceptions)
             return false;
           const auto &Info = *M.Loaded.Exceptions;
           if (Info.StructuralDecode && Info.StructuralDecode->ParseStatus !=
                                            ExceptionParseStatus::Complete)
             return true;
           return llvm::any_of(Info.Functions, [](const auto &F) {
             return F.UnwindFlags &
                    (seh::ExceptionHandlerFlag | seh::UnwindHandlerFlag);
           });
         });
}

llvm::Error ExceptionDispatcher::writeUnwindRecord() {
  const auto &F = Frames.back();
  auto Access = CPU.canAccess(F.Payload, ExceptionRecordSize,
                              Read | Write | UserAccessible);
  if (!Access)
    return Access.takeError();
  if (!*Access)
    return failure(text::ExceptionFrame);
  return CPU.write(F.Payload, F.SEH->Record);
}

llvm::Error ExceptionDispatcher::validateUnwind() {
  if (!Modules || !Budget)
    return failure(text::ExceptionUnhandled);
  for (const auto &Image : Frames.back().SEH->Images) {
    if (!current(*Modules, Image.Identity))
      return failure(text::ExceptionModuleRetired);
    const auto &Loaded = Modules->Modules[Image.Identity.Index].Loaded;
    if (auto E = validateImageMetadata(*Modules, Image.Identity.Index,
                                       Loaded.ExceptionMetadata, *Budget, CPU,
                                       text::ExceptionMetadataChanged))
      return E;
    // The loader proves personality identity from the original import thunk.
    // Guest IAT replacement must not retain that cached identity.
    for (const auto &I : Loaded.Imports) {
      if (!I.Target || I.Target->Kind != API::CSpecificHandler)
        continue;
      auto Access = CPU.canAccess(I.Slot, PointerSize, Read | UserAccessible);
      if (!Access)
        return Access.takeError();
      if (!*Access)
        return failure(text::ExceptionMetadataChanged);
      auto Target = CPU.readInteger(I.Slot, PointerSize);
      if (!Target)
        return Target.takeError();
      if (*Target != I.Gate)
        return failure(text::ExceptionMetadataChanged);
    }
  }
  return llvm::Error::success();
}

llvm::Expected<ExceptionDispatcher::Transfer>
ExceptionDispatcher::startUnwind() {
  if (!Modules || !Budget)
    return failure(text::ExceptionUnhandled);
  // A first-pass search cannot select a handler when no loaded image declares
  // one. Do not manufacture a stack walk through metadata-free entry routines.
  if (!hasFrameHandlers())
    return failure(text::ExceptionUnhandled);
  auto &F = Frames.back();
  const size_t Origin = Frames.size() - 1;
  F.SEH = std::make_unique<Unwind>();
  auto &S = *F.SEH;
  const Frame *Parent = Origin ? &Frames[Origin - 1] : nullptr;
  if (Parent && Parent->LoaderDepth != F.LoaderDepth)
    Parent = nullptr;
  const bool Nested = Parent && Parent->SEH && Parent->SEH->Callback;
  if (Nested)
    S.Images = Parent->SEH->Images;
  for (size_t I = 0; I < Modules->Modules.size(); ++I) {
    const auto &M = Modules->Modules[I];
    if (!resident(M))
      continue;
    const auto Identity = moduleRef(*Modules, I);
    if (llvm::any_of(S.Images, [&](const auto &Image) {
          return Image.Identity == Identity;
        }))
      continue;
    S.Images.push_back(
        {Identity, M.Loaded.Exceptions
                       ? M.Loaded.Exceptions
                       : std::make_shared<const ExceptionInfo>()});
  }
  if (auto E = validateUnwind())
    return std::move(E);
  std::vector<X64SEH::Image> Images;
  for (const auto &Image : S.Images) {
    const auto &Loaded = Modules->Modules[Image.Identity.Index].Loaded;
    Images.push_back({Image.Metadata.get(),
                      Loaded.PreferredBase ? Loaded.PreferredBase : Loaded.Base,
                      Loaded.Base,
                      Loaded.Size,
                      {}});
  }
  auto ReadGuest = [this](uint64_t Address,
                          llvm::MutableArrayRef<uint8_t> Bytes) -> llvm::Error {
    if (!Budget->remainingMicroseconds())
      return failure(text::ModuleTimeout);
    auto Access = CPU.canAccess(Address, Bytes.size(), Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return failure(text::ExceptionFrame);
    return CPU.read(Address, Bytes);
  };
  S.Planner = std::make_unique<X64SEH>(
      std::move(Images),
      [ReadGuest](uint64_t Address) -> llvm::Expected<uint64_t> {
        std::array<uint8_t, PointerSize> Bytes;
        if (auto E = ReadGuest(Address, Bytes))
          return std::move(E);
        return llvm::support::endian::read64le(Bytes.data());
      },
      [this](uint64_t PC) {
        auto Access = CPU.canAccess(PC, 1, Execute | UserAccessible);
        if (!Access) {
          llvm::consumeError(Access.takeError());
          return false;
        }
        return *Access;
      },
      ReadGuest);
  std::vector<uint8_t> Changed(F.Context.size());
  if (auto E = ReadGuest(F.Payload + ExceptionContextOffset, Changed))
    return std::move(E);
  if (auto E = restoreUserContext(CPU, *F.Snapshot, F.Context, Changed,
                                  StackBase, StackTop))
    return std::move(E);
  auto Caller = readUnwindContext(Changed);
  if (!Caller)
    return Caller.takeError();
  S.Record.resize(ExceptionRecordSize);
  if (auto E = ReadGuest(F.Payload, S.Record))
    return std::move(E);
  S.Flags =
      llvm::support::endian::read32le(S.Record.data() + ExceptionFlagsOffset);
  if (S.Flags & ~(ExceptionNoncontinuable | ExceptionSoftwareOriginate))
    return failure(text::ExceptionContext);
  const auto Code = llvm::support::endian::read32le(S.Record.data());
  const X64SEH::Stack Bounds{StackBase, StackTop - StackBase};
  S.Origins.push_back(Origin);
  if (Nested) {
    const auto &Suspended = *Parent->SEH;
    auto Cursor = S.Planner->beginNested(Code, *Caller, Bounds,
                                         Suspended.Cursor, *Suspended.Callback);
    if (!Cursor)
      return Cursor.takeError();
    S.Cursor = std::move(*Cursor);
    const size_t Start = Suspended.Callback->Kind == X64SEH::ActionKind::Filter
                             ? 0
                             : Suspended.Callback->SegmentIndex;
    S.Origins.insert(S.Origins.end(), Suspended.Origins.begin() + Start,
                     Suspended.Origins.end());
  } else
    S.Cursor = S.Planner->begin(Code, *Caller, Bounds);
  return advanceUnwind();
}

llvm::Expected<ExceptionDispatcher::Transfer>
ExceptionDispatcher::advanceUnwind(std::optional<int32_t> Filter) {
  auto &F = Frames.back();
  auto &S = *F.SEH;
  auto Next = S.Planner->advance(S.Cursor, Filter);
  if (!Next)
    return Next.takeError();
  using Kind = X64SEH::ActionKind;
  if (Next->Kind == Kind::Unhandled)
    return failure(text::ExceptionUnhandled);
  if (Next->Kind == Kind::ContinueExecution) {
    if (S.Flags & ExceptionNoncontinuable)
      return failure(text::ExceptionNoncontinuableFilter);
    llvm::support::endian::write32le(S.Record.data() + ExceptionFlagsOffset,
                                     S.Flags);
    if (auto E = writeUnwindRecord())
      return std::move(E);
    F.Kind = HandlerKind::Continue;
    F.Current = ContinueHandlers.begin();
    return callNext();
  }
  auto Context = F.Context;
  if (auto E = writeUnwindContext(Next->State.Registers, Context))
    return std::move(E);
  if (auto E = restoreUserContext(CPU, *F.Snapshot, F.Context, Context,
                                  StackBase, StackTop))
    return std::move(E);
  if (Next->Kind == Kind::Handler) {
    if (Next->SegmentIndex >= S.Origins.size())
      return failure(text::ExceptionFrame);
    const size_t Origin = S.Origins[Next->SegmentIndex];
    const uint64_t PC = Next->State.HandlerPC;
    // A nonlocal handler transfer abandons every intercepted callback between
    // the exception and the target frame. Abandoned API calls never returned.
    Frames.erase(Frames.begin() + Origin, Frames.end());
    collect();
    return Transfer{PC, std::nullopt};
  }
  llvm::support::endian::write32le(S.Record.data() + ExceptionFlagsOffset,
                                   S.Flags | Next->ExceptionFlags);
  if (auto E = writeUnwindRecord())
    return std::move(E);
  const uint64_t First =
      Next->Kind == Kind::Filter ? F.Payload + ExceptionPointersOffset : 1;
  auto Call =
      ABI.prepareCall(CPU, StackBase, F.Top - StackBase, ExceptionReturnGate,
                      {First, Next->State.EstablisherFrame},
                      ExceptionContextOffset + F.Context.size());
  if (!Call)
    return Call.takeError();
  if (auto E =
          CPU.writeRegister(CPURegister::X64PC, {Next->State.HandlerPC, 0}))
    return std::move(E);
  S.Callback = *Next;
  return Transfer{Next->State.HandlerPC, std::nullopt};
}
} // namespace neverd::emulation::windows_process
