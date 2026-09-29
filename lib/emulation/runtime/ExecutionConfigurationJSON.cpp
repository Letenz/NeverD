//===- ExecutionConfigurationJSON.cpp - CPU query serialization ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ExecutionReport.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <limits>

namespace neverd::emulation {
namespace {
namespace field = execution_report;

llvm::Error invalid(llvm::StringRef Reason, llvm::StringRef Detail = {}) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 llvm::Twine(Reason) + Detail);
}

llvm::Expected<ExecutionPrivilege> parsePrivilege(llvm::StringRef Value) {
#define NEVERD_EXECUTION_PRIVILEGE(Name, Text)                                 \
  if (Value == Text)                                                           \
    return ExecutionPrivilege::Name;
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_PRIVILEGE
  return invalid(field::UnknownPrivilege, Value);
}

llvm::Expected<ExecutionFeature> parseFeature(llvm::StringRef Value) {
#define NEVERD_EXECUTION_FEATURE(Name, Bit, Text)                              \
  if (Value == Text)                                                           \
    return ExecutionFeature::Name;
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_FEATURE
  return invalid(field::UnknownFeature, Value);
}

llvm::json::Array featuresJSON(ExecutionFeature Features) {
  llvm::json::Array Result;
#define NEVERD_EXECUTION_FEATURE(Name, Bit, Text)                              \
  if ((Features & ExecutionFeature::Name) != ExecutionFeature::None)           \
    Result.push_back(Text);
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_FEATURE
  return Result;
}

llvm::json::Object configurationJSON(const ExecutionConfiguration &Config) {
  llvm::json::Object Result{
      {field::Backend, executionBackendName(Config.Backend)},
      {field::Contract, executionContractName(Config.Contract)},
      {field::Architecture, guestArchitectureName(Config.Architecture)},
      {field::RequiredFeatures, featuresJSON(Config.RequiredFeatures)}};
  if (Config.Privilege)
    Result[field::Privilege] = executionPrivilegeName(*Config.Privilege);
  if (Config.VirtualAddressBits)
    Result[field::VirtualAddressBits] = *Config.VirtualAddressBits;
  if (Config.PageSize)
    Result[field::PageSize] = *Config.PageSize;
  return Result;
}

llvm::json::Object capabilitiesJSON(const ExecutionCapabilities &Caps) {
  llvm::json::Array Instructions;
  for (const char *Instruction : Caps.InstructionFamilies)
    Instructions.push_back(Instruction);
  return llvm::json::Object{
      {field::Contract, executionContractName(Caps.Contract)},
      {field::Architecture, guestArchitectureName(Caps.Architecture)},
      {field::Privilege, executionPrivilegeName(Caps.Privilege)},
      {field::AddressModel, executionAddressModelName(Caps.AddressModel)},
      {field::VirtualAddressBits, Caps.VirtualAddressBits},
      {field::PageSize, Caps.PageSize},
      {field::MaxPhysicalBytes, Caps.MaxPhysicalBytes},
      {field::MaxMappedBytes, Caps.MaxMappedBytes},
      {field::Features, featuresJSON(Caps.Features)},
      {field::NativeExecution, Caps.SupportsNativeExecution},
      {field::InstructionAllowlist, Caps.HasInstructionAllowlist},
      {field::InstructionFamilies, std::move(Instructions)},
      {field::MemoryObservation,
       executionMemoryObservationName(Caps.MemoryObservation)},
      {field::ControlPrecision,
       executionControlPrecisionName(Caps.ControlPrecision)},
      {field::HardWallClockBound, Caps.HardWallClockBound}};
}
} // namespace

llvm::Expected<ExecutionConfiguration>
executionConfigurationFromJSON(llvm::StringRef Text) {
  if (Text.size() > field::JSONLimit)
    return invalid(field::ConfigurationTooLarge);
  auto Value = llvm::json::parse(Text);
  if (!Value)
    return Value.takeError();
  const auto *Object = Value->getAsObject();
  if (!Object)
    return invalid(field::ConfigurationObject);
  for (const auto &[Key, Value] : *Object) {
    const llvm::StringRef Name = Key;
#define NEVERD_EXECUTION_CONFIG_FIELD(ID, Text)                                \
  if (Name == Text)                                                            \
    continue;
#include "neverd/emulation/ExecutionReport.def"
#undef NEVERD_EXECUTION_CONFIG_FIELD
    return invalid(field::UnknownField, Name);
  }

  ExecutionConfiguration Config;
  auto StringField = [&](llvm::StringRef Name, auto Parse,
                         auto &Target) -> llvm::Error {
    const auto *V = Object->get(Name);
    if (!V)
      return llvm::Error::success();
    auto Text = V->getAsString();
    if (!Text)
      return invalid(field::FieldType, Name);
    auto Parsed = Parse(*Text);
    if (!Parsed)
      return Parsed.takeError();
    Target = *Parsed;
    return llvm::Error::success();
  };
  if (auto E =
          StringField(field::Backend, parseExecutionBackend, Config.Backend))
    return E;
  if (auto E =
          StringField(field::Contract, parseExecutionContract, Config.Contract))
    return E;
  if (auto E = StringField(field::Architecture, parseGuestArchitecture,
                           Config.Architecture))
    return E;
  if (auto E = StringField(field::Privilege, parsePrivilege, Config.Privilege))
    return E;
  if (const auto *V = Object->get(field::VirtualAddressBits)) {
    auto Bits = V->getAsUINT64();
    if (!Bits || *Bits > std::numeric_limits<unsigned>::max())
      return invalid(field::FieldType, field::VirtualAddressBits);
    Config.VirtualAddressBits = static_cast<unsigned>(*Bits);
  }
  if (const auto *V = Object->get(field::PageSize)) {
    auto Size = V->getAsUINT64();
    if (!Size)
      return invalid(field::FieldType, field::PageSize);
    Config.PageSize = *Size;
  }
  if (const auto *V = Object->get(field::RequiredFeatures)) {
    const auto *Array = V->getAsArray();
    if (!Array)
      return invalid(field::FieldType, field::RequiredFeatures);
    for (const auto &Entry : *Array) {
      auto Name = Entry.getAsString();
      if (!Name)
        return invalid(field::FieldType, field::RequiredFeatures);
      auto Feature = parseFeature(*Name);
      if (!Feature)
        return Feature.takeError();
      if ((Config.RequiredFeatures & *Feature) != ExecutionFeature::None)
        return invalid(field::DuplicateFeature, *Name);
      Config.RequiredFeatures |= *Feature;
    }
  }
  auto Resolved = resolveExecutionConfiguration(Config);
  if (!Resolved)
    return Resolved.takeError();
  return Config;
}

llvm::Expected<std::string>
executionCapabilitiesJSON(const ExecutionConfiguration &Configuration,
                          bool ProbeHost) {
  auto Resolved = resolveExecutionConfiguration(Configuration);
  if (!Resolved)
    return Resolved.takeError();
  auto Build = queryExecutionBackendBuild(Resolved->Configuration.Backend,
                                          Resolved->Configuration.Architecture);
  if (!Build)
    return Build.takeError();
  llvm::json::Object Result{
      {field::SchemaVersion, field::Version},
      {field::RequestedConfiguration, configurationJSON(Configuration)},
      {field::Configuration, configurationJSON(Resolved->Configuration)},
      {field::Capabilities, capabilitiesJSON(Resolved->Capabilities)},
      {field::SelectionReason, Resolved->SelectionReason},
      {field::Build,
       llvm::json::Object{
           {field::Availability, backendAvailabilityName(Build->Availability)},
           {field::Reason, Build->Reason}}},
      {field::Host, nullptr}};
  if (ProbeHost) {
    auto Probe = probeExecutionBackend(Configuration);
    if (!Probe)
      return Probe.takeError();
    Result[field::Host] = llvm::json::Object{
        {field::Availability, backendAvailabilityName(Probe->Availability)},
        {field::Reason, Probe->Reason},
        {field::Scope, field::InitializationScope}};
  }
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(std::move(Result));
  return Text;
}
} // namespace neverd::emulation
