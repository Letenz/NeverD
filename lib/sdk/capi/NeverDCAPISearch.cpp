//===- NeverDCAPISearch.cpp - C API: search and binary diff ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Byte and string search plus function and decompilation diff queries.
///
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/sdk/Limits.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <type_traits>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

// ===--------------------------------------------------------------------===//
// Search
// ===--------------------------------------------------------------------===//

namespace {
// Both the legacy queries and the paged query share segment boundaries,
// overlap matching and case folding here. A cursor is exclusive.
llvm::json::Object searchPage(neverd::sdk::Session &S,
                              llvm::ArrayRef<unsigned char> Pattern,
                              bool CaseSensitive, bool TextContext,
                              std::optional<uint64_t> From, bool Backward,
                              size_t Limit) {
  llvm::json::Array Items;
  using Segment = typename std::decay_t<decltype(S.Img.Segments)>::value_type;
  std::vector<const Segment *> Segments;
  for (const auto &Seg : S.Img.Segments)
    Segments.push_back(&Seg);
  std::stable_sort(Segments.begin(), Segments.end(),
                   [](const auto *A, const auto *B) { return A->VA < B->VA; });
  if (Backward)
    std::reverse(Segments.begin(), Segments.end());
  bool Complete = true;
  for (const auto *Seg : Segments) {
    if (Seg->Data.size() < Pattern.size())
      continue;
    size_t First = 0, Last = Seg->Data.size() - Pattern.size();
    if (From) {
      if (Backward) {
        if (*From <= Seg->VA)
          continue;
        Last = std::min<uint64_t>(Last, *From - Seg->VA - 1);
      } else if (*From >= Seg->VA) {
        if (*From - Seg->VA >= Last)
          continue;
        First = size_t(*From - Seg->VA + 1);
      }
    }
    for (size_t Step = 0; Step <= Last - First; ++Step) {
      const size_t I = Backward ? Last - Step : First + Step;
      bool Match = true;
      for (size_t J = 0; J < Pattern.size(); ++J) {
        const auto Fold = [CaseSensitive](unsigned char C) {
          return CaseSensitive ? C
                               : static_cast<unsigned char>(std::tolower(C));
        };
        if (Fold(Seg->Data[I + J]) != Fold(Pattern[J])) {
          Match = false;
          break;
        }
      }
      if (!Match)
        continue;
      if (Items.size() == Limit) {
        Complete = false;
        break;
      }
      llvm::json::Object Hit{{"addr", vaHex(Seg->VA + I)},
                             {"segment", jsonSafeText(Seg->Name)},
                             {"offset", int64_t(I)}};
      if (TextContext) {
        const size_t Count =
            std::min<size_t>({Pattern.size() + 32, Seg->Data.size() - I, 64});
        std::string Context(
            reinterpret_cast<const char *>(Seg->Data.data() + I), Count);
        for (auto &C : Context)
          if (static_cast<unsigned char>(C) < 0x20 || C == 0x7f)
            C = '.';
        Hit["context"] = jsonSafeText(Context);
      }
      Items.push_back(std::move(Hit));
    }
    if (!Complete)
      break;
  }
  llvm::json::Value Next = nullptr;
  if (!Complete && !Items.empty())
    Next = Items.back().getAsObject()->getString("addr")->str();
  return llvm::json::Object{{"items", std::move(Items)},
                            {"complete", Complete},
                            {"next_address", std::move(Next)}};
}
} // namespace

const char *neverd_search_bytes(neverd_session_t Sess, const unsigned char *Pat,
                                int PatLen, int MaxResults) {
  auto *S = toSession(Sess);
  if (!S->Loaded || !Pat || PatLen <= 0)
    return dupStr("[]");
  auto Page = searchPage(
      *S, {Pat, size_t(PatLen)}, true, false, {}, false,
      MaxResults > 0 ? size_t(MaxResults)
                     : static_cast<int>(::neverd::sdk::kDefaultSearchResults));
  return dupStr(jsonToString(std::move(Page["items"])));
}

const char *neverd_search_string(neverd_session_t Sess, const char *Pattern,
                                 int CaseSensitive, int MaxResults) {
  auto *S = toSession(Sess);
  if (!S->Loaded || !Pattern || !*Pattern)
    return dupStr("[]");
  auto Page = searchPage(
      *S,
      {reinterpret_cast<const unsigned char *>(Pattern), std::strlen(Pattern)},
      CaseSensitive != 0, true, {}, false,
      MaxResults > 0 ? size_t(MaxResults)
                     : static_cast<int>(::neverd::sdk::kDefaultSearchResults));
  return dupStr(jsonToString(std::move(Page["items"])));
}

const char *neverd_search_page(neverd_session_t Sess, const char *OptionsJSON) {
  auto *S = toSession(Sess);
  S->clearError();
  const auto Fail = [&](const char *Message) -> const char * {
    S->setError(Message);
    return nullptr;
  };
  if (!S->Loaded)
    return Fail("no binary loaded");
  if (!OptionsJSON || std::strlen(OptionsJSON) > kMaxSearchOptionsBytes)
    return Fail("search options must be bounded JSON");
  auto Parsed = llvm::json::parse(OptionsJSON);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return Fail("invalid search options JSON");
  }
  const auto *Options = Parsed->getAsObject();
  if (!Options)
    return Fail("search options must be an object");
  for (const auto &Field : *Options)
    if (Field.first != "kind" && Field.first != "pattern" &&
        Field.first != "limit" && Field.first != "from" &&
        Field.first != "backward" && Field.first != "case_sensitive")
      return Fail("unknown search option");
  const auto Kind = Options->getString("kind").value_or("text");
  if (Options->get("kind") && !Options->getString("kind"))
    return Fail("search kind must be a string");
  const auto Pattern = Options->getString("pattern");
  const auto Limit =
      Options->getInteger("limit").value_or(kDefaultSearchResults);
  if (!Pattern || Pattern->empty() ||
      Pattern->size() > kMaxSearchPatternBytes || Limit < 1 ||
      Limit > kMaxSearchResults)
    return Fail("a bounded pattern and limit in 1..4096 are required");
  for (const char *Flag : {"backward", "case_sensitive"})
    if (Options->get(Flag) && !Options->getBoolean(Flag))
      return Fail("search flags must be booleans");
  if (Options->get("limit") && !Options->getInteger("limit"))
    return Fail("search limit must be an integer");
  std::optional<uint64_t> From;
  if (Options->get("from")) {
    const auto Address = Options->getString("from");
    uint64_t Value = 0;
    if (!Address || Address->getAsInteger(0, Value))
      return Fail("search cursor must be an address string");
    From = Value;
  }
  std::vector<unsigned char> Bytes;
  if (Kind == "immediate") {
    try {
      return dupStr(jsonToString(llvm::json::Value(searchNativeImmediates(
          *S, *Pattern, From, Options->getBoolean("backward").value_or(false),
          size_t(Limit)))));
    } catch (const std::exception &E) {
      return Fail(E.what());
    }
  }
  if (Kind == "bytes") {
    int High = -1;
    for (const unsigned char C : *Pattern) {
      if (C == ' ' || C == '\t')
        continue;
      const int Digit = C >= '0' && C <= '9'   ? C - '0'
                        : C >= 'a' && C <= 'f' ? C - 'a' + 10
                        : C >= 'A' && C <= 'F' ? C - 'A' + 10
                                               : -1;
      if (Digit < 0)
        return Fail("Byte patterns contain hexadecimal pairs");
      if (High < 0)
        High = Digit;
      else {
        Bytes.push_back(static_cast<unsigned char>(High * 16 + Digit));
        High = -1;
      }
    }
    if (Bytes.empty() || High >= 0)
      return Fail("Byte patterns contain hexadecimal pairs");
  } else if (Kind == "text") {
    Bytes.assign(Pattern->bytes_begin(), Pattern->bytes_end());
  } else
    return Fail("Search kind must be bytes, text or immediate");
  auto Page = searchPage(
      *S, Bytes,
      Kind == "bytes" || Options->getBoolean("case_sensitive").value_or(false),
      Kind == "text", From, Options->getBoolean("backward").value_or(false),
      size_t(Limit));
  return dupStr(jsonToString(llvm::json::Value(std::move(Page))));
}

// ===--------------------------------------------------------------------===//
// Binary diff
// ===--------------------------------------------------------------------===//

const char *neverd_diff_functions(neverd_session_t SessA,
                                  neverd_session_t SessB) {
  auto *A = toSession(SessA);
  auto *B = toSession(SessB);
  if (!A->Loaded || !B->Loaded)
    return dupStr("{}");
  (void)A->synchronizeFunctions();
  (void)B->synchronizeFunctions();

  std::map<std::string, int> MapA, MapB;
  for (int I = 0; I < static_cast<int>(A->Functions.size()); ++I)
    MapA[A->Functions[I].Name] = I;
  for (int I = 0; I < static_cast<int>(B->Functions.size()); ++I)
    MapB[B->Functions[I].Name] = I;

  llvm::json::Array Matched, Added, Removed;

  for (const auto &[Name, IdxA] : MapA) {
    auto It = MapB.find(Name);
    if (It != MapB.end()) {
      int IdxB = It->second;
      llvm::json::Object Obj;
      Obj["name"] = Name;
      Obj["addr_a"] = vaHex(A->Functions[IdxA].Entry);
      Obj["addr_b"] = vaHex(B->Functions[IdxB].Entry);
      Obj["size_a"] = static_cast<int64_t>(A->Functions[IdxA].Size);
      Obj["size_b"] = static_cast<int64_t>(B->Functions[IdxB].Size);
      Obj["same_size"] = (A->Functions[IdxA].Size == B->Functions[IdxB].Size);
      Matched.push_back(std::move(Obj));
    } else {
      llvm::json::Object Obj;
      Obj["name"] = Name;
      Obj["addr"] = vaHex(A->Functions[IdxA].Entry);
      Obj["size"] = static_cast<int64_t>(A->Functions[IdxA].Size);
      Removed.push_back(std::move(Obj));
    }
  }

  for (const auto &[Name, IdxB] : MapB) {
    if (MapA.find(Name) == MapA.end()) {
      llvm::json::Object Obj;
      Obj["name"] = Name;
      Obj["addr"] = vaHex(B->Functions[IdxB].Entry);
      Obj["size"] = static_cast<int64_t>(B->Functions[IdxB].Size);
      Added.push_back(std::move(Obj));
    }
  }

  llvm::json::Object Result;
  Result["matched"] = std::move(Matched);
  Result["added"] = std::move(Added);
  Result["removed"] = std::move(Removed);
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

const char *neverd_diff_decompile(neverd_session_t SessA, neverd_va_t EntryA,
                                  neverd_session_t SessB, neverd_va_t EntryB) {
  const char *CodeA = neverd_decompile(SessA, EntryA);
  const char *CodeB = neverd_decompile(SessB, EntryB);

  llvm::json::Object Result;
  Result["code_a"] = jsonSafeText(CodeA ? CodeA : "");
  Result["code_b"] = jsonSafeText(CodeB ? CodeB : "");
  bool Identical = CodeA && CodeB && std::strcmp(CodeA, CodeB) == 0;
  Result["identical"] = Identical;

  if (CodeA)
    free(const_cast<char *>(CodeA));
  if (CodeB)
    free(const_cast<char *>(CodeB));

  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}
