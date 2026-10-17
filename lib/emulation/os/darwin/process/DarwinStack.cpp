//===- DarwinStack.cpp - argc/argv/envp/apple startup storage -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinProcess.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
llvm::Expected<InitialStack> prepareStack(GuestMemory &Memory,
                                          const ProcessOptions &Options,
                                          llvm::StringRef ExecutableName,
                                          uint64_t MainHeader) {
  const uint64_t Base = value::StackTop - Options.StackSize;
  std::vector<uint8_t> Bytes(Options.StackSize, 0);
  uint64_t Cursor = Bytes.size();
  auto PushString = [&](llvm::StringRef Text) -> llvm::Expected<uint64_t> {
    if (Text.contains('\0'))
      return failure(diagnostic::StartupNul);
    if (Text.size() >= Cursor)
      return failure(diagnostic::StartupArguments);
    Cursor -= Text.size() + 1;
    std::copy(Text.begin(), Text.end(), Bytes.begin() + Cursor);
    return Base + Cursor;
  };
  auto Executable =
      PushString((value::ExecutablePathPrefix + ExecutableName).str());
  if (!Executable)
    return Executable.takeError();
  uint64_t Progname = 0, Vars = 0;
  if (MainHeader) {
    auto Name = PushString(ExecutableName);
    if (!Name)
      return Name.takeError();
    Progname = *Name;
    if (Cursor < 80)
      return failure(diagnostic::StartupVector);
    Cursor = (Cursor - 80) & ~uint64_t(15);
    Vars = Base + Cursor;
  }
  const auto Arguments = Options.Arguments.empty()
                             ? std::vector<std::string>{ExecutableName.str()}
                             : Options.Arguments;
  std::vector<uint64_t> Words{Arguments.size()};
  for (const auto *List : {&Arguments, &Options.Environment}) {
    for (const auto &Text : *List) {
      auto Address = PushString(Text);
      if (!Address)
        return Address.takeError();
      Words.push_back(*Address);
    }
    Words.push_back(0);
  }
  Words.push_back(*Executable);
  Words.push_back(0);
  if (Words.size() > Cursor / 8 || Cursor - Words.size() * 8 < 256)
    return failure(diagnostic::StartupVector);
  Cursor = (Cursor - Words.size() * 8) & ~uint64_t(15);
  const uint64_t SP = Base + Cursor;
  for (const auto Word : Words) {
    llvm::support::endian::write64le(Bytes.data() + Cursor, Word);
    Cursor += 8;
  }
  InitialStack Stack{
      SP,
      Arguments.size(),
      SP + 8,
      SP + 8 * (Arguments.size() + 2),
      SP + 8 * (Arguments.size() + Options.Environment.size() + 3),
      Vars};
  if (Vars) {
    auto *Data = Bytes.data() + (Vars - Base);
    // ProgramVars contains the header and pointers to process-owned variables.
    // argv and envp each require one additional level of indirection.
    const uint64_t Fields[] = {MainHeader, Vars + 40, Vars + 48, Vars + 56,
                               Vars + 64};
    for (unsigned I = 0; I != 5; ++I)
      llvm::support::endian::write64le(Data + 8 * I, Fields[I]);
    llvm::support::endian::write32le(Data + 40, Stack.Argc);
    llvm::support::endian::write64le(Data + 48, Stack.Argv);
    llvm::support::endian::write64le(Data + 56, Stack.Envp);
    llvm::support::endian::write64le(Data + 64, Progname);
  }
  if (auto E = Memory.write(Base, Bytes))
    return std::move(E);
  return Stack;
}
} // namespace neverd::emulation::darwin_model
