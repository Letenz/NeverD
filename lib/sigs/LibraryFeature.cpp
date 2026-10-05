//===- LibraryFeature.cpp - Bounded library feature admission ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/LibraryFeature.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/PatternParser.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>

using namespace neverd;
using namespace neverd::sigs;

namespace {

constexpr size_t MaxFileBytes = 8 * 1024 * 1024;
constexpr size_t MaxRules = 256;
constexpr size_t MaxNodes = 128;
using Object = llvm::json::Object;
using Value = llvm::json::Value;

std::string digest(llvm::StringRef Text) {
  const auto Hash = llvm::SHA256::hash(llvm::arrayRefFromStringRef(Text));
  return llvm::toHex(Hash, true);
}

bool identifier(llvm::StringRef Text) {
  return !Text.empty() && Text.size() <= 128 && llvm::isLower(Text.front()) &&
         llvm::all_of(Text, [](char C) {
           return llvm::isLower(C) || llvm::isDigit(C) || C == '_' ||
                  C == '.' || C == '-';
         });
}

bool hashString(llvm::StringRef Text) {
  return Text.size() == 64 && llvm::all_of(Text, [](char C) {
           return llvm::isDigit(C) || (C >= 'a' && C <= 'f');
         });
}

/// Accumulate one diagnostic while parsing into private, unpublished values.
/// Every accessor tolerates a missing object; an invalid field cannot turn
/// into a partially accepted rule.
class Reader {
public:
  explicit Reader(std::filesystem::path Root) : Root(std::move(Root)) {}

  std::string Error;
  void require(bool Condition, llvm::StringRef Message) {
    if (!Condition && Error.empty())
      Error = Message.str();
  }

  std::string string(const Object *O, llvm::StringRef Key) {
    auto S = O ? O->getString(Key) : std::nullopt;
    require(S && !S->empty() && S->size() <= 16384 && !S->contains('\0'),
            "missing or invalid string: " + Key.str());
    return S ? S->str() : std::string();
  }

  int64_t integer(const Object *O, llvm::StringRef Key, int64_t Min,
                  int64_t Max) {
    auto N = O ? O->getInteger(Key) : std::nullopt;
    require(N && *N >= Min && *N <= Max,
            "missing or invalid integer: " + Key.str());
    return N && *N >= Min && *N <= Max ? *N : Min;
  }

  void keys(const Object *O, std::initializer_list<llvm::StringRef> Required,
            std::initializer_list<llvm::StringRef> Optional = {}) {
    require(O, "expected object");
    if (!O)
      return;
    for (llvm::StringRef Key : Required)
      require(O->get(Key), "missing field: " + Key.str());
    for (const auto &KV : *O)
      require(llvm::is_contained(Required, KV.first.str()) ||
                  llvm::is_contained(Optional, KV.first.str()),
              "unsupported field: " + KV.first.str());
  }

  std::string read(const std::filesystem::path &Path) {
    std::error_code EC;
    const auto Resolved = std::filesystem::canonical(Path, EC);
    require(!EC, "cannot resolve feature file: " + Path.string());
    if (EC)
      return {};
    auto R = Root.begin();
    auto P = Resolved.begin();
    for (; R != Root.end() && P != Resolved.end() && *R == *P; ++R, ++P) {
    }
    require(R == Root.end() && P != Resolved.end(),
            "feature reference escapes its root");
    const auto Size = std::filesystem::file_size(Resolved, EC);
    require(!EC && Size <= MaxFileBytes, "feature file exceeds byte budget");
    if (!Error.empty())
      return {};
    auto Buffer = llvm::MemoryBuffer::getFile(Resolved.string(), false);
    require(static_cast<bool>(Buffer), "cannot read feature file");
    if (!Buffer)
      return {};
    require((*Buffer)->getBufferSize() <= MaxFileBytes,
            "feature file exceeds byte budget");
    return Error.empty() ? (*Buffer)->getBuffer().str() : std::string();
  }

  std::string reference(const Object *O) {
    keys(O, {"path", "sha256"}, {"size"});
    const std::string Relative = string(O, "path");
    const std::string Hash = string(O, "sha256");
    const std::filesystem::path Path(Relative);
    require(!Path.is_absolute() && Relative.find('\\') == std::string::npos &&
                llvm::none_of(Path,
                              [](const auto &Part) {
                                return Part == ".." || Part == ".";
                              }),
            "invalid feature reference path");
    require(hashString(Hash), "invalid feature SHA-256");
    if (!Error.empty())
      return {};
    std::string Text = read(Root / Path);
    require(digest(Text) == Hash, "feature digest mismatch: " + Relative);
    if (O->get("size"))
      require(integer(O, "size", 0, MaxFileBytes) ==
                  static_cast<int64_t>(Text.size()),
              "feature size mismatch: " + Relative);
    return Text;
  }

  Value json(llvm::StringRef Text) {
    if (!Error.empty())
      return nullptr;
    auto Parsed = llvm::json::parse(Text);
    if (!Parsed) {
      require(false,
              "invalid feature JSON: " + llvm::toString(Parsed.takeError()));
      return nullptr;
    }
    require(Parsed->getAsObject(), "feature document must be an object");
    // LLVM's object parser keeps the last spelling of a repeated key. The
    // feature contract rejects duplicates, including differently escaped
    // spellings. Scan only validated JSON tokens; let LLVM decode each key.
    std::vector<std::set<std::string>> Keys;
    for (size_t I = 0; I < Text.size() && Error.empty(); ++I) {
      if (Text[I] == '{' || Text[I] == '[') {
        Keys.emplace_back();
        require(Keys.size() <= 64, "feature JSON nesting budget exceeded");
      } else if (Text[I] == '}' || Text[I] == ']') {
        Keys.pop_back();
      } else if (Text[I] == '"') {
        const size_t Begin = I++;
        for (; I < Text.size() && Text[I] != '"'; ++I)
          if (Text[I] == '\\')
            ++I;
        size_t Next = I + 1;
        while (Next < Text.size() && llvm::isSpace(Text[Next]))
          ++Next;
        if (Next < Text.size() && Text[Next] == ':') {
          auto Key = llvm::json::parse(Text.slice(Begin, I + 1));
          if (!Key) {
            require(false, llvm::toString(Key.takeError()));
            break;
          }
          require(!Keys.empty() &&
                      Keys.back().insert(Key->getAsString()->str()).second,
                  "duplicate feature JSON key");
        }
      }
    }
    return std::move(*Parsed);
  }

private:
  std::filesystem::path Root;
};

bool comparison(LibraryFeatureOp Op) {
  return Op == LibraryFeatureOp::Eq || Op == LibraryFeatureOp::Ne ||
         Op == LibraryFeatureOp::ULT || Op == LibraryFeatureOp::ULE ||
         Op == LibraryFeatureOp::SLT;
}

void expression(Reader &R, const Object *Pattern, LibraryFeatureRule &Rule) {
  R.keys(Pattern, {"nodes", "result"});
  const auto *Nodes = Pattern ? Pattern->getArray("nodes") : nullptr;
  R.require(Nodes && !Nodes->empty() && Nodes->size() <= MaxNodes,
            "invalid expression node count");
  if (!Nodes || !R.Error.empty())
    return;
  std::map<std::string, unsigned> Indices;
  bool HasReceiver = false;
  for (const Value &V : *Nodes) {
    const Object *N = V.getAsObject();
    R.keys(N, {"id", "op", "bits"}, {"args", "role", "value", "offset"});
    const std::string Id = R.string(N, "id");
    R.require(identifier(Id) && !Indices.contains(Id),
              "invalid or duplicate expression node ID");
    const std::string OpName = R.string(N, "op");
    LibraryFeatureNode Node;
    unsigned Arity = 0;
    bool Known = false;
#define NEVERD_LIBRARY_FEATURE_OP(Name, Text, Count)                           \
  if (OpName == Text) {                                                        \
    Node.Op = LibraryFeatureOp::Name;                                          \
    Arity = Count;                                                             \
    Known = true;                                                              \
  }
#include "neverd/sigs/LibraryFeatureOps.def"
    R.require(Known, "unsupported expression operator");
    Node.Bits = R.integer(N, "bits", 8, 64);
    R.require(Node.Bits == 8 || Node.Bits == 16 || Node.Bits == 32 ||
                  Node.Bits == 64,
              "unsupported expression width");
    const auto *Args = N ? N->getArray("args") : nullptr;
    R.require((!Args && Arity == 0 && (!N || !N->get("args"))) ||
                  (Args && Args->size() == Arity),
              "wrong expression arity");
    if (Args)
      for (const Value &Arg : *Args) {
        const auto Name = Arg.getAsString();
        auto I = Name ? Indices.find(Name->str()) : Indices.end();
        R.require(I != Indices.end(), "missing or forward expression argument");
        if (I != Indices.end())
          Node.Args.push_back(I->second);
      }
    if (!R.Error.empty())
      return;
    const auto Width = [&](unsigned I) {
      return Rule.Nodes[Node.Args[I]].Bits;
    };
    R.require(static_cast<bool>(N->get("role")) ==
                      (Node.Op == LibraryFeatureOp::Input) &&
                  static_cast<bool>(N->get("value")) ==
                      (Node.Op == LibraryFeatureOp::Constant) &&
                  static_cast<bool>(N->get("offset")) ==
                      (Node.Op == LibraryFeatureOp::Load),
              "expression field belongs to a different operator");
    switch (Node.Op) {
    case LibraryFeatureOp::Input: {
      const std::string Role = R.string(N, "role");
      R.require(Role == "receiver" || Role == "argument", "invalid input role");
      Node.Receiver = Role == "receiver";
      HasReceiver |= Node.Receiver;
      break;
    }
    case LibraryFeatureOp::Constant: {
      const std::string Text = R.string(N, "value");
      const llvm::StringRef Hex(Text);
      R.require(Hex.starts_with("0x") && Hex.size() > 2 &&
                    llvm::all_of(Hex.drop_front(2),
                                 [](char C) {
                                   return llvm::isDigit(C) ||
                                          (C >= 'a' && C <= 'f');
                                 }) &&
                    !Hex.drop_front(2).getAsInteger(16, Node.Constant) &&
                    (Node.Bits == 64 || Node.Constant < (1ULL << Node.Bits)),
                "invalid expression constant");
      break;
    }
    case LibraryFeatureOp::Load:
      Node.Offset = R.integer(N, "offset", -(1 << 20), 1 << 20);
      R.require(Width(0) == 64, "load address must be 64 bits");
      break;
    case LibraryFeatureOp::Zext:
    case LibraryFeatureOp::Sext:
      R.require(Width(0) < Node.Bits, "extension must widen");
      break;
    case LibraryFeatureOp::Select:
      R.require(Width(0) == 8 && Width(1) == Node.Bits && Width(2) == Node.Bits,
                "invalid select widths");
      break;
    default:
      R.require(
          Width(0) == Width(1) &&
              (comparison(Node.Op) ? Node.Bits == 8 : Node.Bits == Width(0)),
          "invalid binary expression widths");
      break;
    }
    Indices.emplace(Id, Rule.Nodes.size());
    Rule.Nodes.push_back(std::move(Node));
  }
  auto Result = Indices.find(R.string(Pattern, "result"));
  R.require(Result != Indices.end(), "missing expression result");
  if (Result == Indices.end())
    return;
  Rule.Result = Result->second;
  std::set<unsigned> Reachable;
  std::vector<unsigned> Pending{Rule.Result};
  while (!Pending.empty()) {
    const unsigned I = Pending.back();
    Pending.pop_back();
    if (Reachable.insert(I).second)
      Pending.insert(Pending.end(), Rule.Nodes[I].Args.begin(),
                     Rule.Nodes[I].Args.end());
  }
  R.require(Reachable.size() == Rule.Nodes.size() && HasReceiver,
            "expression must use every node and bind its receiver");
  R.require(!Rule.supports(LibraryFeatureScope::InlineRegion),
            "expression cannot describe an effect region");
}

void comPattern(Reader &R, const Object *P, LibraryFeatureRule &Rule) {
  R.keys(P, {"kind", "pointer_offset", "interface_type", "addref_slot",
             "release_slot", "policy", "self_assignment"});
  R.require(Rule.Family == "atl-com" &&
                (Rule.ReceiverType == "ATL::CComPtr<IUnknown>" ||
                 (Rule.ReceiverType == "ATL::CComPtrBase<IUnknown>" &&
                  Rule.Operation == "Release")) &&
                !Rule.supports(LibraryFeatureScope::InlineExpression),
            "unsupported COM receiver or scope");
  R.require(R.string(P, "interface_type") == "IUnknown",
            "unsupported COM interface");
  R.integer(P, "pointer_offset", 0, 0);
  R.integer(P, "addref_slot", 1, 1);
  R.integer(P, "release_slot", 2, 2);
  const std::string Policy = R.string(P, "policy");
  using PolicyKind = LibraryFeatureRule::ComPolicy;
  struct PolicyEntry {
    llvm::StringLiteral Operation;
    llvm::StringLiteral Text;
    PolicyKind Policy;
  };
  static constexpr PolicyEntry Policies[] = {
      {"construct", "zero-initialize", PolicyKind::ZeroInitialize},
      {"copy", "store-before-addref", PolicyKind::StoreBeforeAddref},
      {"assign", "addref-publish-release", PolicyKind::AddrefPublishRelease},
      {"Release", "clear-before-release", PolicyKind::ClearBeforeRelease},
      {"destroy", "release-without-clear", PolicyKind::ReleaseWithoutClear}};
  auto I = llvm::find_if(Policies, [&](const PolicyEntry &E) {
    return E.Operation == Rule.Operation && E.Text == Policy;
  });
  R.require(I != std::end(Policies), "wrong COM effect policy");
  if (I != std::end(Policies))
    Rule.Policy = I->Policy;
  R.require(R.string(P, "self_assignment") == (Rule.Operation == "assign"
                                                   ? "skip-equal-pointer"
                                                   : "not-applicable"),
            "wrong COM self-assignment policy");
}

void bytePattern(Reader &R, const Object *P, LibraryFeatureRule &Rule) {
  R.keys(P, {"kind", "file", "symbol", "generator", "fixed_bytes_min"});
  R.integer(P, "fixed_bytes_min", 16, 16);
  R.require(R.string(P, "generator") == "neverd-sigmaker" &&
                Rule.Scopes ==
                    static_cast<uint8_t>(LibraryFeatureScope::WholeFunction),
            "unsupported byte pattern producer or scope");
  Rule.LinkageName = R.string(P, "symbol");
  const Object *File = P ? P->getObject("file") : nullptr;
  Rule.PatternSHA256 = R.string(File, "sha256");
  R.require(llvm::StringRef(R.string(File, "path")).ends_with(".pat.txt"),
            "gated byte pattern must use .pat.txt");
  Rule.PatternText = R.reference(File);
  if (!R.Error.empty())
    return;
  auto Modules = PatternParser::parseText(Rule.PatternText);
  if (!Modules) {
    R.require(false, llvm::toString(Modules.takeError()));
    return;
  }
  R.require(Modules->size() == 1, "expected one generated byte pattern");
  if (Modules->size() != 1)
    return;
  const PatternModule &M = Modules->front();
  R.require(M.PublicNames.size() == 1 && M.PublicNames[0].Offset == 0 &&
                M.PublicNames[0].Name == Rule.LinkageName,
            "byte pattern and entry symbol disagree");
  R.require(
      llvm::count_if(M.LeadingBytes,
                     [](const PatternByte &B) { return !B.IsWildcard; }) >= 16,
      "insufficient fixed leading bytes");
}

bool operation(llvm::StringRef Family, llvm::StringRef Name) {
  if (Family == "stl")
    return llvm::StringSwitch<bool>(Name)
        .Cases({"size", "empty", "data", "capacity"}, true)
        .Default(false);
  if (Family == "atl-mfc-string")
    return llvm::StringSwitch<bool>(Name)
        .Cases({"construct", "copy", "assign", "GetLength", "IsEmpty"}, true)
        .Cases({"GetString", "GetBuffer", "ReleaseBuffer"}, true)
        .Default(false);
  if (Family == "atl-com")
    return llvm::StringSwitch<bool>(Name)
        .Cases({"construct", "copy", "assign", "Release", "destroy"}, true)
        .Default(false);
  return Family == "libc" &&
         llvm::StringSwitch<bool>(Name)
             .Cases({"memcpy", "memmove", "memset", "memcmp", "strlen"}, true)
             .Default(false);
}

void readRules(Reader &R, const Object *Pack, LibraryFeaturePack &Out) {
  const auto *Rules = Pack ? Pack->getArray("rules") : nullptr;
  R.require(Rules && !Rules->empty() && Rules->size() <= MaxRules,
            "invalid library rule count");
  if (!Rules || !R.Error.empty())
    return;
  std::set<std::string> IDs;
  for (const Value &V : *Rules) {
    const Object *O = V.getAsObject();
    R.keys(O,
           {"id", "revision", "family", "operation", "receiver_type", "layout",
            "identity_evidence", "scope", "pattern", "positive", "negative"});
    LibraryFeatureRule Rule;
    Rule.Id = R.string(O, "id");
    R.require(identifier(Rule.Id) && IDs.insert(Rule.Id).second,
              "invalid or duplicate rule ID");
    Rule.Revision =
        R.integer(O, "revision", 1, std::numeric_limits<unsigned>::max());
    Rule.Family = R.string(O, "family");
    Rule.Operation = R.string(O, "operation");
    R.require(operation(Rule.Family, Rule.Operation),
              "unsupported library operation");
    Rule.Layout = R.string(O, "layout");
    if (Rule.Family != "libc")
      Rule.ReceiverType = R.string(O, "receiver_type");
    else
      R.require(O && O->get("receiver_type") &&
                    O->get("receiver_type")->getAsNull(),
                "libc must have a null receiver");
    const auto Layout = Out.Layouts.find(Rule.Layout);
    R.require(Layout != Out.Layouts.end() &&
                  Layout->second.ReceiverType == Rule.ReceiverType,
              "receiver contradicts its profile layout");
    const std::string Identity = R.string(O, "identity_evidence");
    if (Identity == "authoritative-receiver")
      Rule.Identity = LibraryFeatureIdentity::Receiver;
    else if (Identity == "authenticated-call-relationship")
      Rule.Identity = LibraryFeatureIdentity::CallRelationship;
    else if (Identity == "byte-signature" && Rule.Family == "libc")
      Rule.Identity = LibraryFeatureIdentity::ByteSignature;
    else
      R.require(false, "unsupported library identity evidence");
    const auto *Scopes = O ? O->getArray("scope") : nullptr;
    R.require(Scopes && !Scopes->empty() && Scopes->size() <= 3,
              "invalid rule scope");
    std::set<std::string> ScopeNames;
    if (Scopes)
      for (const Value &Scope : *Scopes) {
        const auto Name = Scope.getAsString();
        const unsigned Flag = Name ? llvm::StringSwitch<unsigned>(*Name)
                                         .Case("whole-function", 1)
                                         .Case("inline-expression", 2)
                                         .Case("inline-region", 4)
                                         .Default(0)
                                   : 0;
        R.require(Flag && !(Rule.Scopes & Flag),
                  "unsupported or duplicate scope");
        Rule.Scopes |= Flag;
        if (Name)
          ScopeNames.insert(Name->str());
      }
    const Object *Pattern = O ? O->getObject("pattern") : nullptr;
    R.require(Pattern, "missing rule pattern");
    if (!R.Error.empty())
      return;
    if (!Pattern->get("kind")) {
      expression(R, Pattern, Rule);
    } else {
      const std::string Kind = R.string(Pattern, "kind");
      if (Kind == "byte-pattern") {
        Rule.PatternKind = LibraryFeatureRule::Kind::BytePattern;
        bytePattern(R, Pattern, Rule);
      } else if (Kind == "com-lifetime") {
        Rule.PatternKind = LibraryFeatureRule::Kind::ComLifetime;
        comPattern(R, Pattern, Rule);
      } else {
        R.require(false, "unsupported library pattern kind");
      }
    }
    R.require(Rule.Family != "libc" ||
                  (Rule.PatternKind == LibraryFeatureRule::Kind::BytePattern &&
                   Rule.Identity == LibraryFeatureIdentity::ByteSignature),
              "libc recognition requires a byte signature");
    std::set<std::string> WitnessScopes;
    for (llvm::StringRef Category : {"positive", "negative"}) {
      const auto *Witnesses = O->getArray(Category);
      R.require(Witnesses && !Witnesses->empty() && Witnesses->size() <= 256,
                "missing or excessive rule witnesses");
      if (!Witnesses)
        continue;
      for (const Value &Witness : *Witnesses) {
        const Object *W = Witness.getAsObject();
        R.keys(W, {"build", "symbol", "expectation"});
        R.string(W, "build");
        const std::string Symbol = R.string(W, "symbol");
        const std::string Expectation = R.string(W, "expectation");
        if (Category == "positive") {
          WitnessScopes.insert(Expectation);
          if (Expectation == "whole-function")
            Rule.WholeFunctionSymbols.push_back(Symbol);
          R.require(Rule.PatternKind != LibraryFeatureRule::Kind::BytePattern ||
                        Symbol == Rule.LinkageName,
                    "byte pattern witness names another callee");
        }
      }
    }
    R.require(WitnessScopes == ScopeNames,
              "every scope needs a positive witness");
    if (!R.Error.empty())
      return;
    Out.Rules.push_back(std::move(Rule));
  }
}

} // namespace

llvm::Expected<LibraryFeaturePack>
neverd::sigs::readLibraryFeaturePack(const std::filesystem::path &Path) {
  std::error_code EC;
  const auto Absolute = std::filesystem::canonical(Path, EC);
  if (EC || Absolute.parent_path().filename() != "rules" ||
      Absolute.parent_path().parent_path().filename() != "features")
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "feature pack must be under features/rules");
  Reader R(Absolute.parent_path().parent_path().parent_path());
  LibraryFeaturePack Out;
  const std::string PackText = R.read(Absolute);
  Out.SHA256 = digest(PackText);
  Value PackValue = R.json(PackText);
  const Object *Pack = PackValue.getAsObject();
  R.keys(Pack,
         {"schema_version", "kind", "id", "profile", "rules", "verification"});
  R.integer(Pack, "schema_version", 1, 1);
  R.require(R.string(Pack, "kind") == "library-feature-pack",
            "wrong feature document kind");
  Out.Id = R.string(Pack, "id");
  R.require(identifier(Out.Id), "invalid feature pack ID");
  const Object *ProfileRef = Pack ? Pack->getObject("profile") : nullptr;
  Out.ProfileSHA256 = R.string(ProfileRef, "sha256");
  Value ProfileValue = R.json(R.reference(ProfileRef));
  const Object *Profile = ProfileValue.getAsObject();
  R.keys(Profile, {"schema_version", "kind", "id", "implementation", "target",
                   "configuration", "compiler", "source", "layouts", "evidence",
                   "artifact_archive"});
  R.integer(Profile, "schema_version", 1, 1);
  R.require(R.string(Profile, "kind") == "library-feature-profile" &&
                R.string(Profile, "id") == Out.Id,
            "feature profile identity mismatch");
  Out.Implementation = R.string(Profile, "implementation");
  const Object *Target = Profile ? Profile->getObject("target") : nullptr;
  R.keys(Target,
         {"architecture", "format", "abi", "pointer_bits", "endianness"});
  const std::string ArchName = R.string(Target, "architecture");
  Out.Architecture = llvm::StringSwitch<Arch>(ArchName)
                         .Case("aarch64", Arch::AArch64)
                         .Case("x86_64", Arch::X64)
                         .Default(Arch::Unknown);
  Out.Format = llvm::StringSwitch<BinaryFormat>(R.string(Target, "format"))
                   .Case("macho", BinaryFormat::MachO)
                   .Case("pe", BinaryFormat::COFF)
                   .Case("elf", BinaryFormat::ELF)
                   .Default(BinaryFormat::Unknown);
  Out.ABI = R.string(Target, "abi");
  R.integer(Target, "pointer_bits", 64, 64);
  R.require(
      R.string(Target, "endianness") == "little" &&
          ((Out.Architecture == Arch::AArch64 &&
            Out.Format == BinaryFormat::MachO && Out.ABI == "darwin-aapcs64") ||
           (Out.Architecture == Arch::X64 && Out.Format == BinaryFormat::COFF &&
            Out.ABI == "win64") ||
           (Out.Architecture == Arch::X64 && Out.Format == BinaryFormat::ELF &&
            Out.ABI == "sysv-amd64")),
      "unsupported feature target");
  const Object *Source = Profile ? Profile->getObject("source") : nullptr;
  R.keys(Source, {"origin", "revision", "license", "files"});
  Out.SourceOrigin = R.string(Source, "origin");
  Out.SourceRevision = R.string(Source, "revision");
  Out.SourceLicense = R.string(Source, "license");
  const Object *Compiler = Profile ? Profile->getObject("compiler") : nullptr;
  Out.CompilerVersion = R.string(Compiler, "version");
  const Object *Evidence = Profile ? Profile->getObject("evidence") : nullptr;
  Out.EvidenceSHA256 = R.string(Evidence, "sha256");
  // Verify the recorded producer manifest, without treating it as evidence
  // about the target binary or decompressing compiler objects during analysis.
  Value EvidenceValue = R.json(R.reference(Evidence));
  const Object *Verification = Pack ? Pack->getObject("verification") : nullptr;
  R.keys(Verification, {"data_verified", "consumer_verified"});
  R.require(Verification && Verification->getBoolean("data_verified") == true &&
                Verification->getBoolean("consumer_verified").has_value(),
            "feature pack lacks producer verification");
  const Object *Layouts = Profile ? Profile->getObject("layouts") : nullptr;
  R.require(Layouts && !Layouts->empty() && Layouts->size() <= 64,
            "invalid layout count");
  if (Layouts)
    for (const auto &KV : *Layouts) {
      const Object *L = KV.second.getAsObject();
      R.require(identifier(KV.first.str()) && L, "invalid profile layout");
      if (!L)
        continue;
      LibraryFeatureLayout Layout;
      const Value *Receiver = L->get("receiver_type");
      if (Receiver && !Receiver->getAsNull()) {
        Layout.ReceiverType = R.string(L, "receiver_type");
        Layout.ObjectBytes = R.integer(L, "object_bytes", 1, 1 << 20);
      } else {
        R.require(Receiver, "missing layout receiver type");
      }
      if (L->get("element_bits"))
        Layout.ElementBits = R.integer(L, "element_bits", 8, 64);
      if (const auto *Derived = L->getArray("known_derived_receivers")) {
        R.require(Derived->size() <= 64, "too many derived receivers");
        for (const Value &D : *Derived) {
          const Object *DO = D.getAsObject();
          R.keys(DO, {"type", "base_offset"});
          std::string Type = R.string(DO, "type");
          int64_t Offset = R.integer(DO, "base_offset", 0, 1 << 20);
          R.require(Layout.DerivedReceivers.emplace(Type, Offset).second,
                    "duplicate derived receiver");
        }
      }
      Out.Layouts.emplace(KV.first.str(), std::move(Layout));
    }
  readRules(R, Pack, Out);
  if (!R.Error.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   Path.string() + ": " + R.Error);
  return Out;
}

bool LibraryFeaturePack::accepts(const BinaryImage &Image) const {
  return accepts(Image.Arch, Image.Format, Image.Bits);
}
