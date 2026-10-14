#include "gtest/gtest.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/PatchObject.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"

namespace {
using namespace neverd;
using Kind = llvm::mc_rewrite::RewriteSectionKind;

std::string hash(llvm::StringRef Bytes) {
  return llvm::toHex(
      llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
          reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size())),
      true);
}

struct Artifact {
  BinaryImage Original;
  CompiledImage Linked;
  SourcePatchMapping Mapping{"replacement", 0x401000};
  const std::string Object = "native object snapshot";
  DynCodePatchEvidence Evidence;
  llvm::json::Object Audit;
  Artifact() {
    Original.Arch = Arch::X64;
    Original.Format = BinaryFormat::ELF;
    Linked.InputKind = RewriteInputKind::ReplacementObject;
    Linked.TargetArch = Original.Arch;
    Linked.Format = Original.Format;
    Linked.BaseVA = 0x500000;
    Linked.Bytes = {0x90, 0xc3};
    Linked.SymbolAddrs.emplace("replacement", Linked.BaseVA);
    CompiledSection Text;
    Text.Name = ".text";
    Text.VA = Linked.BaseVA;
    Text.Size = Linked.Bytes.size();
    Text.Kind = Kind::Code;
    Text.IsAllocated = Text.IsInImage = true;
    Linked.Sections.push_back(Text);
    Evidence.Payload.assign(reinterpret_cast<const char *>(Linked.Bytes.data()),
                            Linked.Bytes.size());
    Audit = llvm::json::Object{{"execution_profile", "hosted-v1"},
                               {"keep_unwind", true},
                               {"selected_section_count", 1},
                               {"remaining_external_count", 0},
                               {"runtime_contract_count", 0},
                               {"padding_size", 0},
                               {"bad_byte_hit_count", 0},
                               {"alignment", 1},
                               {"entry_offset", 0},
                               {"image_size", 2},
                               {"target_triple", "x86_64-unknown-linux-gnu"},
                               {"entry_symbol", "replacement"},
                               {"input_digest", hash(Object)},
                               {"output_digest", hash(Evidence.Payload)}};
    llvm::json::Array Journal;
    for (const char *Key : {"binary", "plan", "relocations"})
      Journal.push_back(
          llvm::json::Object{{"key", Key},
                             {"phase_order", 33},
                             {"provider", "builtin.final_verifier"},
                             {"value", "ok"}});
    Audit["journal"] = std::move(Journal);
  }
  llvm::Error verify(llvm::ArrayRef<SourcePatchBinding> Bindings = {}) {
    Evidence.Audit.clear();
    llvm::raw_string_ostream OS(Evidence.Audit);
    OS << llvm::json::Value(llvm::json::Object(Audit));
    return validateDynCodePatchImage(Evidence, Object, Original, Linked,
                                     {Mapping}, Bindings);
  }
};

TEST(SourcePatchDynCodeEvidence,
     BindsArtifactDigestsEntryAndFinalVerification) {
  Artifact Good;
  EXPECT_FALSE(bool(Good.verify()));
  for (auto [Key, Value] :
       {std::pair<const char *, llvm::json::Value>{"output_digest", "wrong"},
        {"input_digest", "wrong"},
        {"entry_symbol", "other"},
        {"target_triple", "aarch64-unknown-linux-gnu"},
        {"execution_profile", "standalone-v1"},
        {"keep_unwind", false},
        {"runtime_contract_count", 1},
        {"remaining_external_count", 1},
        {"entry_offset", 1},
        {"image_size", 3},
        {"alignment", 16},
        {"padding_size", 1},
        {"selected_section_count", 0},
        {"selected_section_count", 2}}) {
    SCOPED_TRACE(Key);
    Artifact Bad;
    Bad.Audit[Key] = Value;
    auto Error = Bad.verify();
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  Artifact Missing;
  Missing.Audit.erase("journal");
  auto Error = Missing.verify();
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}

TEST(SourcePatchDynCodeEvidence, RejectsChangedCodeAndReorderedNativeEntry) {
  Artifact Changed;
  Changed.Linked.Bytes.front() ^= 1;
  auto Error = Changed.verify();
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  Artifact Reordered;
  Reordered.Linked.SymbolAddrs["replacement"] += 1;
  Error = Reordered.verify();
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  Artifact Truncated;
  Truncated.Linked.Sections.front().Size += 1;
  Error = Truncated.verify();
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}

TEST(SourcePatchDynCodeEvidence, RejectsForeignDataStorageAndOriginalBindings) {
  Artifact Stateful;
  CompiledSection Data = Stateful.Linked.Sections.front();
  Data.Name = ".data";
  Data.Kind = Kind::WritableData;
  Stateful.Linked.Sections.push_back(Data);
  auto Error = Stateful.verify();
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  Artifact Bound;
  SourcePatchBinding Binding{"original", SourcePatchBindingKind::Function,
                             0x402000, 0};
  Error = Bound.verify({Binding});
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  Artifact Foreign;
  Foreign.Linked.TargetArch = Arch::AArch64;
  Error = Foreign.verify();
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}
} // namespace
