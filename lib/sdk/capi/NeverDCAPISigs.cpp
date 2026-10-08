//===- NeverDCAPISigs.cpp - C API: FLIRT signatures -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// FLIRT signature loading, matching, and result queries.
///
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/sigs/SignatureCache.h"
#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <future>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

void Session::refreshFunctionNames() {
  // C emitted so far names data as the user named it then.
  FunctionSources.clear();
  const auto Names = SigDB.buildNameMap();
  for (auto &F : Functions) {
    if (F.LinkageName.empty()) {
      F.LinkageName = F.Name;
      F.LinkageOrigin = F.Origin;
    }
    F.Name = F.LinkageName;
    F.Origin = F.LinkageOrigin;
    if (F.Origin < NameOrigin::Stated) {
      if (auto I = Names.find(F.Entry);
          I != Names.end() && !I->second.empty()) {
        F.Name = I->second;
        F.Origin = NameOrigin::Analysis;
      }
    }
    OriginalNames[F.Entry] = F.Name;
    if (auto Rename = Renames.find(F.Entry); Rename != Renames.end()) {
      F.Name = Rename->second;
      F.Origin = NameOrigin::User;
    }
  }
}

/// The addresses signatures are tried at: every function the session lists,
/// and, until the pipeline has run and published the functions it found,
/// every entry its detector finds too.  The session lists what the image's
/// own tables state, which in a stripped image is little of its code -- a
/// static musl program states 15 of its routines, and an optimized x86 PE
/// leaves hundreds of its runtime's routines out of every table.  An entry
/// the pipeline would later reject costs nothing here: a name needs a whole
/// signature to match there.
static std::vector<uint64_t> signatureEntries(Session &S) {
  std::vector<uint64_t> Entries;
  Entries.reserve(S.Functions.size());
  for (const auto &F : S.Functions)
    Entries.push_back(F.Entry);
  const bool Native = S.Img.Arch == Arch::X86 || S.Img.Arch == Arch::X64 ||
                      S.Img.Arch == Arch::ARM || S.Img.Arch == Arch::AArch64;
  if (Native && !(S.PipeRan && S.NativeFunctionsSynchronized)) {
    Decoder Dec;
    if (Dec.init(S.Img)) {
      FuncDetector Detector;
      for (const auto &Entry : Detector.detect(S.Img, Dec))
        Entries.push_back(Entry.first);
    }
    std::sort(Entries.begin(), Entries.end());
    Entries.erase(std::unique(Entries.begin(), Entries.end()), Entries.end());
  }
  return Entries;
}

/// signatureEntries for \p S, found while the caller loads signature files:
/// it reads only the image.
static std::future<std::vector<uint64_t>> findSignatureEntries(Session &S) {
  return std::async(std::launch::async, [&S] { return signatureEntries(S); });
}

/// Match the loaded signature set against \p S and report how many hits it
/// produced.
///
/// The personality pass runs after the general one because the general one
/// starts by clearing the match list, and running second is also what puts an
/// adopted name in front of the rename loop below.  It is safe to run
/// unconditionally: it only speaks for an address a frame installs as its
/// personality and the image itself cannot name, so a binary that carries its
/// own symbols is left exactly as it was.
static int matchLoadedSignatures(Session &S,
                                 std::future<std::vector<uint64_t>> Entries) {
  S.SigDB.apply(S.Img, Entries.get());
  S.SigDB.identifyPersonalityRoutines(S.Img);

  S.refreshFunctionNames();

  return static_cast<int>(S.SigDB.matches().size());
}

/// Load signature files through the user's signature cache, which the
/// environment may move or turn off; see neverd::sigs::SignatureCache.
static void useSignatureCache(Session &S) {
  S.SigDB.setCache(sigs::SignatureCache::fromEnvironment());
}

int neverd_apply_signatures(neverd_session_t Sess, const char *SigDir) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded || !SigDir)
    return -1;
  S->clearError();

  auto Entries = findSignatureEntries(*S);
  useSignatureCache(*S);
  auto Err = S->SigDB.loadDirectory(SigDir);
  if (Err) {
    S->setError(llvm::toString(std::move(Err)));
    return -1;
  }

  return matchLoadedSignatures(*S, std::move(Entries));
}

int neverd_apply_signature_file(neverd_session_t Sess, const char *SigPath) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded || !SigPath)
    return -1;
  S->clearError();

  auto Entries = findSignatureEntries(*S);
  useSignatureCache(*S);
  auto Err = S->SigDB.loadFile(SigPath);
  if (Err) {
    S->setError(llvm::toString(std::move(Err)));
    return -1;
  }

  return matchLoadedSignatures(*S, std::move(Entries));
}

int neverd_auto_apply_signatures(neverd_session_t Sess,
                                 const char *SigBaseDir) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded || !SigBaseDir)
    return -1;
  S->clearError();

  if (!sigs::SignatureDB::treeDirectory(S->Img))
    return 0;
  auto Entries = findSignatureEntries(*S);
  useSignatureCache(*S);
  auto Err = S->SigDB.loadForImage(S->Img, SigBaseDir);
  if (Err) {
    S->setError(llvm::toString(std::move(Err)));
    return -1;
  }

  return matchLoadedSignatures(*S, std::move(Entries));
}

// ===--------------------------------------------------------------------===//
// Signature generation utilities
// ===--------------------------------------------------------------------===//

unsigned short neverd_sig_compute_crc16(const unsigned char *Data, int Length) {
  if (!Data || Length <= 0)
    return 0;
  return sigs::SignatureMatcher::computeCRC16(Data,
                                              static_cast<size_t>(Length));
}

int neverd_sig_match_count(neverd_session_t Sess) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return 0;
  return static_cast<int>(S->SigDB.matches().size());
}

const char *neverd_sig_matches_json(neverd_session_t Sess) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return dupStr("[]");

  llvm::json::Array Arr;
  for (const auto &M : S->SigDB.matches()) {
    llvm::json::Object Obj;
    Obj["addr"] = vaHex(M.Address);
    Obj["name"] = jsonSafeText(M.Name);
    // The routine's other linkage names in its library, which the match
    // gives the same address.
    if (!M.Aliases.empty()) {
      llvm::json::Array Aliases;
      for (const std::string &Alias : M.Aliases)
        Aliases.push_back(jsonSafeText(Alias));
      Obj["aliases"] = std::move(Aliases);
    }
    Obj["library"] = jsonSafeText(M.LibraryName);
    Obj["func_len"] = static_cast<int64_t>(M.FuncLen);
    // Whether the image confirmed every routine the match's signature
    // branches to: such a match settles an address other matches dispute.
    Obj["confirmed"] = M.Confirmed;
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}
