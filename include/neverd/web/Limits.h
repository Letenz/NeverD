//===- Limits.h - Offline analysis resource limits ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline analysis resource limits.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_LIMITS_H
#define NEVERD_WEB_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::web {

/// Admission limits are charged before allocation and shared by every member
/// in an input tree. Immutable originals use bounded private disk storage;
/// text/AST consumers have separate, smaller materialization budgets.
struct Limits {
  uint64_t MaxInputBytes = 512ULL * 1024 * 1024;
  uint64_t MaxMemberBytes = 256ULL * 1024 * 1024;
  uint64_t MaxEntries = 10000;
  uint64_t MaxDepth = 64;

  static constexpr uint64_t HardInputBytes = 512ULL * 1024 * 1024;
  static constexpr uint64_t HardMemberBytes = 256ULL * 1024 * 1024;
  static constexpr uint64_t HardEntries = 10000;
  static constexpr uint64_t HardDepth = 64;
};

// include/neverd/web/Asar.h
inline constexpr uint64_t MaxAsarHeaderBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxAsarMembers = 10000;
inline constexpr uint64_t MaxAsarDepth = 24;
inline constexpr uint64_t MaxAsarPathBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxAsarPayloadBytes = 512ULL * 1024 * 1024;

// include/neverd/web/Blob.h
inline constexpr uint64_t MaxBlobReadBytes = 8 * 1024 * 1024;

// include/neverd/web/Bun.h
inline constexpr uint32_t MaxBunModules = 4096;
inline constexpr uint32_t MaxBunBuiltins = 4096;
inline constexpr uint64_t MaxBunNameBytes = 1024 * 1024;
inline constexpr uint64_t MaxBunDecodedSourceBytes = 64ULL * 1024 * 1024;

// include/neverd/web/ConfigurationFiles.h
inline constexpr uint64_t MaxConfigurationFileRecords = 1024;
inline constexpr uint64_t MaxConfigurationFileTerms = 4096;
inline constexpr uint64_t MaxConfigurationFileSteps = 2000000;

// include/neverd/web/ConfigurationOrder.h
inline constexpr uint64_t MaxConfigurationOrderRecords = 256;
inline constexpr uint64_t MaxConfigurationOrderTerms = 4096;
inline constexpr uint64_t MaxConfigurationOrderSteps = 2000000;

// include/neverd/web/DataPack.h
inline constexpr uint64_t MaxDataPackBytes = 128ULL * 1024 * 1024;
inline constexpr uint32_t MaxDataPackResources = 65536;
inline constexpr uint32_t MaxCachedDataPacks = 4;
inline constexpr uint32_t MaxCachedDataPackResources = 65536;
inline constexpr uint32_t MaxDataPackPageRecords = 512;
inline constexpr uint32_t MaxDecodedDataPackResources = 16;
inline constexpr uint64_t MaxDataPackDecodedBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxCachedDataPackDecodedBytes = 64 * 1024 * 1024;

// include/neverd/web/DesktopBridge.h
inline constexpr uint64_t MaxDesktopBridgeSteps = 2000000;
inline constexpr uint64_t MaxDesktopBridgeRecords = 1024;

// include/neverd/web/DesktopManifest.h
inline constexpr uint64_t MaxDesktopManifestBytes = 1024 * 1024;

// include/neverd/web/Electron.h
inline constexpr uint64_t MaxElectronRecords = 10000;
inline constexpr uint64_t MaxElectronSteps = 2000000;
inline constexpr uint64_t MaxElectronManifestBytes = 1024 * 1024;

// include/neverd/web/ElectronEntries.h
inline constexpr uint64_t MaxElectronEntrySteps = 4000000;

// include/neverd/web/ElectronIPC.h
inline constexpr uint64_t MaxElectronIPCSources = 16;
inline constexpr uint64_t MaxElectronIPCEndpoints = 10000;
inline constexpr uint64_t MaxElectronIPCUnits = 1024 * 1024;
inline constexpr uint64_t MaxElectronIPCSteps = 2000000;

// include/neverd/web/HTML.h
inline constexpr uint64_t MaxHTMLBytes = 4 * 1024 * 1024;
inline constexpr uint64_t MaxHTMLSteps = 16 * 1024 * 1024;
inline constexpr uint64_t MaxHTMLRecords = 10000;
inline constexpr uint64_t MaxHTMLDecodedBytes = 1024 * 1024;
inline constexpr uint64_t MaxHTMLAttributeBytes = 16384;
inline constexpr uint64_t MaxHTMLPathBytes = 4096;
inline constexpr uint64_t MaxHTMLLinkSteps = 4 * 1024 * 1024;
inline constexpr uint64_t MaxHTMLNamespaceEntries = 10001;
inline constexpr uint64_t MaxHTMLImportMaps = 64;

// include/neverd/web/ImportMap.h
inline constexpr uint64_t MaxImportMapBytes = 1024 * 1024;
inline constexpr uint64_t MaxImportMapRecords = 10000;
inline constexpr uint64_t MaxImportMapURLBytes = 4096;
inline constexpr uint64_t MaxImportMapSteps = 4 * 1024 * 1024;

// include/neverd/web/InterfaceRelations.h
inline constexpr uint64_t MaxInterfaceSourceRelations = 4096;
inline constexpr uint64_t MaxInterfaceRelationSteps = 2000000;

// include/neverd/web/Interfaces.h
inline constexpr uint64_t MaxHARBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxInterfaceRecords = 4096;
inline constexpr uint64_t MaxInterfaceFields = 32768;
inline constexpr uint64_t MaxInterfaceURLBytes = 16384;
inline constexpr uint64_t MaxInterfacePrivateBytes = 2 * 1024 * 1024;
inline constexpr uint64_t MaxInterfaceSteps = 2000000;
inline constexpr uint64_t MaxInterfacePairs = 32768;

// include/neverd/web/NativeInput.h
inline constexpr uint64_t MaxNativeInputBytes = 256ULL * 1024 * 1024;

// include/neverd/web/Nexe.h
inline constexpr uint64_t MaxNexeInputBytes = 256ULL * 1024 * 1024;
inline constexpr uint64_t MaxNexeBootstrapBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxNexeIndexBytes = 1024 * 1024;
inline constexpr uint64_t MaxNexePrivateBytes = 4 * 1024 * 1024;
inline constexpr uint64_t MaxCachedNexePrivateBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxNexeSteps = 2000000;
inline constexpr uint32_t MaxNexeMembers = 4096;
inline constexpr uint32_t MaxCachedNexeExtractions = 4;
inline constexpr uint32_t MaxNexePageRecords = 512;

// include/neverd/web/NodeBehavior.h
inline constexpr uint64_t MaxNodeBehaviorRecords = 4096;
inline constexpr uint64_t MaxNodeBehaviorArguments = 16;
inline constexpr uint64_t MaxNodeBehaviorSteps = 2000000;
inline constexpr uint64_t MaxCachedNodeBehaviors = 4;
inline constexpr uint64_t MaxPackageBehaviorSources = 16;
inline constexpr uint64_t MaxPackageBehaviorBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxPackageBehaviorNodes = 1000000;
inline constexpr uint64_t MaxPackageBehaviorAncestry = 16;
inline constexpr uint64_t MaxPackageBehaviorEntries = 16;
inline constexpr uint64_t MaxPackageBehaviorRecords = 8192;
inline constexpr uint64_t MaxPackageBehaviorSteps = 8000000;
inline constexpr uint64_t MaxCachedPackageBehaviors = 2;

// include/neverd/web/PackageAdvisories.h
inline constexpr uint64_t MaxPackageAdvisoryEntries = 1024;
inline constexpr uint64_t MaxPackageAdvisorySelectors = 8192;
inline constexpr uint64_t MaxPackageAdvisorySeverities = 1024;
inline constexpr uint64_t MaxPackageAdvisoryImports = 16;
inline constexpr uint64_t MaxPackageAdvisoryMatches = 8192;
inline constexpr uint64_t MaxCachedPackageAdvisoryMatches = 2;

// include/neverd/web/PackageArchive.h
inline constexpr uint64_t MaxPackageArchiveBytes = 512ULL * 1024 * 1024;
inline constexpr uint64_t MaxPackageArchiveFileBytes = 256ULL * 1024 * 1024;
inline constexpr uint64_t MaxPackageArchiveMembers = 10000;
inline constexpr uint64_t MaxPackageArchiveMetadata = 8ULL * 1024 * 1024;

// include/neverd/web/PackageAssessment.h
inline constexpr uint64_t MaxPackageAssessmentFindings = 8192;
inline constexpr uint64_t MaxCachedPackageAssessments = 2;
inline constexpr uint64_t MaxPackageAssessmentSteps = 8000000;
inline constexpr uint64_t MaxPackageAssessmentBytes = 16 * 1024 * 1024;
inline constexpr uint64_t MaxPackageReviewDecisions = 256;
inline constexpr uint64_t MaxPackageReviewBytes = 256 * 1024;

// include/neverd/web/PackageAttestations.h
inline constexpr uint64_t MaxPackageAttestationImports = 4;
inline constexpr uint64_t MaxPackageAttestationSubjects = 64;
inline constexpr uint64_t MaxPackageAttestationDigests = 256;
inline constexpr uint64_t MaxPackageAttestationSignatures = 64;
inline constexpr uint64_t MaxPackageAttestationPayloadBytes = 512 * 1024;
inline constexpr uint64_t MaxPackageAttestationSignatureBytes = 16 * 1024;
inline constexpr uint64_t MaxPackageAttestationDigestMatches = 4;

// include/neverd/web/PackageBehaviorDiff.h
inline constexpr uint64_t MaxPackageBehaviorDiffPairs = 32;
inline constexpr uint64_t MaxPackageBehaviorDiffOccurrences = 16384;
inline constexpr uint64_t MaxPackageBehaviorDiffSteps = 8000000;
inline constexpr uint64_t MaxCachedPackageBehaviorDiffs = 2;

// include/neverd/web/PackageEvidence.h
inline constexpr uint64_t MaxPackageEvidenceInputBytes = 1024 * 1024;
inline constexpr uint64_t MaxPackageEvidenceRetainedBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxPackageEvidenceSteps = 8000000;
inline constexpr uint64_t MaxPackageEvidenceDescriptors = 20;

// include/neverd/web/PackageInstall.h
inline constexpr uint64_t MaxPackageInstallScripts = 1024;
inline constexpr uint64_t MaxPackageInstallRequests = 4096;
inline constexpr uint64_t MaxPackageInstallAssociations = 8192;
inline constexpr uint64_t MaxPackageInstallDepth = 16;
inline constexpr uint64_t MaxPackageInstallSteps = 8000000;
inline constexpr uint64_t MaxCachedPackageInstalls = 2;

// include/neverd/web/Packages.h
inline constexpr uint64_t MaxPackageMetadataBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxPackageInstances = 4096;
inline constexpr uint64_t MaxPackageEdges = 32768;
inline constexpr uint64_t MaxPackageRecords = 32768;

// include/neverd/web/PassiveReport.h
inline constexpr uint64_t MaxPassiveReportRecords = 16384;
inline constexpr uint64_t MaxPassiveReportSteps = 8000000;
inline constexpr uint64_t MaxPassiveShapeBytes = 65536;
inline constexpr uint64_t MaxPassiveShapeNodes = 200000;
inline constexpr uint64_t MaxPassiveShapePrivateBytes = 2 * 1024 * 1024;

// include/neverd/web/Pkg.h
inline constexpr uint64_t MaxPkgInputBytes = 256ULL * 1024 * 1024;
inline constexpr uint64_t MaxPkgPreludeBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxPkgIndexBytes = 1024 * 1024;
inline constexpr uint64_t MaxPkgPrivateBytes = 4 * 1024 * 1024;
inline constexpr uint64_t MaxCachedPkgPrivateBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxPkgSteps = 4000000;
inline constexpr uint64_t MaxPkgJSONNodes = 100000;
inline constexpr uint64_t MaxPkgStatBytes = 4096;
inline constexpr uint32_t MaxPkgJSONDepth = 5;
inline constexpr uint32_t MaxPkgPaths = 4096;
inline constexpr uint32_t MaxPkgLinks = 4096;
inline constexpr uint32_t MaxCachedPkgExtractions = 4;
inline constexpr uint32_t MaxPkgPageRecords = 512;

// include/neverd/web/SEA.h
inline constexpr uint32_t MaxSEAAssets = 4096;
inline constexpr uint64_t MaxSEAPrivateBytes = 1024 * 1024;
inline constexpr uint64_t MaxSEANameBytes = 32768;
inline constexpr uint64_t MaxSEAInputBytes = 256ULL * 1024 * 1024;

// include/neverd/web/Source.h
inline constexpr uint64_t MaxJavaScriptBytes = 1024 * 1024;
inline constexpr uint64_t MaxJavaScriptNodes = 100000;
inline constexpr uint64_t MaxJavaScriptStringUnits = 2 * 1024 * 1024;
inline constexpr uint64_t MaxJavaScriptLexemes = 200000;

// include/neverd/web/SourceBindings.h
inline constexpr uint64_t MaxJavaScriptBindingSteps = 2000000;

// include/neverd/web/SourceBundles.h
inline constexpr uint64_t MaxJavaScriptBundleSteps = 4000000;
inline constexpr uint64_t MaxJavaScriptBundles = 64;
inline constexpr uint64_t MaxJavaScriptBundleModules = 4096;
inline constexpr uint64_t MaxJavaScriptBundleDependencies = 50000;
inline constexpr uint64_t MaxJavaScriptBundleNameUnits = 1048576;

// include/neverd/web/SourceEffects.h
inline constexpr uint64_t MaxJavaScriptEffectSteps = 2000000;

// include/neverd/web/SourceFeatures.h
inline constexpr uint64_t MaxJavaScriptClientFeatureSteps = 2000000;
inline constexpr uint64_t MaxJavaScriptClientFeatureRecords = 1024;

// include/neverd/web/SourceMap.h
inline constexpr uint64_t MaxSourceMapBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxSourceMapSegments = 100000;

// include/neverd/web/SourceModules.h
inline constexpr uint64_t MaxJavaScriptModuleSteps = 2000000;
inline constexpr uint64_t MaxJavaScriptModuleNameUnits = 1048576;
inline constexpr uint64_t MaxJavaScriptModulePathUnits = 4096;
inline constexpr uint64_t MaxHTMLModuleLinkSteps = 4 * 1024 * 1024;
inline constexpr uint64_t MaxHTMLModuleRequests = 10000;

// include/neverd/web/SourceNavigation.h
inline constexpr uint64_t MaxJavaScriptNavigationSteps = 2000000;

// include/neverd/web/SourceOrigins.h
inline constexpr uint64_t MaxSourceOriginSteps = 2000000;
inline constexpr uint64_t MaxSourceOriginUnits = 1048576;

// include/neverd/web/SourcePaths.h
inline constexpr uint64_t MaxCapturedPathSteps = 2000000;
inline constexpr uint64_t MaxCapturedPathUnits = 4096;
inline constexpr uint64_t MaxCapturedPathAllocatedUnits = 1048576;

// include/neverd/web/SourceRewrite.h
inline constexpr uint64_t MaxSourceRewriteRecords = 4096;
inline constexpr uint64_t MaxSourceRewriteSteps = 8000000;
inline constexpr uint64_t MaxCachedSourceRewrites = 2;

// include/neverd/web/SourceValues.h
inline constexpr uint64_t MaxJavaScriptValueSteps = 4000000;
inline constexpr uint64_t MaxJavaScriptValueStringUnits = 65536;
inline constexpr uint64_t MaxJavaScriptValueAllocatedUnits = 2097152;
inline constexpr unsigned MaxJavaScriptBigIntBits = 1024;

// include/neverd/web/SourceView.h
inline constexpr uint64_t MaxSourceViewBytes = 4 * 1024 * 1024;
inline constexpr uint64_t MaxSourceViewChunk = 65536;
inline constexpr uint64_t MaxPublishedSourceViews = 2;

// include/neverd/web/Streams.h
inline constexpr uint64_t MaxStreamBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxStreamFragmentBytes = 256 * 1024;
inline constexpr uint64_t MaxStreamRecords = 4096;
inline constexpr uint64_t MaxStreamJSONWork = 200000;
inline constexpr uint64_t MaxStreamPrivateBytes = 1024 * 1024;

// include/neverd/web/Tauri.h
inline constexpr uint64_t MaxTauriInputBytes = 128ULL * 1024 * 1024;
inline constexpr uint32_t MaxCachedTauriExtractions = 2;
inline constexpr uint64_t MaxCachedTauriDecodedBytes = 64ULL * 1024 * 1024;

// include/neverd/web/Wails.h
inline constexpr uint64_t MaxWailsInputBytes = 256ULL * 1024 * 1024;
inline constexpr uint32_t MaxCachedWailsExtractions = 4;
inline constexpr uint64_t MaxCachedWailsPrivateBytes = 8ULL * 1024 * 1024;

} // namespace neverd::web

namespace neverd::limits::web {

// lib/web/BunSourceMap.h
inline constexpr uint64_t kBunSourceMapMaxBunMapSources = 10000;
inline constexpr uint64_t kBunSourceMapMaxBunMapSourceBytes = 4 * 1024 * 1024;
inline constexpr uint64_t kBunSourceMapMaxBunMapDecodedBytes = 8 * 1024 * 1024;

// lib/web/ExportDirectory.h
inline constexpr uint64_t kExportDirectoryMaxOutput = 1024ULL * 1024 * 1024;

// lib/web/HermesModel.h
inline constexpr uint64_t kHermesModelModelLimit = 128 * 1024 * 1024;

// lib/web/RecoveryParser.h
inline constexpr uint64_t kRecoveryParserMaxRecoverySourceBytes =
    32ULL * 1024 * 1024;
inline constexpr uint64_t kRecoveryParserMaxRecoveryNodes = 2000000;
inline constexpr uint64_t kRecoveryParserMaxRecoveryLexemes = 2000000;
inline constexpr uint64_t kRecoveryParserMaxRecoveryStringUnits =
    16ULL * 1024 * 1024;

// lib/web/Session.cpp
inline constexpr uint64_t kSessionMaxCachedSources = 16;
inline constexpr uint64_t kSessionMaxCachedNodes = 200000;

// lib/web/compression/Gzip.cpp
inline constexpr uint64_t kGzipMaxInput = 16 * 1024 * 1024;

// lib/web/nexe/Reader.cpp
inline constexpr uint64_t kReaderSearchWindow = 16000;

// lib/web/packages/advisories/Reader.cpp
inline constexpr size_t kReaderRequiredLimit = 4096;

// lib/web/packages/advisories/SemVer.h
inline constexpr size_t kSemVerMaxSemVerBytes = 1024;

// lib/web/packages/attestations/Internal.h
inline constexpr size_t kInternalTextMaxBytes = 4096;

// lib/web/packages/evidence/Admission.h
inline constexpr size_t kAdmissionTextLimit = 4096;

// lib/web/pkg/Locator.cpp
inline constexpr uint64_t kLocatorLocatorWindow = 512;

// lib/web/Asar.cpp
inline constexpr int kAsarIntegrityBlocksSizeLimit = 10000;
inline constexpr int kAsarDirectoryPathSizeLimit = 4096;

// lib/web/Bun.cpp
inline constexpr int kBunNamePartSizeLimit = 32768;

// lib/web/BunContainer.cpp
inline constexpr int kBunContainerGraphPaddingSizeLimit = 65536;
inline constexpr int kBunContainerMachoCountLimit = 4096;
inline constexpr int kBunContainerMachoSizeLimit = 1024 * 1024;
inline constexpr int kBunContainerMachoSegmentsSizeLimit = 1024;
inline constexpr int kBunContainerPeOptSizeLimit = 4096;
inline constexpr int kBunContainerPeCountLimit = 96;

// lib/web/BunSourceMap.cpp
inline constexpr int kBunSourceMapFrameSizeMaxBlockMinArgument1 = 128 * 1024;
inline constexpr int kBunSourceMapFrameSizeBlocksLimit = 100000;
inline constexpr int kBunSourceMapDecodeBunSourceMapPSizeLimit = 32768;

// lib/web/ElectronSelection.cpp
inline constexpr int kElectronSelectionElectronSourceSelectionIDSizeLimit = 64;

// lib/web/ExportDirectory.h
inline constexpr int kExportDirectoryPathSizeLimit = 32768;
inline constexpr int kExportDirectoryWriteFilesLimit = 40000;

// lib/web/HTML.cpp
inline constexpr int kHTMLTagTNameSizeLimit = 128;
inline constexpr int kHTMLTagAttributeCountLimit = 256;
inline constexpr int kHTMLTagNameSizeLimit = 128;
inline constexpr int kHTMLRunATokenCountLimit = 100000;
inline constexpr int kHTMLRunMathSvgForeignRootsSizeLimit = 64;
inline constexpr int kHTMLRunTemplateDepthLimit = 64;

// lib/web/HermesModel.h
inline constexpr int kHermesModelCollectDepthLimit = 256;

// lib/web/InputRoot.cpp
inline constexpr int kInputRootCapturePathSizeLimit = 32768;

// lib/web/PathPolicy.cpp
inline constexpr int kPathPolicyArchivePathKeyPathSizeLimit = 4096;

// lib/web/Session.cpp
inline constexpr int kMaxSessionPageRows = 512;

// lib/web/SessionDataPackExport.cpp
inline constexpr int
    kSessionDataPackExportSessionExportDataPackEResourceCountLimit = 32768;

// lib/web/SessionInterfaceRelations.cpp
inline constexpr int
    kSessionInterfaceRelationsSessionInterfaceRelationRecordsLimit = 128;

// lib/web/SessionInterfaces.cpp
inline constexpr int kSessionInterfacesPageEndLimit = 128;

// lib/web/SessionMaps.cpp
inline constexpr int kSessionMapsSessionLookupSourceMapAnchorsSizeLimit = 512;

// lib/web/SessionPassiveReport.cpp
inline constexpr int
    kSessionPassiveReportSessionCreatePassiveReportSelectedTextSizeLimit = 64;
inline constexpr int kSessionPassiveReportRecordsLimit = 128;

// lib/web/SessionSEA.cpp
inline constexpr int kSessionSEASessionSeaRecordsLimit = 128;

// lib/web/SessionStreams.cpp
inline constexpr int kSessionStreamsSessionStreamRecordsLimit = 128;

// lib/web/SessionTauri.cpp
inline constexpr int kSessionTauriRecordsLimit = 128;

// lib/web/SessionWails.cpp
inline constexpr int kSessionWailsRecordsLimit = 128;

// lib/web/SourceBindings.cpp
inline constexpr int kSourceBindingsDiagnosticResultDiagnosticsSizeLimit = 32;

// lib/web/SourceBundles.cpp
inline constexpr int kSourceBundlesDiagnosticResultDiagnosticsSizeLimit = 32;

// lib/web/SourceEvaluate.cpp
inline constexpr int kSourceEvaluateBigLiteralTextSizeLimit = 4096;
inline constexpr int kSourceEvaluateStringNumberTextSizeLimit = 4096;

// lib/web/SourceLocation.cpp
inline constexpr int kSourceLocationTextCoordinatesBytesSizeLimit =
    4 * 1024 * 1024;
inline constexpr int kSourceLocationTextCoordinatesLinesSizeLimit = 200000;

// lib/web/SourceMaps.cpp
inline constexpr int kSourceMapsDecodeDepthLimit = 4;
inline constexpr int kSourceMapsDecodeSectionsSizeLimit = 1024;

// lib/web/SourceModules.cpp
inline constexpr int kSourceModulesDiagnosticResultDiagnosticsSizeLimit = 32;

// lib/web/SourceOrigins.cpp
inline constexpr int kSourceOriginsAppendBaseMembersSizeLimit = 32;
inline constexpr int kSourceOriginsPatternDepthLimit = 32;
inline constexpr int kSourceOriginsNodeDepthLimit = 64;

// lib/web/SourcePaths.cpp
inline constexpr int kSourcePathsNodeDepthLimit = 64;
inline constexpr int kSourcePathsRunRequestedSizeLimit = 10000;
inline constexpr int kSourcePathsAnalyzeSourcePathsRequestedSizeLimit = 10000;

// lib/web/SourceRecovery.cpp
inline constexpr int kSourceRecoveryRecoverReadableJavaScriptMinArgument2 = 32;

// lib/web/SourceView.cpp
inline constexpr int kSourceViewPolicyIDRangesSizeLimit = 64;
inline constexpr int kSourceViewPolicyASizeLimit = 64;

// lib/web/archives/PathIndex.cpp
inline constexpr int
    kPathIndexArchivePathIndexAdmitStdCountPathBeginPathEndLimit = 64;

// lib/web/compression/Brotli.cpp
inline constexpr unsigned long long kBrotliDecodeBrotliInputSizeLimit =
    8ULL * 1024 * 1024;
inline constexpr unsigned long long kBrotliDecodeBrotliMaxOutputLimit =
    32ULL * 1024 * 1024;
inline constexpr int kBrotliDecodeBrotliMaxCallsLimit = 65536;
inline constexpr int kBrotliDecodeBrotliInMinArgument1 = 4096;

// lib/web/compression/Gzip.cpp
inline constexpr int kGzipDecodeGzipMaxOutputLimit = 8 * 1024 * 1024;
inline constexpr int kGzipDecodeGzipDeadlineSeconds = 2;

// lib/web/desktop/Manifest.cpp
inline constexpr int kManifestContextManifestContextAIDSizeLimit = 64;
inline constexpr int kManifestContextManifestContextAMemberPathSizeLimit = 4096;
inline constexpr int kManifestContextEntryPathSizeLimit = 4096;

// lib/web/desktop/tauri/Reader.cpp
inline constexpr int kReaderAdmitMappingsMapsSizeLimit = 128;
inline constexpr int kReaderExtractTauriInitInstructionsLimit = 32;
inline constexpr int kReaderExtractTauriKeySizeLimit = 4096;

// lib/web/desktop/wails/BuildInfo.cpp
inline constexpr int kBuildInfoInlineStringSizeLimit = 256 * 1024;

// lib/web/desktop/wails/ELF.cpp
inline constexpr int kELFAdmitELFCountLimit = 1024;

// lib/web/desktop/wails/Mappings.cpp
inline constexpr int kMappingsReaderAdmitMappingsSizeLimit = 128;

// lib/web/desktop/wails/PE.cpp
inline constexpr int kPEAdmitPECountLimit = 96;

// lib/web/desktop/wails/Tables.cpp
inline constexpr int kTablesTableNameSizeLimit = 4096;
inline constexpr int kTablesReadTablesFirstNameSizeLimit = 4096;

// lib/web/interfaces/Endpoint.cpp
inline constexpr int kEndpointInterfaceHeaderKindNameSizeLimit = 64;

// lib/web/interfaces/SourceXHR.cpp
inline constexpr int kSourceXHRHeaderTextSizeLimit = 64;

// lib/web/interfaces/TransformationLinks.cpp
inline constexpr int kTransformationLinksDepthLimit = 64;

// lib/web/interfaces/report/Source.cpp
inline constexpr int kSourceMethodNameSizeLimit = 64;

// lib/web/interfaces/report/SourceStructure.cpp
inline constexpr int
    kSourceStructureRequestStructureValueArrayExpressionObjectExpressionNameSizeLimit =
        64;

// lib/web/packages/PackageIntegrity.cpp
inline constexpr int kPackageIntegrityParseValueSizeLimit = 65536;
inline constexpr int kPackageIntegrityParseTokensLimit = 256;

// lib/web/packages/PackageReader.cpp
inline constexpr int kPackageReaderLegacyDepthLimit = 64;
inline constexpr int kPackageReaderCompareLegacyDepthLimit = 64;

// lib/web/packages/Paths.h
inline constexpr int kPathsConfinedPathSizeLimit = 4096;
inline constexpr int kPathsConfinedDepthLimit = 64;

// lib/web/packages/assessment/Compose.cpp
inline constexpr int kComposerAppendFRelatedIDsSizeLimit = 32;

// lib/web/packages/assessment/Report.cpp
inline constexpr int kReportMarkdownLimit = 512;
inline constexpr int kReportMarkdownTextSizeLimit = 1024 * 1024;

// lib/web/packages/attestations/Statement.cpp
inline constexpr int kStatementReaderSubjectAlgorithmSizeLimit = 64;
inline constexpr int kStatementReaderSubjectDigestSizeLimit = 1024;

// lib/web/packages/evidence/Descriptor.cpp
inline constexpr int kDescriptorPackageEvidenceTimeTextSizeLimit = 128;

// lib/web/rewrite/Export.cpp
inline constexpr int kExportExportPathSizeLimit = 32768;
inline constexpr int kExportWriteCountMinArgument1 = 65536;

// lib/web/sea/ELF.cpp
inline constexpr int kELFLocateELFCountLimit = 1024;
inline constexpr int kELFLocateELFSeenLimit = 8192;

// lib/web/sea/MachO.cpp
inline constexpr int kMachOLocateMachOCountLimit = 4096;
inline constexpr int kMachOLocateMachOSizeLimit = 1024 * 1024;

// lib/web/sea/PE.cpp
inline constexpr int kPELocatePECountLimit = 512;
inline constexpr int kPELocatePEOptionalSizeLimit = 4096;

// lib/web/streams/Records.cpp
inline constexpr int kRecordsIntegralNumberExponentMinArgument1 = 100000;

// lib/web/HTMLReferences.cpp
inline constexpr int kHTMLReferencesDecodeHTMLAttributeEndILimit = 32;

// lib/web/interfaces/report/SourceStructure.cpp
inline constexpr int kMaxRequestStructureDepth = 16;

// lib/web/packages/PackageReader.cpp
inline constexpr int kMaxPackageAnalysisSteps = 2000000;

// lib/web/packages/PackageArchive.cpp
inline constexpr int kPackageArchiveWorkSlack = 1024;

// lib/web/SessionPassiveReport.cpp
inline constexpr int kMaxPassiveReportExportBytes = 16 * 1024 * 1024;

// lib/web/Session.cpp
inline constexpr int kMaxCachedSourceLexemes = 400000;

// lib/web/Session.cpp
inline constexpr int kMaxCachedSourceMapSegments = 200000;

// lib/web/Session.cpp
inline constexpr int kMaxCachedSourceMaps = 4;

// lib/web/platform/WindowsFile.cpp
inline constexpr int kMaxWindowsPathBufferUnits = 65536;

// lib/web/HTML.cpp
inline constexpr int kMaxHTMLTokenBytes = 65536;

// lib/web/sea/PE.cpp
inline constexpr int kMaxPESEAHeaderOffset = 1024 * 1024;

// lib/web/SourceMaps.cpp
inline constexpr int kMaxSourceMapSources = 10000;

// lib/web/SourceMaps.cpp
inline constexpr int kMaxSourceMapNames = 100000;

// lib/web/compression/Gzip.cpp
inline constexpr int kMaxGzipAllocatorBytes = 2 * 1024 * 1024;

// lib/web/desktop/Manifest.cpp
inline constexpr int kMaxDesktopManifestPathBytes = 8 * 1024 * 1024;

// lib/web/desktop/tauri/Reader.cpp
inline constexpr int kMaxTauriInitializerDistance = 65536;

// lib/web/sea/MachO.cpp
inline constexpr int kMaxMachOSEASections = 4096;

// lib/web/sea/PE.cpp
inline constexpr int kMaxPESEAEntries = 4096;

// lib/web/compression/Gzip.cpp
inline constexpr int kMaxGzipInflateCalls = 8192;

// lib/web/BunContainer.cpp
inline constexpr int kMaxBunProgramHeaders = 1024;

// lib/web/BunContainer.cpp
inline constexpr int kMaxBunSections = 4096;

// lib/web/archives/Zip.cpp
inline constexpr int kMaxZipExpansionRatio = 1000;

// lib/web/desktop/wails/Contents.cpp
inline constexpr int kWailsStreamingThresholdBytes = 1024;

// lib/web/SourceLocation.cpp
inline constexpr int kSourceCheckpointByteStride = 128;

// lib/web/SourceModel.cpp
inline constexpr int kMaxSourceModelDepth = 256;

// lib/web/JsonReader.cpp
inline constexpr int kMaxJSONNumberBytes = 128;

// lib/web/desktop/tauri/Reader.cpp
inline constexpr int kMaxTauriInitializerBytes = 256;

} // namespace neverd::limits::web

#endif // NEVERD_WEB_LIMITS_H
