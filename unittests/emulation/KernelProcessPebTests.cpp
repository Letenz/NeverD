//===- KernelProcessPebTests.cpp - Process input identity boundaries
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "neverd/emulation/AddressSpace.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
class KernelProcessPeb : public testing::Test {
protected:
  static constexpr uint64_t Limit = 16 * 1024 * 1024;
  std::shared_ptr<AddressSpace> Memory = llvm::cantFail(AddressSpace::create(
      llvm::cantFail(PhysicalMemory::create(Limit)), Limit));
  DriverResult Result;
  KernelModel Model{*Memory, Result};
  void initialize(std::vector<DriverProcessPeb> Entries) {
    DriverOptions Options;
    Options.Contract = ExecutionContract::CheckedX64;
    Options.ProcessPebs = std::move(Entries);
    DriverImage Image;
    Image.Base = Image.Entry = 0x180000000;
    Image.Size = 4096;
    Image.Regions = {{Image.Base, Read | Execute, std::vector<uint8_t>(4096)}};
    llvm::cantFail(Memory->map(Image.Base, Image.Size, Read | Execute));
    llvm::cantFail(Model.initialize(Image, Options));
    Options.ProcessPebs.clear();
    Model.enterExecution(profile::StackBase);
  }
  uint64_t call(llvm::StringRef Name, std::initializer_list<uint64_t> Args) {
    auto Value = Model.call(Name.str(), Args);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }
  std::string fail(llvm::StringRef Name, std::initializer_list<uint64_t> Args) {
    auto Value = Model.call(Name.str(), Args);
    if (Value) {
      ADD_FAILURE() << "unexpectedly accepted " << Name.str();
      return {};
    }
    return llvm::toString(Value.takeError());
  }
};

TEST_F(KernelProcessPeb, KnownObjectsKeepTheirOwnCopiedDeclaration) {
  initialize({{4, 0}, {0x2000, 0x123456789abcdef0ULL}});
  const auto System = call("PsGetCurrentProcess", {});
  EXPECT_EQ(call("PsGetProcessPeb", {System}), 0u);
  llvm::cantFail(Model.setUserRequestContext(true, 0x2000));
  const auto User = call("PsGetCurrentProcess", {});
  EXPECT_NE(System, User);
  EXPECT_EQ(call("PsGetProcessPeb", {System}), 0u);
  EXPECT_EQ(call("PsGetProcessPeb", {User}), 0x123456789abcdef0ULL);
  llvm::cantFail(Model.setUserRequestContext(false));
  EXPECT_EQ(call("PsGetProcessPeb", {User}), 0x123456789abcdef0ULL);
  EXPECT_EQ(call("PsGetCurrentProcess", {}), System);
  auto Access = Model.validateGuestAccess(User, 1, false);
  ASSERT_TRUE(bool(Access));
  EXPECT_NE(llvm::toString(std::move(Access)).find("opaque process"),
            std::string::npos);
}

TEST_F(KernelProcessPeb, MissingAndForgedObjectsDoNotAcquireIdentity) {
  initialize({{8, 0}});
  const auto System = call("PsGetCurrentProcess", {});
  EXPECT_NE(
      fail("PsGetProcessPeb", {System}).find("undeclared process identity"),
      std::string::npos);
  for (uint64_t Value : std::initializer_list<uint64_t>{
           0, 4, 8, System + 1, call("PsGetCurrentThread", {})})
    EXPECT_NE(fail("PsGetProcessPeb", {Value}).find("known process object"),
              std::string::npos);
  constexpr uint64_t Scratch = 0x60000000;
  llvm::cantFail(Memory->map(Scratch, 4096, Read | Write));
  llvm::cantFail(Memory->writeInteger(Scratch, 0x12345678, 8));
  EXPECT_EQ(call("PsLookupProcessByProcessId", {8, Scratch}), 0xc000000bu);
  EXPECT_EQ(llvm::cantFail(Memory->readInteger(Scratch, 8)), 0x12345678u);
}

TEST_F(KernelProcessPeb, ArityAndPassiveCeilingStayExplicit) {
  initialize({{4, 0}});
  const auto Process = call("PsGetCurrentProcess", {});
  EXPECT_FALSE(fail("PsGetProcessPeb", {}).empty());
  EXPECT_FALSE(fail("PsGetProcessPeb", {Process, 0}).empty());
  EXPECT_EQ(call("KfRaiseIrql", {1}), 0u);
  EXPECT_NE(fail("PsGetProcessPeb", {Process}).find("PASSIVE_LEVEL"),
            std::string::npos);
  call("KeLowerIrql", {0});
  EXPECT_EQ(call("PsGetProcessPeb", {Process}), 0u);
}

TEST_F(KernelProcessPeb, UnusedDeclarationRetainsEnvironmentDependency) {
  initialize({{4, 0}});
  llvm::cantFail(Model.captureUnpackBaseline());
  const auto Dependencies = llvm::cantFail(Model.unpackDependencies());
  EXPECT_NE(std::find(Dependencies.begin(), Dependencies.end(),
                      "unreconstructed kernel or environment effects"),
            Dependencies.end());
}

TEST(KernelProcessPebInitialization, RejectsInvalidAndNonCheckedDeclarations) {
  for (auto Entries : {std::vector<DriverProcessPeb>{{0, 0}},
                       std::vector<DriverProcessPeb>{{4, 0}, {4, 1}},
                       std::vector<DriverProcessPeb>{{4, 0}}}) {
    constexpr uint64_t Limit = 16 * 1024 * 1024;
    auto Memory = llvm::cantFail(AddressSpace::create(
        llvm::cantFail(PhysicalMemory::create(Limit)), Limit));
    DriverResult Result;
    KernelModel Model(*Memory, Result);
    DriverOptions Options;
    Options.ProcessPebs = Entries;
    Options.Contract = Entries.size() == 1 && Entries[0].ProcessID
                           ? ExecutionContract::Legacy
                           : ExecutionContract::CheckedX64;
    auto Error = Model.initialize({}, Options);
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("process_pebs"),
              std::string::npos);
  }
}
} // namespace
} // namespace neverd::emulation
