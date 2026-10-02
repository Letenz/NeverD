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
                                          llvm::StringRef ExecutableName) {
  const uint64_t Base = value::StackTop - Options.StackSize;
  std::vector<uint8_t> Bytes(Options.StackSize, 0);
  uint64_t Cursor = Bytes.size();
  auto PushString = [&](llvm::StringRef Text) -> llvm::Expected<uint64_t> {
    if (Text.contains('\0'))
      return failure("Darwin startup string contains a NUL byte");
    if (Text.size() >= Cursor)
      return failure("Darwin startup arguments exceed the stack");
    Cursor -= Text.size() + 1;
    std::copy(Text.begin(), Text.end(), Bytes.begin() + Cursor);
    return Base + Cursor;
  };
  auto Executable = PushString(("executable_path=" + ExecutableName).str());
  if (!Executable)
    return Executable.takeError();
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
    return failure("Darwin startup vector exceeds the stack");
  Cursor = (Cursor - Words.size() * 8) & ~uint64_t(15);
  const uint64_t SP = Base + Cursor;
  for (const auto Word : Words) {
    llvm::support::endian::write64le(Bytes.data() + Cursor, Word);
    Cursor += 8;
  }
  if (auto E = Memory.write(Base, Bytes))
    return std::move(E);
  return InitialStack{
      SP, Arguments.size(), SP + 8, SP + 8 * (Arguments.size() + 2),
      SP + 8 * (Arguments.size() + Options.Environment.size() + 3)};
}
} // namespace neverd::emulation::darwin_model
