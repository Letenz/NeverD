//===- Packer.h - What one protector knows about its images -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_CORE_PACKER_H
#define NEVERD_UNPACK_CORE_PACKER_H

#include "Capture.h"
#include "Image.h"

namespace neverd::unpack {
/// Static knowledge of one protector. A module answers only for the
/// containers and instruction sets whose stubs it knows and contributes
/// nothing for any other image; it never executes and never predicts.
class Packer {
public:
  virtual ~Packer();
  virtual PackerKind kind() const = 0;
  /// Append each independent observation of this protector in \p Image.
  virtual void collectEvidence(const InputImage &Image,
                               std::vector<PackerEvidence> &Evidence) const = 0;
  /// Observations that must be present before the protector is named. One
  /// alone, such as a section name, can be imitated or erased.
  virtual unsigned requiredEvidence() const = 0;
  /// The address the stub hands control to when it has finished, where the
  /// stub's own operands name it. Absent unless it is unambiguous.
  virtual std::optional<uint64_t> declaredEntry(const InputImage &Image) const;
  /// Metadata of the recovered program that the stub replaced with its own.
  virtual void planRebuild(const InputImage &Image, const Capture &Observed,
                           RebuildPlan &Plan) const;
};

/// Every protector module, in identification order.
llvm::ArrayRef<const Packer *> packers();
/// The module of \p Kind; null for an unidentified input.
const Packer *packerOf(PackerKind Kind);
/// Name the protector of \p Image from the evidence its modules find.
PackerIdentification identify(const InputImage &Image);
} // namespace neverd::unpack
#endif
