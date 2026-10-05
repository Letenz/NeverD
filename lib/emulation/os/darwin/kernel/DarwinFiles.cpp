//===- DarwinFiles.cpp - Workload-owned Darwin file services --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original model of the BSD ABI described in the pinned XNU sources linked
// from docs/darwin-emulation.md. No host descriptors or filesystem calls.
#include "DarwinFiles.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
namespace limits = darwin_file_limits;
bool canonicalPath(llvm::StringRef Path) {
  if (!Path.consume_front("/") || Path.empty() || Path.contains('\0'))
    return false;
  llvm::SmallVector<llvm::StringRef> Parts;
  Path.split(Parts, '/');
  return llvm::all_of(Parts, [](llvm::StringRef Part) {
    return !Part.empty() && Part != "." && Part != "..";
  });
}
std::optional<ServiceResult> returned(uint64_t Value, bool Error = false) {
  return ServiceResult{Value, Error};
}
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
} // namespace

llvm::Error validateFileOptions(const DarwinFileOptions &Options) {
  if (Options.DescriptorLimit < 3 ||
      Options.DescriptorLimit > limits::Descriptors ||
      Options.Files.size() > limits::Files)
    return failure(diagnostic::FileOptionsLimit);
  uint64_t Total = Options.StandardInput ? Options.StandardInput->size() : 0;
  if (Total > limits::Bytes)
    return failure(diagnostic::FileOptionsLimit);
  for (const auto &[Path, Bytes] : Options.Files) {
    if (Path.size() >= limits::Path || !canonicalPath(Path))
      return failure(diagnostic::FileOptionPath);
    llvm::SmallVector<llvm::StringRef> Parts;
    llvm::StringRef(Path).split(Parts, '/');
    if (llvm::any_of(Parts,
                     [](llvm::StringRef P) { return P.size() > limits::Name; }))
      return failure(diagnostic::FileOptionPath);
    for (size_t I = Path.find('/', 1); I != std::string::npos;
         I = Path.find('/', I + 1))
      if (Options.Files.contains(Path.substr(0, I)))
        return failure(diagnostic::FileOptionPath);
    const uint64_t Cost = Path.size() + 1;
    if (Cost > limits::Bytes - Total ||
        Bytes.size() > limits::Bytes - Total - Cost)
      return failure(diagnostic::FileOptionsLimit);
    Total += Cost + Bytes.size();
  }
  return llvm::Error::success();
}

DarwinFiles::DarwinFiles(GuestMemory &Memory,
                         const std::optional<DarwinFileOptions> &Options)
    : Memory(Memory), Options(Options) {
  const llvm::ArrayRef<uint8_t> Input =
      Options && Options->StandardInput
          ? llvm::ArrayRef<uint8_t>(*Options->StandardInput)
          : llvm::ArrayRef<uint8_t>();
  Descriptors.emplace(0, Descriptor{std::make_shared<Description>(
                             Description{Kind::Input, Input})});
  Descriptors.emplace(1, Descriptor{std::make_shared<Description>(
                             Description{Kind::Output, {}})});
  Descriptors.emplace(2, Descriptor{std::make_shared<Description>(
                             Description{Kind::Error, {}})});
}
uint32_t DarwinFiles::limit() const {
  return Options ? Options->DescriptorLimit : limits::DefaultDescriptors;
}
uint32_t DarwinFiles::freeDescriptor(uint32_t Minimum) const {
  while (Minimum < limit() && Descriptors.contains(Minimum))
    ++Minimum;
  return Minimum;
}
std::optional<unsigned> DarwinFiles::outputSink(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I != Descriptors.end()) {
    if (I->second.Open->Type == Kind::Output)
      return 1;
    if (I->second.Open->Type == Kind::Error)
      return 2;
  }
  return std::nullopt;
}

llvm::Expected<DarwinFiles::Pathname> DarwinFiles::readPath(uint64_t Address) {
  std::string Path;
  for (uint64_t I = 0; I < limits::Path; ++I) {
    if (Address >= UserLimit || I >= UserLimit - Address)
      return uint32_t(BadAddress);
    auto Access = Memory.canAccess(Address + I, 1, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return uint32_t(BadAddress);
    auto Byte = Memory.readInteger(Address + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      return Path.empty() ? Pathname(uint32_t(NoEntry)) : Pathname(Path);
    Path.push_back(*Byte);
  }
  return uint32_t(NameTooLong);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::open(uint64_t Address, uint32_t Flags, ProcessResult &Result) {
  if (!Options)
    return unsupported(Result, diagnostic::FileInputs);
  if (Flags & ~uint32_t(OpenCloseOnExec))
    return unsupported(Result, diagnostic::FileOpenFlags);
  // XNU reserves the descriptor before resolving the pathname. A failed
  // open does not retain that reservation.
  const uint32_t FD = freeDescriptor();
  if (FD == limit())
    return returned(TooManyFiles, true);
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Imported))
    return returned(*Error, true);
  const auto &Path = std::get<std::string>(*Imported);
  llvm::SmallVector<llvm::StringRef> Parts;
  llvm::StringRef(Path).split(Parts, '/');
  if (Path == "/")
    return unsupported(Result, diagnostic::FileDirectory);
  if (!canonicalPath(Path))
    return unsupported(Result, diagnostic::FilePathForm);
  // Resolve one component at a time: a missing or nondirectory ancestor
  // precedes a too-long later component, just as in the native name lookup.
  std::string Prefix;
  for (auto Part : llvm::ArrayRef<llvm::StringRef>(Parts).drop_front()) {
    if (Part.size() > limits::Name)
      return returned(NameTooLong, true);
    Prefix += '/';
    Prefix.append(Part.data(), Part.size());
    auto File = Options->Files.find(Prefix);
    if (File != Options->Files.end()) {
      if (Prefix.size() != Path.size())
        return returned(NotDirectory, true);
      Descriptors.emplace(FD,
                          Descriptor{std::make_shared<Description>(
                                         Description{Kind::File, File->second}),
                                     bool(Flags & OpenCloseOnExec)});
      return returned(FD);
    }
    const auto Directory = Prefix + '/';
    auto Next = Options->Files.lower_bound(Directory);
    if (Next == Options->Files.end() ||
        !llvm::StringRef(Next->first).starts_with(Directory))
      return returned(NoEntry, true);
  }
  return unsupported(Result, diagnostic::FileDirectory);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::read(Description &File, uint64_t Address, uint64_t Count,
                  uint64_t Offset, bool Positioned, ProcessResult &Result) {
  if (File.Type == Kind::Output || File.Type == Kind::Error)
    return returned(BadDescriptor, true);
  if (Positioned && File.Type != Kind::File)
    return returned(IllegalSeek, true);
  if (Offset > INT64_MAX)
    return returned(InvalidArgument, true);
  if (Count && File.Type == Kind::Input &&
      (!Options || !Options->StandardInput))
    return unsupported(Result, diagnostic::FileInput);
  const uint64_t Available =
      File.Bytes.size() - std::min<uint64_t>(Offset, File.Bytes.size());
  Count = std::min(Count, Available);
  if (!Count)
    return returned(0);
  if (Address >= UserLimit)
    return returned(BadAddress, true);
  // Copyout failures inside one filesystem transfer do not have a portable
  // partial-cursor contract. Reject a writable prefix before any effect.
  uint64_t Checked = 0;
  while (Checked < Count) {
    const uint64_t Start = Address + Checked;
    const uint64_t Size = std::min(Count - Checked, 4096 - Start % 4096);
    auto Access = Memory.canAccess(Start, Size, Write | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (Start >= UserLimit || !*Access)
      return Checked ? unsupported(Result, diagnostic::FilePartialRead)
                     : returned(BadAddress, true);
    Checked += Size;
  }
  if (auto E = Memory.write(Address, File.Bytes.slice(Offset, Count)))
    return std::move(E);
  if (!Positioned)
    File.Offset += Count;
  return returned(Count);
}

ServiceResult DarwinFiles::seek(Description &File, uint64_t Offset,
                                uint32_t Whence) {
  if (File.Type != Kind::File)
    return {IllegalSeek, true};
  uint64_t Base;
  switch (Whence) {
  case 0:
    Base = 0;
    break;
  case 1:
    Base = File.Offset;
    break;
  case 2:
    Base = File.Bytes.size();
    break;
  default:
    return {InvalidArgument, true};
  }
  if (Offset <= INT64_MAX) {
    if (Offset > uint64_t(INT64_MAX) - Base)
      return {Overflow, true};
    Base += Offset;
  } else {
    const uint64_t Magnitude = uint64_t(0) - Offset;
    if (Magnitude > Base)
      return {InvalidArgument, true};
    Base -= Magnitude;
  }
  File.Offset = Base;
  return {Base, false};
}

ServiceResult DarwinFiles::duplicate(const Descriptor &Source, uint32_t Minimum,
                                     bool CloseOnExec) {
  const uint32_t FD = freeDescriptor(Minimum);
  if (FD >= limit())
    return {TooManyFiles, true};
  Descriptors.emplace(FD, Descriptor{Source.Open, CloseOnExec});
  return {FD, false};
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::handle(ServiceKind Service, const ProcessServiceEvent &Event,
                    ProcessResult &Result) {
  const auto &A = Event.Arguments;
  if (Service == ServiceKind::Open)
    return open(A[0], A[1], Result);
  if ((Service == ServiceKind::Read || Service == ServiceKind::Pread) &&
      A[2] > MaxWriteBytes)
    return returned(InvalidArgument, true);
  auto I = Descriptors.find(uint32_t(A[0]));
  if (I == Descriptors.end())
    return returned(BadDescriptor, true);
  auto &FD = I->second;
  auto &File = *FD.Open;
  switch (Service) {
  case ServiceKind::Read:
  case ServiceKind::Pread:
    return read(File, A[1], A[2],
                Service == ServiceKind::Pread ? A[3] : File.Offset,
                Service == ServiceKind::Pread, Result);
  case ServiceKind::Close:
    Descriptors.erase(I);
    return returned(0);
  case ServiceKind::Lseek:
    if (File.Type == Kind::File && (uint32_t(A[2]) == 3 || uint32_t(A[2]) == 4))
      return unsupported(Result, diagnostic::FileSeek);
    return std::optional<ServiceResult>(seek(File, A[1], A[2]));
  case ServiceKind::Dup:
    return std::optional<ServiceResult>(duplicate(FD, 0, false));
  case ServiceKind::Dup2: {
    const uint32_t Target = A[1];
    if (Target >= limit())
      return returned(BadDescriptor, true);
    if (Target != I->first)
      Descriptors.insert_or_assign(Target, Descriptor{FD.Open, false});
    return returned(Target);
  }
  case ServiceKind::Fcntl:
    switch (uint32_t(A[1])) {
    case DuplicateFD:
    case DuplicateCloseOnExec:
      if (uint32_t(A[2]) >= limit())
        return returned(InvalidArgument, true);
      return std::optional<ServiceResult>(
          duplicate(FD, A[2], uint32_t(A[1]) == DuplicateCloseOnExec));
    case GetDescriptorFlags:
      return returned(FD.CloseOnExec ? 1 : 0);
    case SetDescriptorFlags:
      FD.CloseOnExec = A[2] & 1;
      return returned(0);
    case GetFileFlags:
      return returned(
          File.Type == Kind::Output || File.Type == Kind::Error ? 1 : 0);
    default:
      return unsupported(Result, diagnostic::FileControl);
    }
  default:
    llvm_unreachable("non-file Darwin service");
  }
}
} // namespace neverd::emulation::darwin_model
