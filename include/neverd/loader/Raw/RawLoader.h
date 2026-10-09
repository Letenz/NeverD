//===- RawLoader.h - Binary files no header describes -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The load dialog's "Binary file": a file's bytes read as code of the
// processor the user names, mapped at the address the user names.  Nothing
// in such a file states the platform its code was built for; the user names
// it, or detection reads it from the code, and its conventions -- System V,
// Windows or Apple -- are the ones the image's abiFormat() selects.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_RAW_RAWLOADER_H
#define NEVERD_LOADER_RAW_RAWLOADER_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>
#include <utility>

namespace neverd {

/// How to read a binary file: Size of its bytes from Offset on (0 for the
/// rest of the file) as code of one processor, mapped at Base.
struct RawLoadOptions {
  Arch TheArch = Arch::Unknown;
  InstructionMode Mode = InstructionMode::Default;
  va_t Base = 0;
  uint64_t Offset = 0;
  uint64_t Size = 0;
  /// Where execution starts; Base when unset.
  std::optional<va_t> Entry;
  /// The platform whose conventions the code follows: ELF for System V and
  /// AAPCS, COFF for Windows, MachO for Apple.  None until the user names
  /// one or detection reads it from the code.
  std::optional<BinaryFormat> Platform;
  /// Whether detection chose Platform rather than the user.
  bool PlatformDetected = false;
  /// What detection read the platform from, for the user to check.
  std::string PlatformEvidence;
};

/// The processor named \p Name in RawLoader.def ("x86", "x86_64", "arm",
/// "thumb" or "aarch64"), or none.
std::optional<std::pair<Arch, InstructionMode>>
parseRawProcessor(llvm::StringRef Name);
/// The name RawLoader.def gives a processor, empty when a binary file cannot
/// be read as it.
llvm::StringRef getRawProcessorName(Arch TheArch, InstructionMode Mode);
/// The platform named \p Name in RawLoader.def ("sysv", "windows" or
/// "darwin"), as the format whose conventions it follows, or none.
std::optional<BinaryFormat> parseRawPlatform(llvm::StringRef Name);
/// The load option name RawLoader.def gives the platform whose conventions
/// \p Format images follow, empty for none.
llvm::StringRef getRawPlatformName(BinaryFormat Format);
/// The name the user reads for that platform ("System V", "Windows",
/// "Apple"), empty for none.
llvm::StringRef getRawPlatformText(BinaryFormat Format);

class RawLoader final : public Loader {
public:
  explicit RawLoader(RawLoadOptions Options) : Options(std::move(Options)) {}
  llvm::Expected<BinaryImage> load(const std::filesystem::path &Path) override;

private:
  RawLoadOptions Options;
};

} // namespace neverd

#endif // NEVERD_LOADER_RAW_RAWLOADER_H
