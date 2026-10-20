// Real SDK search regressions without private C++ session dependencies.
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/JSON.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
std::string takeString(const char *value) {
  std::string text = value ? value : "";
  neverd_free_string(value);
  return text;
}
class SearchCAPITest : public ::testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;
  void SetUp() override {
    Directory =
        std::filesystem::temp_directory_path() /
        ("neverd-search-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(Directory));
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code error;
    std::filesystem::remove_all(Directory, error);
    EXPECT_FALSE(error) << error.message();
  }
  std::string write(const std::string &name, const std::string &bytes) {
    const auto path = (Directory / name).string();
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), bytes.size());
    output.close();
    EXPECT_TRUE(output);
    return path;
  }
};
TEST_F(SearchCAPITest, SearchPagesContinuePast4096AndReverseWithoutRepeating) {
  std::string Bytes(10000, '\xCC');
  Bytes.replace(8000, 4, "AbAb");
  const auto Path = write("search.bin", Bytes);
  ASSERT_EQ(
      neverd_session_set_load_options(
          Session, R"({"loader":"binary","processor":"x86_64","base":"0x0"})"),
      0);
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  auto First = llvm::json::parse(takeString(neverd_search_page(
      Session, R"({"kind":"bytes","pattern":"CC","limit":4096})")));
  ASSERT_TRUE(bool(First));
  auto *Page = First->getAsObject();
  ASSERT_TRUE(Page);
  ASSERT_EQ(Page->getArray("items")->size(), 4096u);
  EXPECT_EQ(Page->getBoolean("complete"), false);
  EXPECT_EQ(Page->getString("next_address"), "0xFFF");
  EXPECT_EQ(Page->getArray("items")->front().getAsObject()->getString("addr"),
            "0x0");
  auto Next = llvm::json::parse(takeString(neverd_search_page(
      Session,
      R"({"kind":"bytes","pattern":"CC","from":"0xFFF","limit":4096})")));
  ASSERT_TRUE(bool(Next));
  EXPECT_EQ(
      Next->getAsObject()->getArray("items")->front().getAsObject()->getString(
          "addr"),
      "0x1000");
  auto Back = llvm::json::parse(takeString(neverd_search_page(
      Session,
      R"({"kind":"bytes","pattern":"CC","from":"0x1000","backward":true,"limit":4096})")));
  ASSERT_TRUE(bool(Back));
  EXPECT_EQ(Back->getAsObject()->getBoolean("complete"), true);
  EXPECT_EQ(Back->getAsObject()->getArray("items")->size(), 4096u);
  EXPECT_EQ(
      Back->getAsObject()->getArray("items")->front().getAsObject()->getString(
          "addr"),
      "0xFFF");
  EXPECT_EQ(
      Back->getAsObject()->getArray("items")->back().getAsObject()->getString(
          "addr"),
      "0x0");
  auto Text = llvm::json::parse(
      takeString(neverd_search_page(Session, R"({"pattern":"ab","limit":1})")));
  ASSERT_TRUE(bool(Text));
  EXPECT_EQ(Text->getAsObject()->getBoolean("complete"), false);
  EXPECT_EQ(Text->getAsObject()->getString("next_address"), "0x1F40");
  auto Tail = llvm::json::parse(takeString(neverd_search_page(
      Session, R"({"pattern":"ab","from":"0x1F40","limit":1})")));
  ASSERT_TRUE(bool(Tail));
  EXPECT_EQ(Tail->getAsObject()->getBoolean("complete"), true);
  EXPECT_EQ(
      Tail->getAsObject()->getArray("items")->front().getAsObject()->getString(
          "addr"),
      "0x1F42");
  for (const char *Options :
       {R"({"pattern":"CC","kind":4})", R"({"pattern":"CC","limit":true})",
        R"({"pattern":"C","kind":"bytes"})",
        R"({"pattern":"CC","from":"oops"})",
        R"({"pattern":"CC","backward":0})"})
    EXPECT_EQ(neverd_search_page(Session, Options), nullptr) << Options;
}

TEST_F(SearchCAPITest, SearchPreservesOverlapsSegmentsAndLegacyResults) {
  // Initialized segments separated by an unmapped gap must not be joined.
  using namespace llvm::ELF;
  Elf64_Ehdr Header{};
  std::memcpy(Header.e_ident,
              "\x7f"
              "ELF",
              4);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_EXEC;
  Header.e_machine = EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_entry = 0x400000;
  Header.e_ehsize = sizeof(Header);
  Header.e_phoff = sizeof(Header);
  Header.e_phentsize = sizeof(Elf64_Phdr);
  Header.e_phnum = 2;
  Elf64_Phdr Segments[2]{};
  for (unsigned I = 0; I < 2; ++I) {
    Segments[I].p_type = PT_LOAD;
    Segments[I].p_flags = PF_R | (I ? PF_W : PF_X);
    Segments[I].p_offset = 0x100 * (I + 1);
    Segments[I].p_vaddr = 0x400000 + I * 0x100000;
    Segments[I].p_filesz = Segments[I].p_memsz = I ? 4 : 3;
    Segments[I].p_align = 1;
  }
  std::string Bytes(0x204, '\0');
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), Segments, sizeof(Segments));
  Bytes.replace(0x100, 3, "AAA");
  Bytes.replace(0x200, 4, "AbAb");
  const auto Path = write("segments.elf", Bytes);
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  auto Page = llvm::json::parse(takeString(neverd_search_page(
      Session, R"({"pattern":"AA","case_sensitive":true})")));
  ASSERT_TRUE(bool(Page));
  ASSERT_EQ(Page->getAsObject()->getArray("items")->size(), 2u);
  EXPECT_EQ(
      Page->getAsObject()->getArray("items")->back().getAsObject()->getString(
          "addr"),
      "0x400001");
  auto Legacy = llvm::json::parse(
      takeString(neverd_search_string(Session, "AA", 1, 256)));
  ASSERT_TRUE(bool(Legacy));
  EXPECT_EQ(*Legacy, *Page->getAsObject()->get("items"));
  const unsigned char Pattern[] = {'A', 'A'};
  auto LegacyBytes = llvm::json::parse(
      takeString(neverd_search_bytes(Session, Pattern, 2, 256)));
  ASSERT_TRUE(bool(LegacyBytes));
  ASSERT_EQ(LegacyBytes->getAsArray()->size(), Legacy->getAsArray()->size());
  for (size_t I = 0; I < LegacyBytes->getAsArray()->size(); ++I)
    EXPECT_EQ((*LegacyBytes->getAsArray())[I].getAsObject()->getString("addr"),
              (*Legacy->getAsArray())[I].getAsObject()->getString("addr"));
  auto Gap = llvm::json::parse(takeString(neverd_search_page(
      Session, R"({"pattern":"AAAAb","case_sensitive":true})")));
  ASSERT_TRUE(bool(Gap));
  EXPECT_TRUE(Gap->getAsObject()->getArray("items")->empty());
  auto Back = llvm::json::parse(takeString(neverd_search_page(
      Session, R"({"pattern":"ab","backward":true,"limit":1})")));
  ASSERT_TRUE(bool(Back));
  EXPECT_EQ(Back->getAsObject()->getString("next_address"), "0x500002");
  auto Before = llvm::json::parse(takeString(neverd_search_page(
      Session, R"({"pattern":"AA","backward":true,"from":"0x400000"})")));
  ASSERT_TRUE(bool(Before));
  EXPECT_TRUE(Before->getAsObject()->getArray("items")->empty());
}
} // namespace
