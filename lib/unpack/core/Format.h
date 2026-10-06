//===- Format.h - One executable container format ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_CORE_FORMAT_H
#define NEVERD_UNPACK_CORE_FORMAT_H

#include "Capture.h"
#include "Image.h"

#include <memory>

namespace neverd::unpack {
/// How one container's files are validated and how observed memory becomes a
/// file of that container again. A format owns every byte layout rule of its
/// container; no other layer reads or writes container headers.
class Format {
public:
  virtual ~Format();
  virtual FormatKind kind() const = 0;
  /// \p File begins as this container. This selects the format and validates
  /// nothing.
  virtual bool recognizes(llvm::ArrayRef<uint8_t> File) const = 0;
  /// Decode and validate the facts unpacking relies on. Anything the module
  /// cannot represent exactly is an error rather than a guessed layout.
  virtual llvm::Expected<std::unique_ptr<InputImage>>
  read(llvm::ArrayRef<uint8_t> File) const = 0;
  /// Write \p Observed as a file whose entry is the recovered one. \p Image
  /// is one this format read.
  virtual llvm::Expected<RebuiltImage>
  rebuild(const InputImage &Image, const Capture &Observed,
          const RebuildPlan &Plan) const = 0;
};

/// Every format, in the order of Unpack.def.
llvm::ArrayRef<const Format *> formats();
/// The format whose container \p File is. The error names the supported ones.
llvm::Expected<const Format *> formatOf(llvm::ArrayRef<uint8_t> File);
/// Select the format of \p File and read it.
llvm::Expected<std::unique_ptr<InputImage>>
readImage(llvm::ArrayRef<uint8_t> File, const Format *&Selected);
} // namespace neverd::unpack
#endif
