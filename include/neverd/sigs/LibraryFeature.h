//===- LibraryFeature.h - Evidence-gated library descriptions -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_LIBRARYFEATURE_H
#define NEVERD_SIGS_LIBRARYFEATURE_H

#include "neverd/Common.h"
#include "neverd/sigs/Signature.h"

#include "llvm/Support/Error.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace neverd {
struct BinaryImage;

namespace sigs {

/// Finite, typed expression vocabulary. A rule describes original operations;
/// it never authorizes inserting a call or replacing their semantics.
enum class LibraryFeatureOp : uint8_t {
#define NEVERD_LIBRARY_FEATURE_OP(Name, Text, Arity) Name,
#include "neverd/sigs/LibraryFeatureOps.def"
};

struct LibraryFeatureNode {
  LibraryFeatureOp Op = LibraryFeatureOp::Input;
  uint8_t Bits = 0;
  std::vector<unsigned> Args;
  uint64_t Constant = 0;
  int64_t Offset = 0;
  bool Receiver = false;
  unsigned Argument = 0;
};

enum class LibraryFeatureScope : uint8_t {
  WholeFunction = 1,
  InlineExpression = 2,
  InlineRegion = 4,
  /// Runtime annotation of an existing call; never admitted in a rule pack.
  CallSite = 8,
};

enum class LibraryFeatureIdentity : uint8_t {
  Receiver,
  CallRelationship,
  ByteSignature,
};

struct LibraryFeatureLayout {
  std::string ReceiverType;
  uint32_t ObjectBytes = 0;
  uint16_t ElementBits = 0;
  /// Only these exact derived types may project to the declaring receiver.
  std::map<std::string, int64_t> DerivedReceivers;
};

struct LibraryFeatureRule {
  enum class Kind : uint8_t { Expression, ComLifetime, BytePattern };
  enum class ComPolicy : uint8_t {
    ZeroInitialize,
    StoreBeforeAddref,
    AddrefPublishRelease,
    ClearBeforeRelease,
    ReleaseWithoutClear,
  };

  std::string Id;
  unsigned Revision = 0;
  std::string Family;
  std::string Operation;
  std::string Layout;
  std::string ReceiverType;
  LibraryFeatureIdentity Identity = LibraryFeatureIdentity::Receiver;
  /// Compared only with an independently stated symbol in the target. Probe
  /// wrapper names never establish a standalone library-method identity.
  std::vector<std::string> WholeFunctionSymbols;
  Kind PatternKind = Kind::Expression;
  uint8_t Scopes = 0;
  std::vector<LibraryFeatureNode> Nodes;
  unsigned Result = 0;
  ComPolicy Policy = ComPolicy::ZeroInitialize;
  /// Unmodified text and linkage identity for the existing signature engine.
  std::string PatternText;
  std::string LinkageName;
  std::string PatternSHA256;

  bool supports(LibraryFeatureScope Scope) const {
    return Scopes & static_cast<uint8_t>(Scope);
  }
};

struct LibraryFeaturePack {
  std::string Id;
  std::string SHA256;
  std::string ProfileSHA256;
  std::string EvidenceSHA256;
  std::string Implementation;
  std::string SourceOrigin;
  std::string SourceRevision;
  std::string SourceLicense;
  std::string CompilerVersion;
  Arch Architecture = Arch::Unknown;
  BinaryFormat Format = BinaryFormat::Unknown;
  std::string ABI;
  std::map<std::string, LibraryFeatureLayout> Layouts;
  std::vector<LibraryFeatureRule> Rules;

  bool accepts(Arch Arch, BinaryFormat Container, Bitness Bits) const {
    return Arch == Architecture && Container == Format &&
           Bits == Bitness::Bits64;
  }
  bool accepts(const BinaryImage &Image) const;
};

/// Load one v1 pack from <root>/features/rules. Referenced profiles, evidence
/// manifests and byte patterns are bounded, confined to that root and checked
/// against their recorded size and SHA-256. Publication is transactional.
/// Producer archives remain provenance; they are not target identity evidence.
llvm::Expected<LibraryFeaturePack>
readLibraryFeaturePack(const std::filesystem::path &Path);

} // namespace sigs
} // namespace neverd

#endif
