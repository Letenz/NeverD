#include "../lift/NeverDLiftFixture.h"

namespace {
class BytecodeCLI : public NeverDLiftTest {};

TEST_F(BytecodeCLI, JoinedLoopBackEdgesKeepLabelsAndStateUpdates) {
  const auto Code = tmpFile("control.bin");
  const auto Profile = tmpFile("control.json");
  const auto Functions = tmpFile("control-functions.json");
  const auto Output = tmpFile("control.c");
  // A small independently constructed language and graph. Every cycle
  // decrements a caller-supplied budget, so all input combinations terminate.
  // Distinct per-block additions make wrong edges observable, including two
  // conditional edges that become identical after threading an empty jump.
  std::ofstream(Profile) << R"({"version":1,"register_bytes":24,
    "temporary_bytes":32,"byte_order":"little","encodings":[
    {"size":2,"match":[{"offset":0,"value":161}],"operations":[
      {"op":"INT_SUB","output":{"space":"reg","size":8,"value":{}},
       "inputs":[{"space":"reg","size":8,"value":{}},{"space":"const","size":8,"value":{"addend":1}}]},
      {"op":"INT_MULT","output":{"space":"reg","size":8,"value":{"addend":8}},
       "inputs":[{"space":"reg","size":8,"value":{"addend":8}},{"space":"const","size":8,"value":{"addend":13}}]},
      {"op":"INT_ADD","output":{"space":"reg","size":8,"value":{"addend":8}},
       "inputs":[{"space":"reg","size":8,"value":{"addend":8}},{"space":"const","size":8,"value":{"offset":1,"bytes":1}}]}]},
    {"size":5,"match":[{"offset":0,"value":178}],"operations":[
      {"op":"INT_EQUAL","output":{"space":"temp","size":1,"value":{}},
       "inputs":[{"space":"reg","size":8,"value":{}},{"space":"const","size":8,"value":{}}]},
      {"op":"COND_BR","inputs":[{"space":"const","size":8,"value":{"offset":1,"bytes":4}},
       {"space":"temp","size":1,"value":{}}]}]},
    {"size":6,"match":[{"offset":0,"value":195}],"operations":[
      {"op":"INT_AND","output":{"space":"temp","size":8,"value":{}},
       "inputs":[{"space":"reg","size":8,"value":{"addend":8}},
       {"space":"const","size":8,"value":{"offset":5,"bytes":1}}]},
      {"op":"INT_NOTEQUAL","output":{"space":"temp","size":1,"value":{"addend":8}},
       "inputs":[{"space":"temp","size":8,"value":{}},{"space":"const","size":8,"value":{}}]},
      {"op":"COND_BR","inputs":[{"space":"const","size":8,"value":{"offset":1,"bytes":4}},
       {"space":"temp","size":1,"value":{"addend":8}}]}]},
    {"size":5,"match":[{"offset":0,"value":212}],"operations":[
      {"op":"BRANCH","inputs":[{"space":"const","size":8,"value":{"offset":1,"bytes":4}}]}]},
    {"size":1,"match":[{"offset":0,"value":229}],"operations":[{"op":"RETURN","inputs":[]}]}]})";
  const unsigned Graph[][4] = {
      {3, 9, 4, 144},   {3, 1, 32, 196},  {0, 11, 32, 107}, {5, 3, 32, 212},
      {5, 6, 16, 230},  {10, 10, 4, 115}, {1, 4, 32, 79},   {8, 9, 16, 94},
      {6, 10, 32, 151}, {6, 1, 32, 214},  {7, 7, 4, 156},   {0, 8, 4, 208}};
  std::vector<uint8_t> Program;
  auto Address = [&](unsigned Value) {
    for (unsigned I = 0; I != 4; ++I)
      Program.push_back(static_cast<uint8_t>(Value >> (I * 8)));
  };
  for (const auto &Edges : Graph) {
    Program.push_back(0xb2);
    Address(216);
    Program.insert(Program.end(), {0xa1, static_cast<uint8_t>(Edges[3]), 0xc3});
    Address(Edges[0] * 18);
    Program.push_back(Edges[2]);
    Program.push_back(0xd4);
    Address(Edges[1] * 18);
  }
  Program.push_back(0xe5);
  std::ofstream(Code, std::ios::binary)
      .write(reinterpret_cast<const char *>(Program.data()), Program.size());
  std::ofstream(Functions)
      << R"([{"entry":0,"end":217,"name":"bounded_graph"}])";
  for (unsigned Route : {0u, 1u, 2u}) {
    SCOPED_TRACE(Route);
    std::vector<std::string> Args{
        Code.string(),      "--profile", Profile.string(), "--functions",
        Functions.string(), "-o",        Output.string()};
    if (Route)
      Args.push_back("--llvm");
    if (Route == 2)
      Args.push_back("--optimize");
    const auto Ran = exec(NEVERD_BYTECODE_BINARY, Args);
    ASSERT_TRUE(Ran.ok()) << Ran.err;
    if (!hasCrossTargetClang())
      continue;
    std::ofstream(Output, std::ios::app) << R"(
int main(void) {
  static const unsigned graph[12][4] = {
    {3,9,4,144}, {3,1,32,196}, {0,11,32,107}, {5,3,32,212},
    {5,6,16,230}, {10,10,4,115}, {1,4,32,79}, {8,9,16,94},
    {6,10,32,151}, {6,1,32,214}, {7,7,4,156}, {0,8,4,208}
  };
  for (unsigned count = 0; count < 32; ++count) {
    for (unsigned seed = 0; seed < 64; ++seed) {
      uint64_t state[3] = {count, seed, UINT64_C(0x0123456789abcdef)};
      uint64_t expected = seed;
      unsigned block = 0;
      for (unsigned i = 0; i < count; ++i) {
        expected = expected * 13 + graph[block][3];
        block = graph[block][(expected & graph[block][2]) ? 0 : 1];
      }
      if (bounded_graph(state) != 0 || state[0] != 0 ||
          state[1] != expected || state[2] != UINT64_C(0x0123456789abcdef))
        return 1;
    }
  }
  return 0;
}
)";
    for (const char *Optimization : {"-O0", "-O2"}) {
      auto Executable = tmpFile(std::string("control-run") +
                                neverd::test::executableSuffix());
      const auto Built =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", Optimization, "-Werror", "-fsanitize=undefined",
                "-fsanitize-trap=undefined", Output.string(), "-o",
                Executable.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      EXPECT_TRUE(exec(Executable.string(), {}).ok());
    }
  }
}

TEST_F(BytecodeCLI, EmitsBothRoutesAndPreservesOutputOnRejectedInput) {
  const auto Code = tmpFile("program.bin");
  const auto Profile = tmpFile("language.json");
  const auto Functions = tmpFile("functions.json");
  const auto Output = tmpFile("output.c");
  const std::string Specification = R"({"version":1,"register_bytes":8,
    "byte_order":"little","encodings":[
    {"size":3,"match":[{"offset":0,"value":180}],"operations":[
      {"op":"COPY","output":{"space":"reg","size":2,"value":{}},
       "inputs":[{"space":"const","size":2,"value":{"offset":1,"bytes":2}}]}]},
    {"size":1,"match":[{"offset":0,"value":231}],"operations":[
      {"op":"RETURN","inputs":[]}]}]})";
  std::ofstream(Profile) << Specification;
  std::ofstream(Functions) << R"([{"entry":0,"end":4,"name":"example"}])";
  const std::vector<std::string> Arguments{
      Code.string(),      "--profile", Profile.string(), "--functions",
      Functions.string(), "-o",        Output.string()};
  auto Bytes = [&]() {
    std::ofstream Stream(Code, std::ios::binary);
    const char Program[] = {char(0xb4), 0x34, 0x12, char(0xe7)};
    Stream.write(Program, sizeof(Program));
  };
  auto Read = [&]() {
    std::ifstream Stream(Output);
    return std::string(std::istreambuf_iterator<char>(Stream), {});
  };
  Bytes();
  for (unsigned Route : {0u, 1u, 2u}) {
    SCOPED_TRACE(Route);
    auto Args = Arguments;
    if (Route)
      Args.push_back("--llvm");
    if (Route == 2)
      Args.push_back("--optimize");
    auto Ran = exec(NEVERD_BYTECODE_BINARY, Args);
    ASSERT_TRUE(Ran.ok()) << Ran.err;
    const auto Source = Read();
    EXPECT_NE(Source.find("example("), std::string::npos);
    EXPECT_EQ(Source.find("unknown value"), std::string::npos);
    if (hasCrossTargetClang()) {
      std::ofstream(Output, std::ios::app) << R"(
int main(void) {
  uint64_t state = UINT64_C(0xabcdef0123456789);
  return example(&state) != 0 || state != UINT64_C(0xabcdef0123451234);
}
)";
      auto Program =
          tmpFile(std::string("cli-source") + neverd::test::executableSuffix());
      auto Built =
          exec(NEVERD_TEST_CLANG, {"-std=c11", "-O2", "-Werror",
                                   Output.string(), "-o", Program.string()});
      ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Read();
      ASSERT_TRUE(exec(Program.string(), {}).ok());
    }
  }
  for (unsigned Case = 0; Case != 7; ++Case) {
    Bytes();
    std::ofstream(Profile) << Specification;
    std::ofstream(Functions) << R"([{"entry":0,"end":4,"name":"example"}])";
    std::ofstream(Output) << "retain this artifact\n";
    if (Case == 0)
      std::ofstream(Code, std::ios::binary) << char(0x42);
    if (Case == 1)
      std::ofstream(Functions) << R"([{"entry":0,"end":4,"name":"return"}])";
    if (Case == 2)
      std::ofstream(Functions) << R"([{"entry":0,"end":2,"name":"example"}])";
    if (Case == 3)
      std::ofstream(Profile) << "{\"version\":1,\"guess\":true}";
    if (Case == 4)
      std::ofstream(Functions)
          << R"([{"entry":0,"end":4,"name":"first"},{"entry":3,"end":4,"name":"second"}])";
    auto Args = Arguments;
    if (Case == 5)
      Args.push_back("--optimize");
    if (Case == 6)
      Args.insert(Args.end(), {"--llvm", "--optimize", "--check"});
    const auto Ran = exec(NEVERD_BYTECODE_BINARY, Args);
    EXPECT_FALSE(Ran.ok());
    EXPECT_EQ(Read(), "retain this artifact\n");
  }
}
} // namespace
