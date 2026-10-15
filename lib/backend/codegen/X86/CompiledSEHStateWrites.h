//===- CompiledSEHStateWrites.h - SEH receipt checks -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserves callback state-write identity across compiler layout iterations
/// and closes the final instruction extents against allocated code sections.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_X86_COMPILEDSEHSTATEWRITES_H
#define NEVERD_BACKEND_CODEGEN_X86_COMPILEDSEHSTATEWRITES_H

#include "neverd/backend/codegen/BinaryRewriter.h"

#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES

namespace neverd {

struct X86SEHFrameEmissionShape {
  std::string SourceFunction;
  bool IsPrivate = false;
  int32_t RegistrationOffset = 0;
  uint32_t RegistrationSize = 0;
  uint32_t StateOffset = 0;
  llvm::mc_rewrite::RewriteWinEHSemanticEncoding Encoding =
      llvm::mc_rewrite::RewriteWinEHSemanticEncoding::X86SEH3;

  friend bool operator==(const X86SEHFrameEmissionShape &,
                         const X86SEHFrameEmissionShape &) = default;
};

struct X86SEHStateWriteEmissionShape {
  std::string SourceFunction;
  bool IsPrivate = false;
  uint64_t WriteID = 0;
  X86SEHFrameEmissionShape ParentFrame;
  std::optional<llvm::mc_rewrite::RewriteWinEHSemanticToken> Semantic;
  int32_t GeneratedState = -1;
  uint8_t AddressRegister = 0;
  uint64_t InstructionSize = 0;

  friend bool operator==(const X86SEHStateWriteEmissionShape &,
                         const X86SEHStateWriteEmissionShape &) = default;
};

/// Source identities and ordered writes, independent of MC symbol spelling
/// and section rebasing. Preceding branch relaxation may move a store within
/// its callback, but cannot change its fixed frame-relative MOV encoding.
struct X86SEHStateEmissionShape {
  std::vector<X86SEHFrameEmissionShape> Frames;
  std::vector<X86SEHStateWriteEmissionShape> Writes;

  friend bool operator==(const X86SEHStateEmissionShape &,
                         const X86SEHStateEmissionShape &) = default;
};

/// Validate portable receipts before collecting a layout-independent shape.
/// Provisional sections may overlap until the placement loop converges.
std::optional<X86SEHStateEmissionShape>
collectX86SEHStateEmissionShape(const llvm::mc_rewrite::RewriteResult &Result);

/// Revalidate final source ownership, global range disjointness and exact
/// allocated code storage. This is also usable by the native installer after
/// receiving a CompiledImage from an independent producer.
bool validateCompiledX86SEHStateWrites(const CompiledImage &Image);

} // namespace neverd

#endif
#endif
