//===- Benchmark.cpp - Focused acceleration measurements
//-------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Measures production primitives; reports work digests separately from time.
/// Run Release variants on the same host and compare whole requests as well.
///
//===----------------------------------------------------------------------===//

#include "common/transport/Json.h"

#include "neverd/support/ByteScan.h"
#include "neverd/support/Parallel.h"
#include "neverd/support/TextEncoding.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using Json = neverd::transport::Json;

namespace {
template <class Fn>
Json measure(const char *Name, size_t Bytes, unsigned Rounds, Fn Work) {
  const uint64_t Expected = Work();
  Json Times = Json::array();
  Json CPUTimes = Json::array();
  uint64_t Digest = 0;
  for (unsigned Sample = 0; Sample < 7; ++Sample) {
    const auto Begin = std::chrono::steady_clock::now();
    const auto CPUBegin = std::clock();
    for (unsigned I = 0; I < Rounds; ++I) {
      const auto Result = Work();
      if (Result != Expected)
        throw std::runtime_error("benchmark work changed between iterations");
      Digest += Result;
    }
    const auto Elapsed = std::chrono::duration<double, std::micro>(
                             std::chrono::steady_clock::now() - Begin)
                             .count();
    Times.push_back(Elapsed / Rounds);
    const auto CPUEnd = std::clock();
    if (CPUBegin != std::clock_t(-1) && CPUEnd != std::clock_t(-1))
      CPUTimes.push_back(1e6 * double(CPUEnd - CPUBegin) / CLOCKS_PER_SEC /
                         Rounds);
  }
  return {{"name", Name},
          {"bytes", Bytes},
          {"rounds", Rounds},
          {"result", Expected},
          {"digest", Digest},
          {"microseconds", Times},
          {"cpu_microseconds", CPUTimes}};
}

uint64_t scanASCII(const std::string &Bytes) {
  using namespace neverd::strings::detail;
  uint64_t Result = 0;
  for (size_t I = 0; I < Bytes.size();) {
    const auto *At = reinterpret_cast<const uint8_t *>(Bytes.data() + I);
    if (!isTextASCII(*At)) {
      ++I;
      continue;
    }
    const auto Run = textASCIIPrefix(At, Bytes.size() - I);
    Result += Run * (I + 1);
    I += Run;
  }
  return Result;
}
} // namespace

int main(int Argc, char **Argv) {
  try {
    Json Results = Json::array();
    const std::string LongASCII(1024 * 1024, 'x');
    Results.push_back(measure("ascii-long", LongASCII.size(), 100,
                              [&] { return scanASCII(LongASCII); }));
    std::string Mixed;
    for (unsigned I = 0; I < 1024 * 1024; ++I)
      Mixed.push_back(static_cast<char>((uint64_t(I) * 0x9e3779b9u) >> 24));
    Results.push_back(measure("ascii-binary", Mixed.size(), 10,
                              [&] { return scanASCII(Mixed); }));
    if (Argc == 2) {
      std::ifstream File(Argv[1], std::ios::binary);
      if (!File)
        throw std::runtime_error("cannot open benchmark input");
      const std::string Bytes{std::istreambuf_iterator<char>(File), {}};
      Results.push_back(measure("ascii-input", Bytes.size(), 10,
                                [&] { return scanASCII(Bytes); }));
    }
    std::string Unicode;
    for (unsigned I = 0; I < 65536; ++I)
      Unicode += "neverd\xe4\xb8\xad\xe6\x96\x87";
    Results.push_back(measure("utf8-valid", Unicode.size(), 20, [&] {
      return neverd::escapeInvalidUTF8(Unicode).size();
    }));
    Unicode[Unicode.size() / 2] = '\xff';
    Results.push_back(measure("utf8-invalid", Unicode.size(), 20, [&] {
      return neverd::escapeInvalidUTF8(Unicode).size();
    }));
    for (unsigned Count : {1u, 100u, 10000u}) {
      Json Value = Json::array();
      for (unsigned I = 0; I < Count; ++I)
        Value.push_back({{"id", I},
                         {"name", "function_name"},
                         {"address", I * 16},
                         {"parameters", Json::array({"a", "b"})}});
      const std::string Text = Value.dump();
      const auto Name = "json-" + std::to_string(Count);
      Results.push_back(
          measure(Name.c_str(), Text.size(), Count > 100 ? 5 : 100, [&] {
            const auto Parsed = neverd::transport::parseJson(Text);
            return Parsed.size() + Parsed.back().at("address").get<uint64_t>();
          }));
    }
    Results.push_back(measure("scheduler-small-batches", 64, 100, [&] {
      std::array<uint64_t, 64> Slots{};
      std::atomic<size_t> Next{0};
      auto Work = [&] {
        for (size_t I; (I = Next.fetch_add(1)) < Slots.size();)
          Slots[I] = I * I + 1;
      };
      neverd::runPipelineWorkers(4, Work);
      uint64_t Sum = 0;
      for (auto Value : Slots)
        Sum += Value;
      return Sum;
    }));
    std::cout << Results.dump(2) << '\n';
  } catch (const std::exception &Error) {
    std::cerr << Error.what() << '\n';
    return 1;
  }
}
