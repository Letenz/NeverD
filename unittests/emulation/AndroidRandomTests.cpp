//===- AndroidRandomTests.cpp - Bionic PRNG semantic callers -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compares native callers with independently compiled Android 9 answers.
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <tuple>
namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidRandom : public testing::TestWithParam<
                          std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->TraceLimit = 16384;
    Options.Android->Memory.push_back({Buffer, 8192, {}, false});
    Options.Android->ReadMemory.push_back({Buffer, 512});
    Options.Limits.Instructions = 1000000;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {ProcessProfile::AndroidNativeAArch64,
              GuestArchitecture::AArch64,
              Options.Backend,
              {}};
    }
    return std::move(*R);
  }
  void returned(const ProcessResult &R, uint64_t Value = 0) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
  }
};

// Generated with the unmodified arithmetic in AOSP android-9.0.0_r1
// libc/upstream-netbsd/common/lib/libc/stdlib/random.c (NetBSD 1.4).
// Source SHA-256:
// 09d841bd15b11a4e3e5e084ac9f459ce085887b103dfbd25db92f23bb734265a Only host
// namespace/lock headers were adapted; compiled at -O0 -fwrapv. These literals
// test zero/high-bit seeds, both pointer wraps and mixed APIs.
struct Answer {
  uint32_t Seed;
  std::array<uint32_t, 64> Values;
};
constexpr Answer Answers[] = {
    {0u, {2059215011u, 2052119187u, 2047820401u, 968558420u,  2033801269u,
          2028408493u, 2022120753u, 2011989735u, 931476857u,  924298741u,
          1986019194u, 1977521812u, 1969298984u, 881354730u,  870707920u,
          1934986164u, 844534382u,  831227251u,  1894014004u, 800688518u,
          784108052u,  1845081469u, 1822213460u, 727886662u,  1786633308u,
          686278071u,  660821379u,  1716810928u, 1685933010u, 580839665u,
          559652127u,  1597664373u, 485475204u,  459988880u,  418739145u,
          371792825u,  340913725u,  293376250u,  236298912u,  1272390583u,
          1217674992u, 74834458u,   1102428747u, 1039490328u, 956189189u,
          1973136667u, 826992844u,  1800723571u, 656880271u,  573523200u,
          453928442u,  1440988323u, 271121021u,  128658254u,  21391338u,
          2057754329u, 814936325u,  682212717u,  1627081609u, 353385687u,
          1263052383u, 39250088u,   1951050060u, 1748527587u}},
    {1u, {1804289383u, 846930886u,  1681692777u, 1714636915u, 1957747793u,
          424238335u,  719885386u,  1649760492u, 596516649u,  1189641421u,
          1025202362u, 1350490027u, 783368690u,  1102520059u, 2044897763u,
          1967513926u, 1365180540u, 1540383426u, 304089172u,  1303455736u,
          35005211u,   521595368u,  294702567u,  1726956429u, 336465782u,
          861021530u,  278722862u,  233665123u,  2145174067u, 468703135u,
          1101513929u, 1801979802u, 1315634022u, 635723058u,  1369133069u,
          1125898167u, 1059961393u, 2089018456u, 628175011u,  1656478042u,
          1131176229u, 1653377373u, 859484421u,  1914544919u, 608413784u,
          756898537u,  1734575198u, 1973594324u, 149798315u,  2038664370u,
          1129566413u, 184803526u,  412776091u,  1424268980u, 1911759956u,
          749241873u,  137806862u,  42999170u,   982906996u,  135497281u,
          511702305u,  2084420925u, 1937477084u, 1827336327u}},
    {42u, {71876166u,   708592740u,  1483128881u, 907283241u,  442951012u,
           537146758u,  1366999021u, 1854614940u, 647800535u,  53523743u,
           783815874u,  1643643143u, 682599717u,  291474504u,  229233696u,
           1633529762u, 175389892u,  1183169448u, 1212580698u, 1596161259u,
           2108313867u, 469976352u,  975807809u,  1113801033u, 1232315727u,
           1192349579u, 1564541169u, 1350496504u, 1709672141u, 1253520176u,
           590056433u,  1781548307u, 1962112916u, 2073185314u, 541347900u,
           257580280u,  462848424u,  1908346921u, 2112195221u, 1110648960u,
           1961870665u, 748527447u,  606808455u,  496986734u,  1040001951u,
           836042151u,  2130516497u, 1215391843u, 2019211600u, 1195613547u,
           664069454u,  1980041819u, 1665589900u, 1639877263u, 946359204u,
           750421979u,  684743195u,  363416725u,  2100918483u, 246931688u,
           1616936901u, 543491269u,  2028479995u, 1431566170u}},
    {2147483647u,
     {977399425u,  2046899839u, 966903128u,  2032571191u, 2027351843u,
      946434570u,  2010447341u, 2003997915u, 922023992u,  910350580u,
      902358760u,  892905895u,  878957734u,  1942780949u, 858165032u,
      1915307431u, 829250001u,  816707113u,  797028380u,  781743999u,
      767223861u,  1817721886u, 1798777367u, 1781893176u, 1754533593u,
      1731097500u, 637620365u,  605520650u,  1650340079u, 1627139065u,
      515848787u,  480255856u,  1526555256u, 1482751916u, 365343399u,
      1406423451u, 281702838u,  228307092u,  1262937718u, 1203726831u,
      1138657673u, 17812830u,   2096632726u, 2017615407u, 1960593779u,
      807314111u,  1785439190u, 642360133u,  1624021224u, 434983923u,
      1424104132u, 243761438u,  105222161u,  1075397851u, 2025654614u,
      1859755754u, 659011703u,  515791331u,  317792756u,  161868134u,
      2142930396u, 833641544u,  642123991u,  1522002004u}},
    {2147483648u,
     {1336741213u, 1210407648u, 1447044896u, 337392383u,  82502902u,
      538660432u,  1313908778u, 370221063u,  344413073u,  1896089129u,
      2044477265u, 1711647701u, 128787895u,  864892100u,  985836693u,
      2134634618u, 1642121871u, 1452847611u, 539426807u,  1663660949u,
      765987032u,  1350845683u, 1550946986u, 94407367u,   1482267241u,
      927920760u,  405299531u,  446952434u,  1723835522u, 1213320778u,
      1649425186u, 913093087u,  276244779u,  948986434u,  1250485470u,
      358747681u,  1487646867u, 416910600u,  728968744u,  1832059940u,
      165516081u,  625962361u,  1396223994u, 294303977u,  1490854462u,
      234577039u,  281454947u,  985492685u,  1687424650u, 820881754u,
      501669986u,  305928034u,  24243790u,   2052616972u, 400335402u,
      1506511031u, 833054085u,  805634933u,  1953463465u, 409405959u,
      2018955712u, 1455405003u, 1322499046u, 147716843u}},
    {4294967295u,
     {254925627u,  1205188300u, 366127624u,  1401405153u, 76053476u,
      1604170158u, 1302235366u, 362229243u,  334960208u,  1882140968u,
      960816832u,  627031785u,  1185930294u, 1926318319u, 973293805u,
      2114955885u, 1626837490u, 1438327473u, 1589924831u, 1644716430u,
      749102841u,  1323486100u, 1527510893u, 1148413881u, 1450167526u,
      1972740189u, 382098517u,  1483145805u, 1688242591u, 112136530u,
      1605621846u, 1943168218u, 1317324830u, 1971749470u, 1197089724u,
      1393378306u, 1428435980u, 351841442u,  1755607549u, 1763396188u,
      86498762u,   568940733u,  242944325u,  1272429056u, 347775404u,
      1216238130u, 1239901294u, 1974612895u, 507081955u,  682342477u,
      1471845677u, 1256184797u, 2005828578u, 851872922u,  257115030u,
      1308512456u, 677129463u,  639213547u,  644174613u,  217888406u,
      751350077u,  102312811u,  13572976u,   2068674908u}},
};

TEST_P(AndroidRandom, PinnedSequencesWidthsAndProcessIsolation) {
  for (const auto &A : Answers) {
    for (uint64_t Mode : {1, 2}) {
      auto R = run("random_sequence",
                   {Buffer, 0xabcdef0000000000ULL | A.Seed, Mode});
      returned(R);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      for (size_t I = 0; I < A.Values.size(); ++I)
        EXPECT_EQ(llvm::support::endian::read32le(
                      R.MemorySnapshots[0].Bytes.data() + 4 * I),
                  A.Values[I])
            << A.Seed << ":" << I;
      EXPECT_FALSE(R.TraceTruncated);
    }
  }
  auto R = run("random_sequence", {Buffer, 0, 0});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  for (size_t I = 0; I < 64; ++I)
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + 4 * I),
              Answers[1].Values[I]);
}
TEST_P(AndroidRandom, ReseedingSharesStateAcrossAliases) {
  auto R = run("random_reseed", {Buffer});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const uint32_t Expected[] = {Answers[2].Values[0], Answers[2].Values[1],
                               Answers[1].Values[0], Answers[1].Values[1],
                               Answers[2].Values[0]};
  for (unsigned I = 0; I < 5; ++I)
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + 4 * I),
              Expected[I]);
}
TEST_P(AndroidRandom, ThreadsConsumeOneProcessSequence) {
  Options.Android->ThreadLimit = 2;
  for (uint64_t Quantum : {7, 4096}) {
    Options.InstructionQuantum = Quantum;
    auto R = run("random_threads", {Buffer});
    returned(R);
    unsigned I = 0;
    for (const auto &C : R.NativeCalls)
      if (C.Name == "rand" || C.Name == "random") {
        ASSERT_LT(I, 64u);
        EXPECT_EQ(C.Result, Answers[2].Values[I++]);
      }
    EXPECT_EQ(I, 64u);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    for (const auto &T : R.NativeThreads)
      EXPECT_TRUE(T.Finished);
  }
}
TEST_P(AndroidRandom, ReentrantWordsPreservePaddingAndGlobalState) {
  Options.Android->Memory[0].Bytes.assign(8192, 0xa5);
  llvm::support::endian::write32le(
      Options.Android->Memory[0].Bytes.data() + 4092, 0xffffffffu);
  Options.Android->ReadMemory.push_back({Buffer + 4088, 8});
  auto R = run("random_reentrant", {Buffer, Buffer + 4092});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 2u);
  // Independent 64-bit modular arithmetic, including full high-bit seed stores.
  uint64_t Seed = 0xffffffffu;
  for (unsigned I = 0; I < 8; ++I) {
    Seed = (Seed * 1103515245 + 12345) % 0x100000000ULL;
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + I * 8),
              Seed % 0x80000000ULL);
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + I * 8 + 4),
              Seed);
  }
  EXPECT_EQ(
      llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data() + 64),
      Answers[1].Values[0]);
  EXPECT_EQ(llvm::support::endian::read32le(R.MemorySnapshots[1].Bytes.data()),
            0xa5a5a5a5u);
  EXPECT_EQ(
      llvm::support::endian::read32le(R.MemorySnapshots[1].Bytes.data() + 4),
      Seed);
}
TEST_P(AndroidRandom, InvalidSeedMemoryStopsBeforeAResult) {
  for (uint64_t Address :
       std::array<uint64_t, 4>{0, 1, Buffer + 8190, UINT64_MAX}) {
    auto R = run("random_reentrant", {Buffer, Address});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "rand_r");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_FALSE(R.ReturnValue);
  }
  auto R = run("random_readonly", {Buffer + 4092});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "rand_r");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidRandom, DynamicCallsRetainStateAndProviderLifetime) {
  Options.Android->Libraries = {{"libprng.so", {"rand", "srandom"}}};
  auto R = run("random_dynamic", {Buffer, 0});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  for (unsigned I = 0; I < 3; ++I)
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + 4 * I),
              Answers[2].Values[I]);
  R = run("random_dynamic", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "rand");
  EXPECT_EQ(R.NativeCalls.back().Library, "libprng.so");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidRandom, AlternateStateRequiresAnImplementedContract) {
  auto R = run("random_custom_state", {Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, "unmodeled Android import: initstate");
  EXPECT_FALSE(R.ReturnValue);
}
INSTANTIATE_TEST_SUITE_P(
    OptimizationPackingAndBackend, AndroidRandom,
    testing::Combine(testing::Values("random-O0-none", "random-O0-android",
                                     "random-O0-relr", "random-O2-none",
                                     "random-O2-android", "random-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF)),
    [](const testing::TestParamInfo<AndroidRandom::ParamType> &Info) {
      std::string Name = std::get<0>(Info.param);
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name + "_" + executionBackendName(std::get<1>(Info.param));
    });
} // namespace
} // namespace neverd::emulation
