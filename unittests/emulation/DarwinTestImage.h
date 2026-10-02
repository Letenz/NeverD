//===- DarwinTestImage.h - Independent tiny Mach-O test writer ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_DARWINTESTIMAGE_H
#define NEVERD_UNITTESTS_EMULATION_DARWINTESTIMAGE_H
#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace neverd::emulation::darwin_test {
/// Two original instruction sequences read argc from the direct-entry stack
/// and exit with that value. The writer does not use the production parser.
struct Image {
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(16384, 0);
  size_t CommandsEnd = 32, PageZero = 0, Text = 0, Platform = 0, Thread = 0;
  void u32(size_t At, uint32_t Value) {
    llvm::support::endian::write32le(Bytes.data() + At, Value);
  }
  void u64(size_t At, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + At, Value);
  }
  size_t command(uint32_t Kind, uint32_t Size) {
    const auto At = CommandsEnd;
    CommandsEnd += Size;
    assert(CommandsEnd < 4096);
    u32(At, Kind);
    u32(At + 4, Size);
    u32(16, llvm::support::endian::read32le(Bytes.data() + 16) + 1);
    u32(20, CommandsEnd - 32);
    return At;
  }
  explicit Image(bool X64 = false, uint32_t OS = llvm::MachO::PLATFORM_MACOS) {
    using namespace llvm::MachO;
    u32(0, MH_MAGIC_64);
    u32(4, X64 ? CPU_TYPE_X86_64 : CPU_TYPE_ARM64);
    u32(8, X64 ? 3 : 0);
    u32(12, MH_EXECUTE);
    PageZero = command(LC_SEGMENT_64, 72);
    std::copy_n("__PAGEZERO", 10, Bytes.data() + PageZero + 8);
    u64(PageZero + 32, 0x100000000ULL);
    Text = command(LC_SEGMENT_64, 72);
    std::copy_n("__TEXT", 6, Bytes.data() + Text + 8);
    u64(Text + 24, 0x100000000ULL);
    u64(Text + 32, Bytes.size());
    u64(Text + 48, Bytes.size());
    u32(Text + 56, 5);
    u32(Text + 60, 5);
    Platform = command(LC_BUILD_VERSION, 24);
    u32(Platform + 8, OS);
    u32(Platform + 12, 0x000e0000);
    u32(Platform + 16, 0x000e0000);
    Thread = command(LC_UNIXTHREAD, X64 ? 184 : 288);
    u32(Thread + 8, X64 ? 4 : 6);
    u32(Thread + 12, X64 ? 42 : 68);
    u64(Thread + 16 + (X64 ? 128 : 256), 0x100001000ULL);
    if (X64) {
      // mov (%rsp),%rdi; mov $0x2000001,%eax; syscall
      const uint8_t Code[] = {0x48, 0x8b, 0x3c, 0x24, 0xb8, 1,
                              0,    0,    2,    0x0f, 5};
      std::copy(std::begin(Code), std::end(Code), Bytes.begin() + 4096);
    } else {
      // ldr x0,[sp]; mov x16,#1; svc #0x80
      u32(4096, 0xf94003e0);
      u32(4100, 0xd2800030);
      u32(4104, 0xd4001001);
    }
  }
};
struct TemporaryImage {
  llvm::SmallString<256> Path;
  explicit TemporaryImage(const Image &Image) {
    int FD;
    llvm::cantFail(llvm::errorCodeToError(llvm::sys::fs::createTemporaryFile(
        "neverd-darwin", "macho", FD, Path)));
    llvm::raw_fd_ostream Out(FD, true);
    Out.write(reinterpret_cast<const char *>(Image.Bytes.data()),
              Image.Bytes.size());
  }
  ~TemporaryImage() { llvm::sys::fs::remove(Path); }
  std::filesystem::path path() const { return Path.str().str(); }
};
} // namespace neverd::emulation::darwin_test
#endif
