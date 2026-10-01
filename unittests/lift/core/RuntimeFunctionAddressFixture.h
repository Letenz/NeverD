#ifndef NEVERD_TEST_RUNTIMEFUNCTIONADDRESSFIXTURE_H
#define NEVERD_TEST_RUNTIMEFUNCTIONADDRESSFIXTURE_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/RuntimeFunctionAddress.h"

namespace runtime_function_address_test {
constexpr neverd::va_t Slot = 0x2000;
inline neverd::BinaryImage image(neverd::Arch Architecture) {
  using namespace neverd;
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Segment Data;
  Data.VA = Slot;
  Data.Size = Data.FileSz = 8;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(8);
  Image.Segments.push_back(std::move(Data));
  Section Got;
  Got.Name = "__got";
  Got.VA = Slot;
  Got.Size = Got.FileSz = 8;
  Got.Flags = SegmentFlags::Readable;
  Got.Type = llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS;
  Image.Sections.push_back(Got);
  Image.ImportPtrSlots[Slot] = "_swift_release";
  Image.DyldBindSlots[Slot] = {"_swift_release", 0,
                               "/usr/lib/swift/libswiftCore.dylib", false};
  return Image;
}
} // namespace runtime_function_address_test
#endif
