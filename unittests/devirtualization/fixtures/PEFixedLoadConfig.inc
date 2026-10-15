//===- PEFixedLoadConfig.inc - Synthetic PE loader-write fixtures --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original synthetic load-configuration inputs, independent of image contents.
//===----------------------------------------------------------------------===//

using LoadConfig = llvm::object::coff_load_configuration64;
constexpr size_t LoadConfigOffset = RData + 0x80;
constexpr uint32_t LoadConfigRVA = 0x2080;
constexpr uint32_t CookieRVA = 0x2060;
constexpr uint32_t GuardCheckRVA = 0x2068;
constexpr uint32_t GuardDispatchRVA = 0x2070;

std::vector<uint8_t> loadConfigFixture() {
  auto Bytes = fixture();
  directory(Bytes, LOAD_CONFIG_TABLE, LoadConfigRVA, sizeof(LoadConfig));
  put32(Bytes, LoadConfigOffset, sizeof(LoadConfig));
  put64(Bytes, LoadConfigOffset + offsetof(LoadConfig, SecurityCookie),
        Base + CookieRVA);
  put64(Bytes, LoadConfigOffset + offsetof(LoadConfig, GuardCFCheckFunction),
        Base + GuardCheckRVA);
  put64(Bytes, LoadConfigOffset + offsetof(LoadConfig, GuardCFCheckDispatch),
        Base + GuardDispatchRVA);
  put32(Bytes, LoadConfigOffset + offsetof(LoadConfig, GuardFlags),
        uint32_t(GuardFlags::CF_INSTRUMENTED));
  return Bytes;
}

constexpr size_t UnsupportedLoadConfigFields[] = {
    offsetof(LoadConfig, LockPrefixTable),
    offsetof(LoadConfig, EditList),
    offsetof(LoadConfig, DynamicValueRelocTable),
    offsetof(LoadConfig, DynamicValueRelocTableOffset),
    offsetof(LoadConfig, CHPEMetadataPointer),
    offsetof(LoadConfig, HotPatchTableOffset),
    offsetof(LoadConfig, GuardRFFailureRoutineFunctionPointer),
    offsetof(LoadConfig, VolatileMetadataPointer),
    offsetof(LoadConfig, GuardCFFunctionTable),
    offsetof(LoadConfig, GuardEHContinuationTable)};

constexpr uint32_t ConflictingLoaderWriteRVAs[] = {
    LoadConfigRVA, GuardCheckRVA, 0x3000, 0x3030, 0x3060, 0x3080, 0x4000};
