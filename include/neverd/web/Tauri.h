//===- Tauri.h - Explicit native PHF asset evidence --------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable assets selected by a bounded native initializer projection.
/// Native loading/lifting is owned by the SDK and pipeline, not this reader.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"
#include "neverd/web/Limits.h"

#include <array>

namespace neverd::web {
inline constexpr std::string_view TauriProfile =
    "tauri-phf-0.10-macho-x64-five-store-brotli-v1";

/// SDK projection of one immutable, hash-bound native image. Never accept
/// this descriptor from transport JSON. It states local instruction effects,
/// not execution, object identity, compiler version or asset-server binding.
struct TauriInitializer {
  struct Field {
    uint64_t Instruction = 0, Value = 0;
    bool ExactAddress = false;
  };
  struct Mapping {
    uint64_t Address = 0, Size = 0, Offset = 0, StoredSize = 0;
    bool Readable = false;
  };
  std::string InputHash;
  uint64_t Start = 0, End = 0, Function = 0, Offset = 0;
  uint64_t Instructions = 0, NativeSteps = 0;
  std::array<Field, 5> Fields;
  std::vector<Mapping> Mappings;
};

struct TauriLimits {
  uint32_t Members = 4096;
  uint64_t StoredBytes = 32ULL * 1024 * 1024;
  uint64_t DecodedBytes = 32ULL * 1024 * 1024;
  uint64_t MemberDecodedBytes = 16ULL * 1024 * 1024;
  uint64_t PrivateBytes = 2ULL * 1024 * 1024;
  uint64_t Steps = 1000000;
  uint64_t CodecCalls = 65536, CodecMillis = 5000;
  uint64_t CodecMemory = 64ULL * 1024 * 1024;
};

struct TauriMember {
  std::string ID, Path, Kind, BlobHash, StoredHash;
  uint64_t RowOffset = 0, NameOffset = 0, Offset = 0, DecodedSize = 0;
  Blob Stored, Content;
};

struct TauriExtraction {
  std::string ID, ArtifactID, BlobHash, Profile;
  TauriInitializer Initializer;
  uint64_t DisplacementsOffset = 0, EntriesOffset = 0;
  uint64_t Steps = 0, StoredBytes = 0, DecodedBytes = 0, PrivateBytes = 0;
  uint64_t CodecCalls = 0;
  std::vector<TauriMember> Members;
};

bool tauriAvailable();
/// All five field roles, mappings, PHF lookups, private paths, compressed
/// framing and budgets must validate before publication. Limits may only
/// lower the published bounds. Codec selection is explicit, never guessed.
TauriExtraction extractTauri(const Artifact &Input,
                             const TauriInitializer &Initializer,
                             std::string_view Profile, TauriLimits Limits = {});
Snapshot tauriNamespace(const TauriExtraction &Extraction);
} // namespace neverd::web
