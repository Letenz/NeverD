//===- KernelMSRReadTests.cpp - Declared MSR environment dependencies
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Unused CPU input declarations cannot certify a native kernel snapshot.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "neverd/emulation/AddressSpace.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
TEST(KernelMSRRead, EvenUnusedDeclarationsRetainEnvironmentDependencies) {
  constexpr uint64_t Base = 0x180000000, Page = 4096, Limit = 16 * 1024 * 1024;
  for (bool Declared : {false, true}) {
    auto Memory = llvm::cantFail(AddressSpace::create(
        llvm::cantFail(PhysicalMemory::create(Limit)), Limit));
    llvm::cantFail(Memory->map(Base, Page, Read | Execute));
    DriverImage Image;
    Image.Base = Image.Entry = Base;
    Image.Size = Page;
    Image.Regions = {{Base, Read | Execute, std::vector<uint8_t>(Page)}};
    DriverResult Result;
    KernelModel Model(*Memory, Result);
    DriverOptions Options;
    Options.Contract = ExecutionContract::CheckedX64;
    if (Declared)
      Options.MSRReads = {{0xc0000082, 0}};
    llvm::cantFail(Model.initialize(Image, Options));
    Options.MSRReads.clear();
    llvm::cantFail(Model.captureUnpackBaseline());
    const auto Dependencies = llvm::cantFail(Model.unpackDependencies());
    EXPECT_EQ(std::find(Dependencies.begin(), Dependencies.end(),
                        "unreconstructed kernel or environment effects") !=
                  Dependencies.end(),
              Declared);
  }
}

TEST(KernelMSRRead, DirectModelInitializationKeepsDeclarationValidation) {
  constexpr uint64_t Limit = 16 * 1024 * 1024;
  for (bool BadSelector : {false, true}) {
    auto Memory = llvm::cantFail(AddressSpace::create(
        llvm::cantFail(PhysicalMemory::create(Limit)), Limit));
    DriverResult Result;
    KernelModel Model(*Memory, Result);
    DriverOptions Options;
    Options.Contract =
        BadSelector ? ExecutionContract::CheckedX64 : ExecutionContract::Legacy;
    Options.MSRReads = {{BadSelector ? 0xc0000101u : 0xc0000082u, 0}};
    auto Error = Model.initialize(DriverImage{}, Options);
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(
        llvm::toString(std::move(Error))
            .find(BadSelector ? "unsupported MSR selector" : "checked-x64"),
        std::string::npos);
  }
}
} // namespace
} // namespace neverd::emulation
