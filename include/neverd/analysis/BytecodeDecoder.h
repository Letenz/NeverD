//===- BytecodeDecoder.h - Externally described bytecode --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BYTECODEDECODER_H
#define NEVERD_ANALYSIS_BYTECODEDECODER_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Endian.h"

#include <array>
#include <memory>

namespace neverd::analysis {

/// An unsigned instruction field followed by an optional exact lookup. The
/// value is (lookup((read >> Shift) & Mask) * Scale + Addend + optional PC),
/// modulo 2^ValueBits. Bytes == 0 supplies zero instead of reading a field.
/// Field byte order comes from the profile. Lookup failure is a decode error.
/// This describes encodings only; arithmetic semantics remain ordinary LowIR.
struct BytecodeValue {
  uint16_t Offset = 0;
  uint8_t Bytes = 0;
  uint8_t Shift = 0;
  uint64_t Mask = UINT64_MAX;
  uint64_t Scale = 1;
  uint64_t Addend = 0;
  uint8_t ValueBits = 64;
  bool PCRelative = false;
  std::vector<uint64_t> Lookup;
};

struct BytecodeOperand {
  VnodeSpace Space = VnodeSpace::CONST;
  uint16_t Size = 0;
  BytecodeValue Value;
};

struct BytecodeOperation {
  NdOp Opcode = NdOp::NOP;
  BytecodeOperand Output;
  std::vector<BytecodeOperand> Inputs;
};

struct BytecodeMatch {
  uint16_t Offset = 0;
  uint8_t Mask = 0xff;
  uint8_t Value = 0;
};

struct BytecodeEncoding {
  uint16_t Size = 0;
  std::vector<BytecodeMatch> Match;
  std::vector<BytecodeOperation> Operations;
};

/// Supplied by the caller, independently of proprietary container formats,
/// identifiers, opcode assignments and host-service registries. A profile is
/// a semantic specification, not evidence of equivalence to a native binary.
/// Register operands name byte ranges in a virtual register bank; a narrow
/// write changes exactly that range and does not implicitly clear high bytes.
/// Temporaries are instruction-local typed values: a read needs an exact
/// preceding definition at the same offset and width. Overlapping writes
/// invalidate older temporary views; use SUBBYTES/CONCAT for explicit slices.
struct BytecodeProfile {
  uint32_t RegisterBytes = 0;
  uint32_t TemporaryBytes = 4096;
  llvm::endianness ByteOrder = llvm::endianness::little;
  std::vector<BytecodeEncoding> Encodings;
};

struct BytecodeInstruction {
  uint16_t Size = 0;
  uint32_t Encoding = 0;
  std::vector<LowOp> Operations;
};

struct BytecodeFunction {
  LowFunc Function;
  std::set<va_t> DirectCalls;
  uint64_t DecodedBytes = 0;
};

struct BytecodeDecodeLimits {
  uint32_t MaxInstructions = 100000;
  uint64_t MaxOperations = 1000000;
};

/// Read version 1 of the external profile format. No built-in dialect or
/// private opcode table is consulted. Unknown keys and malformed fields fail.
llvm::Expected<BytecodeProfile> readBytecodeProfile(llvm::StringRef JSON);

class BytecodeDecoder {
public:
  static llvm::Expected<std::unique_ptr<BytecodeDecoder>>
  create(BytecodeProfile Profile);

  /// Address is the logical bytecode PC; Bytes begins at that instruction.
  /// Unknown, ambiguous, truncated and malformed instructions return errors.
  llvm::Expected<BytecodeInstruction> decode(llvm::ArrayRef<uint8_t> Bytes,
                                             va_t Address) const;

  /// Recover the CFG within exactly [Entry, End), inside Code at Base.
  /// Calls are separate function roots, never intra-function CFG edges.
  /// Straight-line instructions share blocks; control targets and call
  /// continuations are leaders. Each operation retains its instruction PC.
  /// Unresolved indirect branches and overlapping instruction ranges fail.
  /// Only reachable instructions are claimed; DecodedBytes is coverage, not
  /// a claim that unvisited bytes are padding. No native provenance is minted.
  llvm::Expected<BytecodeFunction>
  function(llvm::ArrayRef<uint8_t> Code, va_t Base, va_t Entry, va_t End,
           llvm::StringRef Name, const BytecodeDecodeLimits &Limits = {}) const;

  const BytecodeProfile &profile() const { return Profile; }

private:
  BytecodeProfile Profile;
  std::array<std::vector<uint32_t>, 256> FirstByte;
  explicit BytecodeDecoder(BytecodeProfile Profile);
};

} // namespace neverd::analysis

#endif
