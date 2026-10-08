//===- RawLoader.h - Binary files no header describes -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The load dialog's "Binary file": a file's bytes read as code of the
// processor the user names, mapped at the address the user names.  Nothing
// in such a file states a calling convention, so its functions are browsed,
// not decompiled.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_RAW_RAWLOADER_H
#define NEVERD_LOADER_RAW_RAWLOADER_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"

#include <optional>
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
};

/// The processor named \p Name in RawLoader.def ("x86", "x86_64", "arm",
/// "thumb" or "aarch64"), or none.
std::optional<std::pair<Arch, InstructionMode>>
parseRawProcessor(llvm::StringRef Name);
/// The name RawLoader.def gives a processor, empty when a binary file cannot
/// be read as it.
llvm::StringRef getRawProcessorName(Arch TheArch, InstructionMode Mode);

class RawLoader final : public Loader {
public:
  explicit RawLoader(RawLoadOptions Options) : Options(std::move(Options)) {}
  llvm::Expected<BinaryImage> load(const std::filesystem::path &Path) override;

private:
  RawLoadOptions Options;
};

} // namespace neverd

#endif // NEVERD_LOADER_RAW_RAWLOADER_H
