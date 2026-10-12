//===- SourcePatchLegacyMachOFixture.cpp - Compiler-owned legacy fixture ===//
#include "neverd/backend/codegen/BinaryRewriter.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/TargetParser/Triple.h"

#include <cstring>
#include <fstream>
#include <string>
using namespace neverd;
namespace {
void put(std::string &Bytes, size_t At, uint64_t Value, unsigned Width) {
  for (unsigned I = 0; I < Width; ++I)
    Bytes.at(At + I) = char(Value >> (8 * I));
}
// Modern ld64 cannot link 32-bit Mach-O. The fixture container has a
// compiler-produced text section, relocated CFI, exact nlist and __LINKEDIT.
// It is a structural/CPU fixture, not a legacy OS runtime certification.
std::string legacyMachOImage(Arch Architecture, llvm::Module &Module) {
  constexpr uint32_t Base = 0x400000, Code = 0x1000, EH = 0x2000;
  const std::string Triple = Module.getTargetTriple().str();
  auto Compiled = compileImageForPatchWithFixedSectionVAs(
      Module, Architecture, BinaryFormat::MachO, Base + Code,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      [&](llvm::StringRef Name) -> std::optional<uint64_t> {
        return Name == "__eh_frame" ? std::optional<uint64_t>(Base + EH)
                                    : std::nullopt;
      },
      0, Triple);
  if (!Compiled.Success || Compiled.Bytes.empty() ||
      !Compiled.Unresolved.empty())
    return {};
  const CompiledSection *Text = nullptr, *Frame = nullptr;
  for (const auto &Section : Compiled.Sections) {
    if (Section.Name == "__text")
      Text = &Section;
    if (Section.Name == "__eh_frame")
      Frame = &Section;
  }
  if (!Text || (Frame && (Frame->IsInImage || Frame->Size >= 0x800)) ||
      Compiled.Bytes.size() >= EH - Code)
    return {};
  std::string B(0x5000, '\0');
  using namespace llvm::MachO;
  constexpr size_t H = sizeof(mach_header), S = sizeof(segment_command),
                   Sec = sizeof(section);
  put(B, 0, MH_MAGIC, 4);
  put(B, 4, Architecture == Arch::ARM ? CPU_TYPE_ARM : CPU_TYPE_X86, 4);
  const llvm::Triple Target(Triple);
  put(B, 8,
      Architecture == Arch::ARM
          ? uint32_t(Target.isWatchABI() ? CPU_SUBTYPE_ARM_V7K
                                         : CPU_SUBTYPE_ARM_V7)
          : uint32_t(CPU_SUBTYPE_I386_ALL),
      4);
  put(B, 12, MH_EXECUTE, 4);
  put(B, 16, 4, 4);
  put(B, 20,
      2 * S + 2 * Sec + sizeof(symtab_command) + sizeof(entry_point_command),
      4);
  put(B, H, LC_SEGMENT, 4);
  put(B, H + 4, S + 2 * Sec, 4);
  B.replace(H + 8, 6, "__TEXT");
  put(B, H + 24, Base, 4);
  put(B, H + 28, 0x4000, 4);
  put(B, H + 36, 0x4000, 4);
  put(B, H + 40, 5, 4);
  put(B, H + 44, 5, 4);
  put(B, H + 48, 2, 4);
  auto Section = [&](size_t At, const char *Name, uint32_t Offset,
                     uint32_t Size, uint32_t Flags) {
    B.replace(At, std::strlen(Name), Name);
    B.replace(At + 16, 6, "__TEXT");
    put(B, At + 32, Base + Offset, 4);
    put(B, At + 36, Size, 4);
    put(B, At + 40, Offset, 4);
    put(B, At + 44, 2, 4);
    put(B, At + 56, Flags, 4);
  };
  Section(H + S, "__text", Code, Text->Size,
          S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
  Section(H + S + Sec, "__eh_frame", EH, Frame ? Frame->Size : 4, S_REGULAR);
  const size_t L = H + S + 2 * Sec;
  put(B, L, LC_SEGMENT, 4);
  put(B, L + 4, S, 4);
  B.replace(L + 8, 10, "__LINKEDIT");
  put(B, L + 24, Base + 0x4000, 4);
  put(B, L + 28, 0x1000, 4);
  put(B, L + 32, 0x4000, 4);
  put(B, L + 36, 0x1000, 4);
  put(B, L + 40, 1, 4);
  put(B, L + 44, 1, 4);
  const size_t Symbols = L + S;
  std::vector<std::pair<std::string, uint64_t>> Definitions;
  for (const auto &Function : Module) {
    if (Function.isDeclaration())
      continue;
    const auto Name = "_" + Function.getName().str();
    auto Found = Compiled.SymbolAddrs.find(Name);
    if (Found == Compiled.SymbolAddrs.end())
      Found = Compiled.SymbolAddrs.find(Function.getName().str());
    if (Found == Compiled.SymbolAddrs.end())
      return {};
    Definitions.emplace_back(Name, Found->second & ~uint64_t(1));
  }
  const uint32_t StringOffset =
      llvm::alignTo(0x4000 + Definitions.size() * sizeof(nlist), 16);
  std::string Names(1, '\0');
  for (size_t I = 0; I < Definitions.size(); ++I) {
    const size_t Slot = 0x4000 + I * sizeof(nlist);
    put(B, Slot, Names.size(), 4);
    put(B, Slot + 4, uint32_t(N_EXT) | uint32_t(N_SECT), 1);
    put(B, Slot + 5, 1, 1);
    put(B, Slot + 6, Architecture == Arch::ARM ? N_ARM_THUMB_DEF : 0, 2);
    put(B, Slot + 8, Definitions[I].second, 4);
    Names += Definitions[I].first;
    Names += '\0';
  }
  if (StringOffset + Names.size() > B.size())
    return {};
  put(B, Symbols, LC_SYMTAB, 4);
  put(B, Symbols + 4, sizeof(symtab_command), 4);
  put(B, Symbols + 8, 0x4000, 4);
  put(B, Symbols + 12, Definitions.size(), 4);
  put(B, Symbols + 16, StringOffset, 4);
  put(B, Symbols + 20, Names.size(), 4);
  B.replace(StringOffset, Names.size(), Names);
  const size_t Entry = Symbols + sizeof(symtab_command);
  put(B, Entry, LC_MAIN, 4);
  put(B, Entry + 4, sizeof(entry_point_command), 4);
  put(B, Entry + 8, Code, 8);
  B.replace(Code, Compiled.Bytes.size(),
            reinterpret_cast<const char *>(Compiled.Bytes.data()),
            Compiled.Bytes.size());
  if (Frame)
    B.replace(EH, Frame->ExternalBytes.size(),
              reinterpret_cast<const char *>(Frame->ExternalBytes.data()),
              Frame->ExternalBytes.size());
  return B;
}

} // namespace
int main(int Count, char **Arguments) {
  if (Count != 3)
    return 2;
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseIRFile(Arguments[1], Diagnostic, Context);
  if (!Module) {
    Diagnostic.print(Arguments[0], llvm::errs());
    return 1;
  }
  const llvm::Triple Triple(Module->getTargetTriple());
  const Arch Architecture = Triple.getArch() == llvm::Triple::x86 ? Arch::X86
                            : (Triple.isARM() || Triple.isThumb())
                                ? Arch::ARM
                                : Arch::Unknown;
  if (Architecture == Arch::Unknown)
    return 2;
  auto Bytes = legacyMachOImage(Architecture, *Module);
  if (Bytes.empty())
    return 1;
  std::ofstream Output(Arguments[2], std::ios::binary);
  Output.write(Bytes.data(), Bytes.size());
  return Output ? 0 : 1;
}
