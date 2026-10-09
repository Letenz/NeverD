//===- WindowsProcessServices.cpp - Bounded Win32 user API models --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessContext.h"
#include "WindowsProcessExceptions.h"
#include "WindowsProcessModules.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
constexpr Service Registry[] = {
#define NEVERD_WINDOWS_PROCESS_API(Name, Provider, Count, Returns)             \
  {API::Name, #Name, Count, APIProvider::Provider, Returns},
#define NEVERD_WINDOWS_PROCESS_NAMED_API(Name, Symbol, Provider, Count,        \
                                         Returns)                              \
  {API::Name, Symbol, Count, APIProvider::Provider, Returns},
#include "WindowsProcessServices.def"
#undef NEVERD_WINDOWS_PROCESS_NAMED_API
#undef NEVERD_WINDOWS_PROCESS_API
};
static_assert([] {
  for (auto &S : Registry)
    if (S.Arguments > std::tuple_size_v<decltype(NativeCallEvent::Arguments)>)
      return false;
  return true;
}());

const Service *findDeclaredService(llvm::StringRef Module,
                                   llvm::StringRef Name) {
  const auto Provider = findProvider(Module);
  std::string Alias;
  if (Provider == APIProvider::Native && Name.starts_with("Nt") &&
      nativeServiceNumber(Name)) {
    Alias = ("Zw" + Name.drop_front(2)).str();
    Name = Alias;
  }
  for (const auto &S : Registry)
    if (Name == S.Name && Provider == S.Provider)
      return &S;
  return nullptr;
}

bool permitsModule(const Service &S, llvm::StringRef Module) {
  switch (S.Kind) {
#define NEVERD_WINDOWS_PROCESS_API_MODULE(Name, ModuleName)                    \
  case API::Name:                                                              \
    return Module.equals_insensitive(text::ModuleName);
#include "WindowsProcessServices.def"
#undef NEVERD_WINDOWS_PROCESS_API_MODULE
  default:
    return true;
  }
}
} // namespace
llvm::Expected<ProcessDynamicThreadLocalState>
Services::dynamicThreadLocalState() const {
  uint8_t Bytes[DynamicTLSCount * PointerSize];
  if (auto E = Memory.snapshotBacking(TEB + TebTLSSlots, Bytes))
    return std::move(E);
  ProcessDynamicThreadLocalState State;
  State.FiberSlots = FLSSlots.count();
  for (uint64_t I = 0; I < DynamicTLSCount; ++I)
    if (TLSSlots[I] || llvm::support::endian::read64le(Bytes + I * PointerSize))
      ++State.ThreadSlots;
  return State;
}

llvm::ArrayRef<Service> services() { return Registry; }
std::optional<APIProvider> findProvider(llvm::StringRef Module) {
  const auto Lower = Module.lower();
  for (const auto &Provider : systemProviders())
    if (Lower == Provider.Name)
      return Provider.Family;
  return std::nullopt;
}
const Service *findService(llvm::StringRef Module, llvm::StringRef Name) {
  if (const auto *S = findDeclaredService(Module, Name))
    if (permitsModule(*S, Module))
      return S;
  return nullptr;
}
bool isServiceAbsent(llvm::StringRef Module, llvm::StringRef Name) {
  const auto *S = findDeclaredService(Module, Name);
  return S && !permitsModule(*S, Module);
}
std::optional<uint64_t> Services::unsupported(const Service &S) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = std::string(text::ServiceArguments) + S.Name;
  return std::nullopt;
}
llvm::Error Services::complete(const FLSCleanup &Cleanup) {
  auto I = FLSData.find(Cleanup.Index);
  if (I == FLSData.end() || !I->second.Cleaning ||
      I->second.Callback != Cleanup.Function)
    return failure(text::FLSCallback);
  // Keep the index allocated throughout the callback. Its value can be read
  // or changed by guest code, and another allocation must not reuse it yet.
  FLSSlots.reset(Cleanup.Index);
  FLSData.erase(I);
  return llvm::Error::success();
}
llvm::Expected<bool> Services::access(uint64_t Address, uint64_t Size,
                                      unsigned Rights) {
  if (!Size)
    return true;
  if (Address < ImageAlignment || Address >= UserLimit ||
      Size > UserLimit - Address)
    return false;
  return CPU.canAccess(Address, Size, Rights | UserAccessible);
}
llvm::Expected<uint64_t> Services::error(uint32_t Code, uint64_t ReturnValue) {
  if (auto E = CPU.writeInteger(TEB + TebLastError, Code, DWordSize))
    return std::move(E);
  return ReturnValue;
}
llvm::Expected<ServiceOutcome> Services::invoke(const Service &S,
                                                const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto WinError = [&](uint32_t Code, uint64_t ReturnValue =
                                         0) -> llvm::Expected<ServiceOutcome> {
    auto V = error(Code, ReturnValue);
    if (!V)
      return V.takeError();
    return ServiceOutcome(*V);
  };
  auto Value = [](uint64_t N) -> llvm::Expected<ServiceOutcome> {
    return ServiceOutcome(N);
  };
  auto Wrap = [](llvm::Expected<std::optional<uint64_t>> V)
      -> llvm::Expected<ServiceOutcome> {
    if (!V)
      return V.takeError();
    return ServiceOutcome(*V);
  };
  switch (S.Kind) {
  case API::RtlCaptureContext:
  case API::NativeCaptureContext:
    if (auto E = captureCallerContext(CPU, A[0]))
      return std::move(E);
    return Value(0);
  case API::CSpecificHandler:
    return failure(text::ExceptionPersonalityCall);
  case API::AddVectoredExceptionHandler:
  case API::AddVectoredContinueHandler: {
    const auto Kind = S.Kind == API::AddVectoredContinueHandler
                          ? ExceptionDispatcher::HandlerKind::Continue
                          : ExceptionDispatcher::HandlerKind::Exception;
    auto Handle = Exceptions.add(Kind, uint32_t(A[0]) != 0, A[1]);
    if (!Handle)
      return Handle.takeError();
    return Value(*Handle);
  }
  case API::RemoveVectoredExceptionHandler:
    return Value(
        Exceptions.remove(ExceptionDispatcher::HandlerKind::Exception, A[0]));
  case API::RemoveVectoredContinueHandler:
    return Value(
        Exceptions.remove(ExceptionDispatcher::HandlerKind::Continue, A[0]));
  case API::RaiseException: {
    const uint32_t Flags = A[1];
    const uint32_t Count = A[3] ? uint32_t(A[2]) : 0;
    if ((Flags & ~ExceptionNoncontinuable) || Count > MaxExceptionArguments)
      return failure(text::ExceptionArguments);
    auto Access = access(A[3], Count * PointerSize, Read);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return failure(text::Access);
    ServiceOutcome::Exception Raised{
        uint32_t(A[0]),
        uint32_t(Flags | ExceptionSoftwareOriginate),
        0, // The process dispatcher binds the executable continuation address.
        {}};
    for (uint32_t I = 0; I < Count; ++I) {
      auto Argument = CPU.readInteger(A[3] + I * PointerSize, PointerSize);
      if (!Argument)
        return Argument.takeError();
      Raised.Arguments.push_back(*Argument);
    }
    return ServiceOutcome(std::move(Raised));
  }
  case API::ExitProcess:
  case API::RtlExitUserProcess:
    Result.ExitStatus = uint32_t(A[0]);
    Result.Stop = ProcessStopReason::Exited;
    return ServiceOutcome(std::nullopt);
  case API::GetLastError: {
    auto V = CPU.readInteger(TEB + TebLastError, DWordSize);
    if (!V)
      return V.takeError();
    return Value(*V);
  }
  case API::SetLastError:
    if (auto E =
            CPU.writeInteger(TEB + TebLastError, uint32_t(A[0]), DWordSize))
      return std::move(E);
    return Value(0);
  case API::GetCurrentProcessId:
    return Value(ProcessID);
  case API::GetCurrentThreadId:
    return Value(ThreadID);
  case API::GetCurrentProcess:
    return Value(CurrentProcess);
  case API::GetCurrentThread:
    return Value(CurrentThread);
  case API::GetCommandLineW:
    return Value(Env.CommandLine);
  case API::GetEnvironmentVariableW:
  case API::SetEnvironmentVariableW:
  case API::GetEnvironmentStringsW:
  case API::FreeEnvironmentStringsW:
  case API::ExpandEnvironmentStringsW:
    return Wrap(environment(S, Event));
  case API::GetProcessHeap:
    return Value(HeapHandle);
  case API::VirtualAlloc:
  case API::VirtualFree:
  case API::VirtualProtect:
  case API::VirtualQuery:
  case API::FlushInstructionCache:
    return Wrap(memory(S, Event));
  case API::WriteProcessMemory:
    return Wrap(writeProcessMemory(S, Event));
  case API::HeapAlloc:
  case API::RtlAllocateHeap:
  case API::HeapReAlloc:
  case API::RtlReAllocateHeap:
  case API::HeapFree:
  case API::RtlFreeHeap:
  case API::HeapSize:
  case API::RtlSizeHeap:
  case API::HeapCreate:
  case API::HeapDestroy:
  case API::HeapSetInformation:
    return Wrap(heap(S, Event));
  case API::ZwOpenFile:
    return Wrap(openImage(S, Event));
  case API::ZwCreateSection:
    return Wrap(createSection(S, Event));
  case API::ZwOpenSection:
    return Wrap(openSection(S, Event));
  case API::ZwMapViewOfSection:
    return Wrap(mapSection(S, Event));
  case API::ZwUnmapViewOfSection:
    return Wrap(unmapSection(S, Event));
  case API::ZwClose:
    return Wrap(closeHandle(S, Event));
  case API::ZwProtectVirtualMemory:
    return Wrap(protectMemory(S, Event));
  case API::ZwQuerySystemInformation:
    return Wrap(querySystem(S, Event));
  case API::ZwQueryInformationProcess:
    return Wrap(queryProcess(S, Event));
  case API::ZwQueryInformationThread:
    return Wrap(queryThread(S, Event));
  case API::ZwSetInformationThread:
    return Wrap(setThread(S, Event));
  case API::ZwDelayExecution:
    return Wrap(delay(S, Event));
  case API::GetSystemTimeAsFileTime:
  case API::GetTickCount:
  case API::QueryPerformanceCounter:
  case API::QueryPerformanceFrequency:
    return Wrap(clock(S, Event));
  case API::EncodePointer:
  case API::DecodePointer:
  case API::RtlEncodePointer:
  case API::RtlDecodePointer:
    return Wrap(encodePointer(S, Event));
  case API::InitializeCriticalSection:
  case API::InitializeCriticalSectionAndSpinCount:
  case API::RtlInitializeCriticalSection:
  case API::RtlInitializeCriticalSectionAndSpinCount:
  case API::EnterCriticalSection:
  case API::LeaveCriticalSection:
  case API::TryEnterCriticalSection:
  case API::RtlEnterCriticalSection:
  case API::RtlLeaveCriticalSection:
  case API::RtlTryEnterCriticalSection:
  case API::DeleteCriticalSection:
  case API::RtlDeleteCriticalSection:
    return Wrap(criticalSection(S, Event));
  case API::GetCommandLineA:
  case API::GetStartupInfoA:
  case API::GetFileType:
  case API::SetHandleCount:
  case API::GetACP:
  case API::IsValidCodePage:
  case API::GetCPInfo:
  case API::GetStringTypeW:
  case API::WideCharToMultiByte:
  case API::MultiByteToWideChar:
  case API::LCMapStringW:
  case API::GetModuleFileNameA:
    return Wrap(crt(S, Event));
  case API::CreateToolhelp32Snapshot:
  case API::Thread32First:
  case API::Thread32Next:
  case API::CloseHandle:
  case API::GetSystemInfo:
  case API::RtlSetThreadErrorMode:
    return Wrap(toolhelp(S, Event));
  case API::FlsAlloc:
    for (uint32_t I = 0; I < DynamicTLSCount; ++I) {
      if (FLSSlots[I])
        continue;
      FLSSlots.set(I);
      FLSData[I] = {A[0], 0};
      return Value(I);
    }
    return WinError(ErrorNotEnoughMemory, TLSOutOfIndexes);
  case API::FlsFree:
  case API::FlsGetValue:
  case API::FlsSetValue: {
    const uint32_t Index = A[0];
    if (Index >= DynamicTLSCount || !FLSSlots[Index])
      return WinError(ErrorInvalidParameter, 0);
    auto &Data = FLSData[Index];
    if (S.Kind == API::FlsGetValue)
      return WinError(ErrorSuccess, Data.Value);
    if (S.Kind == API::FlsFree) {
      // Recursive release of the same in-flight index has no modeled
      // lifecycle. Other indices may create nested cleanup continuations.
      if (Data.Cleaning)
        return ServiceOutcome(unsupported(S));
      if (Data.Callback && Data.Value) {
        Data.Cleaning = true;
        return ServiceOutcome(FLSCleanup{Index, Data.Callback, Data.Value});
      }
      FLSSlots.reset(Index);
      FLSData.erase(Index);
    } else
      Data.Value = A[1];
    return Value(1);
  }
  case API::GetStdHandle:
    switch (uint32_t(A[0])) {
    case StdInputSelector:
      return Value(StandardInput);
    case StdOutputSelector:
      return Value(StandardOutput);
    case StdErrorSelector:
      return Value(StandardError);
    default:
      return WinError(ErrorInvalidParameter, InvalidHandle);
    }
  case API::WriteFile: {
    // Only synchronous writes to the two explicit byte sinks are modeled.
    // Check every argument before publishing output or a guest completion.
    if (A[4])
      return ServiceOutcome(unsupported(S));
    const uint64_t Count = uint32_t(A[2]);
    auto Written = access(A[3], DWordSize, Write);
    if (!Written)
      return Written.takeError();
    if (!*Written) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = text::UserException;
      return ServiceOutcome(std::nullopt);
    }
    if (A[0] != StandardOutput && A[0] != StandardError) {
      if (auto E = CPU.writeInteger(A[3], 0, DWordSize))
        return std::move(E);
      return WinError(ErrorInvalidHandle);
    }
    auto Bytes = access(A[1], Count, Read);
    if (!Bytes)
      return Bytes.takeError();
    if (!*Bytes) {
      if (auto E = CPU.writeInteger(A[3], 0, DWordSize))
        return std::move(E);
      return WinError(ErrorInvalidUserBuffer);
    }
    if (Count > Options.OutputLimit - Result.StandardOutput.size() -
                    Result.StandardError.size()) {
      Result.Stop = ProcessStopReason::OutputLimit;
      Result.Diagnostic = text::Output;
      return ServiceOutcome(std::nullopt);
    }
    // Win32 clears the completion count before copying the input. In
    // particular, the buffer may overlap this DWORD or a return-address slot.
    if (auto E = CPU.writeInteger(A[3], 0, DWordSize))
      return std::move(E);
    std::vector<uint8_t> Data(Count);
    if (Count)
      if (auto E = CPU.read(A[1], Data))
        return std::move(E);
    if (auto E = CPU.writeInteger(A[3], Count, DWordSize))
      return std::move(E);
    auto &Output =
        A[0] == StandardOutput ? Result.StandardOutput : Result.StandardError;
    Output.append(Data.begin(), Data.end());
    return Value(1);
  }
  case API::TlsAlloc:
    for (uint32_t I = 0; I < DynamicTLSCount; ++I) {
      if (TLSSlots[I])
        continue;
      if (auto E = CPU.writeInteger(TEB + TebTLSSlots + I * PointerSize, 0,
                                    PointerSize))
        return std::move(E);
      TLSSlots.set(I);
      return Value(I);
    }
    return WinError(ErrorNotEnoughMemory, TLSOutOfIndexes);
  case API::TlsFree:
  case API::TlsSetValue:
  case API::TlsGetValue: {
    const uint32_t Index = A[0];
    if (Index >= DynamicTLSCount ||
        (S.Kind == API::TlsFree && !TLSSlots[Index]))
      return WinError(ErrorInvalidParameter);
    const uint64_t Address = TEB + TebTLSSlots + Index * PointerSize;
    if (S.Kind == API::TlsGetValue) {
      auto V = CPU.readInteger(Address, PointerSize);
      if (!V)
        return V.takeError();
      return WinError(ErrorSuccess, *V);
    }
    if (S.Kind == API::TlsFree) {
      if (auto E = CPU.writeInteger(Address, 0, PointerSize))
        return std::move(E);
      TLSSlots.reset(Index);
    } else if (auto E = CPU.writeInteger(Address, A[1], PointerSize))
      return std::move(E);
    return Value(1);
  }
  case API::FreeLibrary:
    return ServiceOutcome(
        LoaderRequest{LoaderRequest::Kind::Free, A[0], {}, {}});
  case API::LoadLibraryA:
  case API::LoadLibraryW:
  case API::GetModuleHandleA:
  case API::GetModuleHandleW: {
    const bool Handle =
        S.Kind == API::GetModuleHandleW || S.Kind == API::GetModuleHandleA;
    if (!A[0])
      return Handle ? Value(Loaded.Base)
                    : llvm::Expected<ServiceOutcome>(failure(text::Access));
    const unsigned Unit =
        S.Kind == API::LoadLibraryA || S.Kind == API::GetModuleHandleA
            ? 1
            : WideSize;
    std::string Name;
    for (uint64_t I = 0; I < MaxName; ++I) {
      if (!Budget.remainingMicroseconds())
        return failure(text::ModuleTimeout);
      if (Modules.Reads.MetadataBytes < Unit)
        return failure(text::ExportBudget);
      Modules.Reads.MetadataBytes -= Unit;
      if (A[0] >= UserLimit || I * Unit + Unit > UserLimit - A[0])
        return failure(text::Access);
      auto Accessible = access(A[0] + I * Unit, Unit, Read);
      if (!Accessible)
        return Accessible.takeError();
      if (!*Accessible)
        return failure(text::Access);
      auto C = CPU.readInteger(A[0] + I * Unit, Unit);
      if (!C)
        return C.takeError();
      if (!*C) {
        if (Name.empty() || Name.back() == '.')
          return ServiceOutcome(unsupported(S));
        if (!llvm::StringRef(Name).contains('.'))
          Name += text::DLLExtension;
        if (!Handle) {
          auto Key = moduleName(Name);
          if (!Key)
            return Key.takeError();
          return ServiceOutcome(
              LoaderRequest{LoaderRequest::Kind::Load, 0, *Key, {}});
        }
        if (auto M = findModule(Modules, Name))
          return Value(Modules.Modules[*M].Loaded.Base);
        return WinError(ErrorModuleNotFound);
      }
      if (*C > ASCIIUpperBound || !(llvm::isAlnum(char(*C)) || *C == '_' ||
                                    *C == '-' || *C == '.' || *C == ' '))
        return ServiceOutcome(unsupported(S));
      Name += char(*C);
    }
    return ServiceOutcome(unsupported(S));
  }
  case API::GetProcAddress: {
    auto Module = llvm::find_if(Modules.Identities, [&](const auto &M) {
      return M.Base && M.Base == A[0];
    });
    if (Module == Modules.Identities.end())
      return ServiceOutcome(unsupported(S));
    std::optional<uint16_t> Ordinal;
    std::string Name;
    if (A[1] <= ImportOrdinalMask)
      Ordinal = uint16_t(A[1]);
    else {
      bool Terminated = false;
      for (uint64_t I = 0; I < MaxName; ++I) {
        if (!Budget.remainingMicroseconds())
          return failure(text::ModuleTimeout);
        if (!Modules.Reads.MetadataBytes)
          return failure(text::ExportBudget);
        --Modules.Reads.MetadataBytes;
        if (A[1] >= UserLimit || I >= UserLimit - A[1])
          return failure(text::Access);
        auto Accessible = access(A[1] + I, 1, Read);
        if (!Accessible)
          return Accessible.takeError();
        if (!*Accessible)
          return failure(text::Access);
        auto C = CPU.readInteger(A[1] + I, 1);
        if (!C)
          return C.takeError();
        if (!*C) {
          Terminated = true;
          break;
        }
        Name += char(*C);
      }
      if (!Terminated)
        return ServiceOutcome(unsupported(S));
    }
    return ServiceOutcome(LoaderRequest{LoaderRequest::Kind::Export, A[0],
                                        std::move(Name), Ordinal});
  }
  }
  return failure(text::Service);
}
} // namespace neverd::emulation::windows_process
