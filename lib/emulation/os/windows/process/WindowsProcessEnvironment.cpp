//===- WindowsProcessEnvironment.cpp - PE64 initial thread state --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcess.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
llvm::Expected<std::u16string> utf16(llvm::StringRef Input) {
  llvm::SmallVector<llvm::UTF16> Units;
  if (Input.contains('\0') || Input.size() > StringCapacity ||
      !llvm::convertUTF8ToUTF16String(Input, Units) ||
      Units.size() >= MaxStringUnits)
    return failure(text::Strings);
  return std::u16string(Units.begin(), Units.end());
}
std::u16string quote(const std::u16string &Input) {
  // Encode argv using the documented Microsoft CRT backslash/quote rules.
  // Quote every argument so empty strings and trailing backslashes survive.
  std::u16string Out(1, u'"');
  size_t Backslashes = 0;
  for (char16_t C : Input) {
    if (C == u'\\') {
      ++Backslashes;
      continue;
    }
    Out.append(Backslashes * (C == u'"' ? 2 : 1), u'\\');
    Backslashes = 0;
    if (C == u'"')
      Out += u'\\';
    Out += C;
  }
  Out.append(Backslashes * 2, u'\\');
  Out += u'"';
  return Out;
}
} // namespace

llvm::Expected<Environment> prepareEnvironment(AddressSpace &Memory,
                                               const Image &Image,
                                               const ProcessOptions &Options,
                                               llvm::StringRef ImageName) {
  auto Name = utf16(ImageName);
  if (!Name)
    return Name.takeError();
  std::u16string Command;
  auto Arguments = Options.Arguments;
  if (Arguments.empty())
    Arguments.push_back(ImageName.str());
  if (Arguments.size() > MaxStringUnits ||
      Options.Environment.size() > MaxStringUnits)
    return failure(text::Strings);
  for (const auto &Arg : Arguments) {
    auto Text = utf16(Arg);
    if (!Text)
      return Text.takeError();
    if (Command.empty()) {
      // CRT treats argv[0] separately: backslashes do not escape its quote.
      if (Text->find(u'"') != std::u16string::npos)
        return failure(text::Strings);
      Command = std::u16string(1, u'"') + *Text + u'"';
    } else {
      Command += u' ';
      Command += quote(*Text);
    }
    if (Command.size() >= MaxStringUnits)
      return failure(text::Strings);
  }
  std::map<std::string, std::u16string> Sorted;
  size_t EnvironmentUnits = 1;
  for (const auto &Item : Options.Environment) {
    const auto Equal = Item.find('=');
    if (Equal == std::string::npos || !Equal)
      return failure(text::Strings);
    const llvm::StringRef Key(Item.data(), Equal);
    // Unicode values are supported. ASCII names avoid inventing an incomplete
    // Windows Unicode case-folding table for environment identity.
    if (!llvm::all_of(Key, [](char C) { return llvm::isPrint(C); }))
      return failure(text::Strings);
    auto Text = utf16(Item);
    if (!Text)
      return Text.takeError();
    EnvironmentUnits += Text->size() + 1;
    if (EnvironmentUnits > MaxStringUnits ||
        !Sorted.emplace(Key.lower(), std::move(*Text)).second)
      return failure(text::Strings);
  }
  std::u16string EnvironmentBlock;
  for (const auto &[Key, Text] : Sorted) {
    EnvironmentBlock += Text;
    EnvironmentBlock += u'\0';
  }
  if (EnvironmentBlock.empty())
    EnvironmentBlock += u'\0';
  EnvironmentBlock += u'\0';
  if (auto E =
          Memory.map(TEB, EnvironmentEnd - TEB, Read | Write | UserAccessible))
    return std::move(E);
  uint64_t Cursor = StringBase;
  auto Store = [&](const std::u16string &Text) -> llvm::Expected<uint64_t> {
    const uint64_t Size = (Text.size() + 1) * WideSize;
    if (Size > EnvironmentEnd - Cursor)
      return failure(text::Strings);
    std::vector<uint8_t> Bytes(Size);
    for (size_t I = 0; I < Text.size(); ++I)
      llvm::support::endian::write16le(Bytes.data() + I * WideSize, Text[I]);
    const uint64_t Address = Cursor;
    if (auto E = Memory.write(Address, Bytes))
      return std::move(E);
    Cursor += Size;
    return Address;
  };
  auto ImageAddress = Store(*Name);
  if (!ImageAddress)
    return ImageAddress.takeError();
  auto CommandAddress = Store(Command);
  if (!CommandAddress)
    return CommandAddress.takeError();
  auto EnvAddress = Store(EnvironmentBlock);
  if (!EnvAddress)
    return EnvAddress.takeError();
  auto Unicode = [&](uint64_t Address, uint64_t Buffer,
                     size_t Length) -> llvm::Error {
    if (auto E = Memory.writeInteger(Address + UnicodeLength, Length * WideSize,
                                     WideSize))
      return E;
    if (auto E = Memory.writeInteger(Address + UnicodeMaximumLength,
                                     (Length + 1) * WideSize, WideSize))
      return E;
    return Memory.writeInteger(Address + UnicodeBuffer, Buffer, PointerSize);
  };
  struct Field {
    uint64_t Address, Value;
    unsigned Size = PointerSize;
  };
  const Field Fields[] = {
      {TEB + TebStackBase, StackTop},
      {TEB + TebStackLimit, StackTop - Options.StackSize},
      {TEB + TebDeallocationStack, StackTop - Options.StackSize},
      {TEB + TebSelf, TEB},
      {TEB + TebProcessID, ProcessID},
      {TEB + TebThreadID, ThreadID},
      {TEB + TebPEB, PEB},
      {TEB + TebTLSVector, TLSVector},
      {PEB + PebImageBase, Image.Base},
      {PEB + PebParameters, Parameters},
      {PEB + PebHeap, HeapHandle},
      {PEB + PebLdr, Ldr},
      {Parameters + ParamsMaximumLength, ParameterSize, DWordSize},
      {Parameters + ParamsLength, ParameterSize, DWordSize},
      {Parameters + ParamsFlags, ParamsNormalized, DWordSize},
      {Parameters + ParamsStdInput, StandardInput},
      {Parameters + ParamsStdOutput, StandardOutput},
      {Parameters + ParamsStdError, StandardError},
      {Parameters + ParamsEnvironment, *EnvAddress},
      {Ldr, LdrSize, DWordSize},
      {Ldr + LdrInitialized, 1, 1},
      {ModuleEntry + ModuleBase, Image.Base},
      {ModuleEntry + ModuleEntryPoint, Image.Entry},
      {ModuleEntry + ModuleImageSize, Image.Size, DWordSize}};
  for (const auto &F : Fields)
    if (auto E = Memory.writeInteger(F.Address, F.Value, F.Size))
      return std::move(E);
  // Only the executable is mapped as a PE module. Named system API models are
  // not disguised as installed system DLLs or discoverable PE exports.
  for (auto [Head, Node] :
       {std::pair{Ldr + LdrLoadList, ModuleEntry},
        std::pair{Ldr + LdrMemoryList, ModuleEntry + ModuleMemoryLink}}) {
    for (uint64_t Offset : {uint64_t(0), PointerSize}) {
      if (auto E = Memory.writeInteger(Head + Offset, Node, PointerSize))
        return std::move(E);
      if (auto E = Memory.writeInteger(Node + Offset, Head, PointerSize))
        return std::move(E);
    }
  }
  for (uint64_t Offset : {uint64_t(0), PointerSize})
    if (auto E = Memory.writeInteger(Ldr + LdrInitList + Offset,
                                     Ldr + LdrInitList, PointerSize))
      return std::move(E);
  for (uint64_t Address :
       {Parameters + ParamsImagePath, ModuleEntry + ModuleFullName,
        ModuleEntry + ModuleBaseName})
    if (auto E = Unicode(Address, *ImageAddress, Name->size()))
      return std::move(E);
  if (auto E = Unicode(Parameters + ParamsCommandLine, *CommandAddress,
                       Command.size()))
    return std::move(E);
  if (Image.TLSIndex) {
    if (auto E = Memory.writeInteger(Image.TLSIndex, 0, DWordSize))
      return std::move(E);
    if (auto E = Memory.writeInteger(TLSVector, TLSData, PointerSize))
      return std::move(E);
    if (!Image.TLSBytes.empty())
      if (auto E = Memory.write(TLSData, Image.TLSBytes))
        return std::move(E);
  }
  return Environment{*CommandAddress, std::move(*Name)};
}
} // namespace neverd::emulation::windows_process
