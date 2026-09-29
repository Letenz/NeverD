//===- LinuxProcessStack.cpp - Explicit ELF process initial stack --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxProcess.h"

#include "neverd/loader/BinaryImageModel.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
llvm::Expected<uint64_t> prepareStack(GuestMemory &Memory,
                                      const BinaryImage &Image,
                                      const ProcessLayout &Layout,
                                      const ProcessOptions &Options,
                                      llvm::StringRef ExecutableName) {
  if (!Image.InputFileSHA256 || Options.StackSize > StackTop)
    return failure(ProgramHeaders);
  const uint64_t Base = StackTop - Options.StackSize;
  std::vector<uint8_t> Bytes(Options.StackSize, 0);
  uint64_t Cursor = Bytes.size();
  auto PushString = [&](llvm::StringRef Text) -> llvm::Expected<uint64_t> {
    if (Text.contains('\0'))
      return failure(String);
    if (Text.size() >= Cursor)
      return failure(Stack);
    Cursor -= Text.size() + 1;
    std::copy(Text.begin(), Text.end(), Bytes.begin() + Cursor);
    return Base + Cursor;
  };
  auto ExecutableAddress = PushString(ExecutableName);
  if (!ExecutableAddress)
    return ExecutableAddress.takeError();
  if (RandomSize > Cursor)
    return failure(Stack);
  Cursor -= RandomSize;
  const uint64_t RandomAddress = Base + Cursor;
  // Reproducible model entropy derived from the input identity, not host
  // randomness. The profile reports this policy; it is not a cryptographic OS.
  std::copy_n(Image.InputFileSHA256->begin(), RandomSize,
              Bytes.begin() + Cursor);
  const auto Arguments = Options.Arguments.empty()
                             ? std::vector<std::string>{ExecutableName.str()}
                             : Options.Arguments;
  std::vector<uint64_t> Words;
  Words.push_back(Arguments.size());
  for (const auto *Strings : {&Arguments, &Options.Environment}) {
    for (const auto &Text : *Strings) {
      auto Address = PushString(Text);
      if (!Address)
        return Address.takeError();
      Words.push_back(*Address);
    }
    Words.push_back(0);
  }
#define NEVERD_LINUX_AUX(Name, Tag, Value)                                     \
  Words.push_back(Tag);                                                        \
  Words.push_back(Value);
#include "LinuxValues.def"
#undef NEVERD_LINUX_AUX
  if (Words.size() > Cursor / Layout.Calls.info().WordSize)
    return failure(Stack);
  Cursor -= Words.size() * Layout.Calls.info().WordSize;
  Cursor &= ~(Layout.Calls.info().StackAlignment - 1);
  for (uint64_t Word : Words) {
    llvm::support::endian::write64le(Bytes.data() + Cursor, Word);
    Cursor += Layout.Calls.info().WordSize;
  }
  const uint64_t SP =
      Base + Cursor - Words.size() * Layout.Calls.info().WordSize;
  if (auto E = Memory.write(Base, Bytes))
    return std::move(E);
  return SP;
}
} // namespace neverd::emulation::linux_model
