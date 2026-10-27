//===- Session.cpp - Offline analysis session --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline analysis session.
///
//===----------------------------------------------------------------------===//

#include "PathPolicy.h"
#include "RecoveryParser.h"
#include "SessionInternal.h"
#include "compression/Gzip.h"

#include "neverd/web/Limits.h"
#include "neverd/web/Source.h"
#include "neverd/web/SourceRecovery.h"

namespace neverd::web {

namespace {

std::string sourceSummary(const SourceAnalysis &Source, uint64_t Revision,
                          const SourceBindingAnalysis *Bindings = nullptr) {
  llvm::json::Array Diagnostics;
  for (const auto &D : Source.Diagnostics)
    Diagnostics.emplace_back(llvm::json::Object{
        {"code", D.Code},
        {"byte_offset",
         D.ByteOffset < 0 ? llvm::json::Value(nullptr)
                          : llvm::json::Value(std::to_string(D.ByteOffset))}});
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"source_id", Source.ID},
      {"artifact_id", Source.ArtifactID},
      {"blob_sha256", Source.BlobHash},
      {"source_type", Source.SourceType},
      {"parse_status", Source.ParseStatus},
      {"parser_profile", std::string(sourceLimits(Source).Profile)},
      {"node_count", Source.Nodes.size()},
      {"comment_count", Source.CommentCount},
      {"lexeme_status", Source.LexemeStatus},
      {"lexeme_count", Source.Lexemes.size()},
      {"diagnostics", std::move(Diagnostics)},
      {"validation", "parser_checks"},
      {"semantic_analysis", Bindings ? "partial" : "not_analyzed"},
      {"lexical_binding_status", Bindings ? Bindings->Status : "not_analyzed"},
      {"redaction_policy", "metadata-only-v1"}});
}
} // namespace

Session::Session() : State(std::make_unique<Impl>()) {}
Session::~Session() = default;

std::string Session::capabilities() {
  llvm::json::Array Operations{
      "import_preview",      "import_commit",      "metadata",
      "artifacts",           "source_map_analyze", "source_map_sources",
      "source_map_segments", "bun_extract",        "bun_records",
      "bun_export",          "native_open",        "native_metadata",
      "native_analyze"};
  llvm::json::Array Analyses{llvm::json::Object{
      {"kind", "source_map_metadata"},
      {"profile", std::string(SourceMapProfile)},
      {"max_map_bytes", std::to_string(MaxSourceMapBytes)},
      {"max_map_segments", MaxSourceMapSegments},
      {"max_cached_maps", ::neverd::limits::web::kMaxCachedSourceMaps},
      {"max_cached_map_segments",
       ::neverd::limits::web::kMaxCachedSourceMapSegments},
      {"resolves_external_references", false}}};
  Operations.emplace_back("electron_manifest_analyze");
  Operations.emplace_back("desktop_manifest_analyze");
  Operations.emplace_back("desktop_resources_export");
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "desktop_manifest"},
      {"profiles", llvm::json::Array{std::string(NWManifestProfile),
                                     std::string(VSIXManifestProfile),
                                     std::string(DesktopResourceProfile)}},
      {"input_kinds",
       llvm::json::Array{"nwjs", "vsix", "cef-assets", "webview-assets"}},
      {"max_bytes", std::to_string(MaxDesktopManifestBytes)},
      {"max_entries", 641},
      {"max_cached", 16},
      {"framework_verified", false},
      {"runtime_entry_verified", false}});
  Operations.emplace_back("sea_extract");
  Operations.emplace_back("sea_records");
  Operations.emplace_back("datapack_extract");
  Operations.emplace_back("datapack_records");
  if (compression::gzipAvailable())
    Operations.emplace_back("datapack_decode");
  Operations.emplace_back("datapack_export");
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "datapack_extraction"},
      {"profiles", llvm::json::Array{std::string(DataPackProfile),
                                     std::string(WebViewDataPackProfile)}},
      {"max_input_bytes", std::to_string(MaxDataPackBytes)},
      {"max_resources", MaxDataPackResources},
      {"max_cached_extractions", MaxCachedDataPacks},
      {"max_cached_resources", MaxCachedDataPackResources},
      {"max_page_records", MaxDataPackPageRecords},
      {"codec", compression::gzipAvailable()
                    ? llvm::json::Value("gzip-rfc1952-v1")
                    : llvm::json::Value(nullptr)},
      {"max_decoded_bytes", std::to_string(MaxDataPackDecodedBytes)},
      {"max_cached_decoded_bytes",
       std::to_string(MaxCachedDataPackDecodedBytes)},
      {"max_decoded_resources", MaxDecodedDataPackResources},
      {"framework_identity", "not_inferred"},
      {"runtime_resource_binding", "not_checked"},
      {"executes_input", false}});
  if (nexeAvailable()) {
    Operations.emplace_back("nexe_extract");
    Operations.emplace_back("nexe_records");
    Analyses.emplace_back(llvm::json::Object{
        {"kind", "nexe_extraction"},
        {"profile", std::string(NexeProfile)},
        {"max_input_bytes", std::to_string(MaxNexeInputBytes)},
        {"max_bootstrap_bytes", std::to_string(MaxNexeBootstrapBytes)},
        {"max_index_bytes", std::to_string(MaxNexeIndexBytes)},
        {"max_members", MaxNexeMembers},
        {"max_private_bytes", std::to_string(MaxNexePrivateBytes)},
        {"max_cached_private_bytes", std::to_string(MaxCachedNexePrivateBytes)},
        {"max_reader_steps", std::to_string(MaxNexeSteps)},
        {"max_cached_extractions", MaxCachedNexeExtractions},
        {"max_page_records", MaxNexePageRecords},
        {"member_path_policy", std::string(ArchivePathProfile)},
        {"executes_input", false},
        {"authenticates_producer_version", false},
        {"signature_status", "not_checked"},
        {"native_prefix_validation", "not_checked"},
        {"runtime_activation", "not_checked"}});
  }
  if (pkgAvailable()) {
    Operations.emplace_back("pkg_extract");
    Operations.emplace_back("pkg_records");
    Analyses.emplace_back(llvm::json::Object{
        {"kind", "pkg_extraction"},
        {"profile", std::string(PkgProfile)},
        {"max_input_bytes", std::to_string(MaxPkgInputBytes)},
        {"max_prelude_bytes", std::to_string(MaxPkgPreludeBytes)},
        {"max_index_bytes", std::to_string(MaxPkgIndexBytes)},
        {"max_paths", MaxPkgPaths},
        {"max_stores", MaxPkgStores},
        {"max_links", MaxPkgLinks},
        {"max_json_nodes", MaxPkgJSONNodes},
        {"max_stat_bytes", std::to_string(MaxPkgStatBytes)},
        {"max_json_depth", MaxPkgJSONDepth},
        {"max_private_bytes", std::to_string(MaxPkgPrivateBytes)},
        {"max_cached_private_bytes", std::to_string(MaxCachedPkgPrivateBytes)},
        {"max_reader_steps", std::to_string(MaxPkgSteps)},
        {"max_cached_extractions", MaxCachedPkgExtractions},
        {"max_page_records", MaxPkgPageRecords},
        {"member_path_policy", std::string(ArchivePathProfile)},
        {"executes_input", false},
        {"authenticates_producer_version", false},
        {"signature_status", "not_checked"},
        {"native_prefix_validation", "not_checked"},
        {"runtime_activation", "not_checked"},
        {"v8_decoding", "opaque"}});
  }
  if (tauriAvailable()) {
    Operations.emplace_back("tauri_extract");
    Operations.emplace_back("tauri_records");
    Operations.emplace_back("tauri_export");
    const TauriLimits L;
    Analyses.emplace_back(llvm::json::Object{
        {"kind", "tauri_extraction"},
        {"profile", std::string(TauriProfile)},
        {"max_input_bytes", std::to_string(MaxTauriInputBytes)},
        {"max_members", L.Members},
        {"max_stored_bytes", std::to_string(L.StoredBytes)},
        {"max_member_stored_bytes", std::to_string(MaxBlobReadBytes)},
        {"max_decoded_bytes", std::to_string(L.DecodedBytes)},
        {"max_member_decoded_bytes", std::to_string(L.MemberDecodedBytes)},
        {"max_private_bytes", std::to_string(L.PrivateBytes)},
        {"max_reader_steps", std::to_string(L.Steps)},
        {"max_codec_calls", std::to_string(L.CodecCalls)},
        {"max_codec_millis", std::to_string(L.CodecMillis)},
        {"max_codec_memory", std::to_string(L.CodecMemory)},
        {"max_cached_extractions", MaxCachedTauriExtractions},
        {"max_cached_decoded_bytes",
         std::to_string(MaxCachedTauriDecodedBytes)},
        {"max_page_records", 128},
        {"member_path_policy", std::string(ArchivePathProfile)},
        {"initializer_selection", "explicit_native_instruction_address"},
        {"scope", "selected_initializer_table"},
        {"executes_input", false}});
  }
  if (wailsAvailable()) {
    Operations.emplace_back("wails_extract");
    Operations.emplace_back("wails_records");
    Operations.emplace_back("wails_export");
    const WailsLimits Limits;
    llvm::json::Array Profiles;
    for (const auto P : wailsProfiles())
      Profiles.emplace_back(std::string(P));
    Analyses.emplace_back(llvm::json::Object{
        {"kind", "wails_extraction"},
        {"profile", std::string(WailsProfile)},
        {"profiles", std::move(Profiles)},
        {"max_input_bytes", std::to_string(MaxWailsInputBytes)},
        {"max_tables", Limits.Tables},
        {"max_members", Limits.Members},
        {"max_payload_bytes", std::to_string(Limits.PayloadBytes)},
        {"max_private_bytes", std::to_string(Limits.PrivateBytes)},
        {"max_reader_steps", std::to_string(Limits.Steps)},
        {"max_cached_extractions", MaxCachedWailsExtractions},
        {"max_cached_private_bytes",
         std::to_string(MaxCachedWailsPrivateBytes)},
        {"max_page_records", 128},
        {"member_path_policy", std::string(ArchivePathProfile)},
        {"executes_input", false},
        {"authenticates_producer_version", false},
        {"runtime_filesystem_binding", "not_checked"},
        {"native_bridge_analysis", "not_analyzed"}});
  }
  llvm::json::Array SEAProfiles;
  for (const auto &P : seaProfiles())
    SEAProfiles.emplace_back(P);
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "node_sea_extraction"},
      {"profiles", std::move(SEAProfiles)},
      {"max_input_bytes", std::to_string(MaxSEAInputBytes)},
      {"max_assets", MaxSEAAssets},
      {"max_private_name_bytes", std::to_string(MaxSEAPrivateBytes)},
      {"max_cached_extractions", 4},
      {"max_page_records", 128},
      {"executes_input", false},
      {"authenticates_producer_version", false},
      {"runtime_activation", "not_checked"},
      {"v8_decoding", "opaque"}});
  for (const auto *Name : {"stream_preview", "stream_commit", "stream_records"})
    Operations.emplace_back(Name);
  llvm::json::Array StreamProfiles;
  for (const auto Profile : streamProfiles())
    StreamProfiles.emplace_back(std::string(Profile));
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "passive_streams"},
      {"profiles", std::move(StreamProfiles)},
      {"redaction_policy", std::string(StreamRedactionPolicy)},
      {"max_bytes", std::to_string(MaxStreamBytes)},
      {"max_fragment_bytes", MaxStreamFragmentBytes},
      {"max_records", MaxStreamRecords},
      {"max_json_nodes", MaxStreamJSONWork},
      {"max_json_depth", 32},
      {"max_private_id_bytes", MaxStreamPrivateBytes},
      {"max_cached_captures", 4},
      {"max_pending_previews", 1},
      {"explicit_preview_required", true},
      {"protocol_auto_detection", false},
      {"network_access", false},
      {"executes_input", false}});
  for (const auto *Name : {"har_preview", "har_commit", "har_records"})
    Operations.emplace_back(Name);
  for (const auto *Name : {"passive_report_create", "passive_report_records"})
    Operations.emplace_back(Name);
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "passive_report"},
      {"profile", std::string(PassiveReportProfile)},
      {"max_records", MaxPassiveReportRecords},
      {"max_steps", MaxPassiveReportSteps},
      {"max_cached", 2},
      {"max_page_records", 128},
      {"max_metadata_bytes", 16 * 1024 * 1024},
      {"max_json_body_bytes", MaxPassiveShapeBytes},
      {"max_aggregate_body_nodes", MaxPassiveShapeNodes},
      {"max_shape_private_bytes", MaxPassiveShapePrivateBytes},
      {"all_input_values", "withheld"},
      {"server_constraints", "unknown"},
      {"executes_input", false},
      {"network_access", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "har_observations"},
      {"profile", std::string(HARProfile)},
      {"redaction_policy", std::string(InterfaceRedactionPolicy)},
      {"explicit_preview_required", true},
      {"max_bytes", std::to_string(MaxHARBytes)},
      {"max_records", MaxInterfaceRecords},
      {"max_fields", MaxInterfaceFields},
      {"max_url_bytes", MaxInterfaceURLBytes},
      {"max_steps", MaxInterfaceSteps},
      {"max_private_url_bytes", MaxInterfacePrivateBytes},
      {"max_cached", 4},
      {"max_page_records", 128},
      {"network_access", false},
      {"executes_input", false}});
#ifdef NEVERD_ENABLE_WEB_JAVASCRIPT
  for (const auto *Name :
       {"interfaces_analyze", "interface_records", "interfaces_compare",
        "interface_correlation_records", "interface_relations_analyze",
        "interface_relation_records"})
    Operations.emplace_back(Name);
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "source_interfaces"},
      {"profile", std::string(SourceInterfaceProfile)},
      {"correlation_profile", std::string(InterfaceCorrelationProfile)},
      {"max_records", MaxInterfaceRecords},
      {"max_pairs", MaxInterfacePairs},
      {"max_steps", MaxInterfaceSteps},
      {"max_cached", 4},
      {"max_cached_correlations", 4},
      {"intrinsic_verified", false},
      {"fetch_member_identity", "property_spelling_unverified"},
      {"fetch_member_correlation", false},
      {"wrapper_profile", std::string(SourceRequestWrapperProfile)},
      {"wrapper_argument_flow", "single_layer_parameter_forwarding"},
      {"wrapper_correlation", false},
      {"spread_argument_resolution", "not_analyzed"},
      {"websocket_correlation", false},
      {"xhr_correlation", false},
      {"candidate_kinds",
       llvm::json::Array{"fetch", "fetch_member_call", "fetch_wrapper_call",
                         "websocket", "xhr_open", "xhr_set_request_header",
                         "xhr_send", "xhr_event_handler"}}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "source_interface_relations"},
      {"profile", std::string(InterfaceSourceRelationProfile)},
      {"max_records", MaxInterfaceSourceRelations},
      {"max_steps", MaxInterfaceRelationSteps},
      {"max_cached", 4},
      {"max_page_records", 128},
      {"relationship_meaning", "source_syntax_only"},
      {"response_api_identity", "member_spelling_unverified"},
      {"wrapper_argument_flow", "not_analyzed"},
      {"callback_parameter_roles", "not_assigned"},
      {"har_correlation", false},
      {"network_access", false},
      {"executes_input", false}});
#endif
  Operations.emplace_back("package_archive_extract");
  Operations.emplace_back("package_archive_records");
  Operations.emplace_back("package_integrity_verify");
  for (const auto *Name :
       {"package_assessment_create", "package_assessment_findings",
        "package_review_append", "package_review_decisions",
        "package_review_restore", "package_assessment_report"})
    Operations.emplace_back(Name);
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "package_assessments"},
      {"profile", std::string(PackageAssessmentProfile)},
      {"available", true},
      {"max_cached", MaxCachedPackageAssessments},
      {"max_findings", MaxPackageAssessmentFindings},
      {"max_steps", MaxPackageAssessmentSteps},
      {"max_retained_bytes_per_assessment", MaxPackageAssessmentBytes},
      {"max_review_events_per_session", MaxPackageReviewDecisions},
      {"max_review_bytes_per_session", MaxPackageReviewBytes},
      {"review_journal_profile", std::string(PackageReviewJournalProfile)},
      {"review_restore_atomic", true},
      {"max_page_records", 512},
      {"dispositions", llvm::json::Array{"unreviewed", "needs_review",
                                         "accepted_risk", "false_positive"}},
      {"report_formats", llvm::json::Array{"markdown"}},
      {"review_head_required", true},
      {"disposition_source", "caller_assertion"},
      {"safety_verdict", "not_assessed"},
      {"signature_verification", "not_performed"},
      {"installed_tree_verified", false},
      {"runtime_verified", false},
      {"reachability_verified", false},
      {"freshness_verified", false},
      {"fetches_remote_data", false},
      {"executes_input", false}});
  for (const auto *Name :
       {"package_attestation_import", "package_attestation_records",
        "package_attestation_digest_match",
        "package_attestation_digest_result"})
    Operations.emplace_back(Name);
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "package_attestations"},
      {"profile", std::string(PackageAttestationProfile)},
      {"descriptor_profile", std::string(PackageEvidenceProfile)},
      {"available", true},
      {"input_kinds",
       llvm::json::Array{"registry-dist", "intoto-statement", "dsse-intoto"}},
      {"max_input_bytes", MaxPackageEvidenceInputBytes},
      {"max_retained_metadata_bytes", MaxPackageEvidenceRetainedBytes},
      {"max_descriptors", MaxPackageEvidenceDescriptors},
      {"max_imports", MaxPackageAttestationImports},
      {"max_subjects", MaxPackageAttestationSubjects},
      {"max_digests", MaxPackageAttestationDigests},
      {"max_signatures", MaxPackageAttestationSignatures},
      {"max_payload_bytes", MaxPackageAttestationPayloadBytes},
      {"max_signature_bytes", MaxPackageAttestationSignatureBytes},
      {"max_cached_digest_matches", MaxPackageAttestationDigestMatches},
      {"max_steps", MaxPackageEvidenceSteps},
      {"digest_algorithms", llvm::json::Array{"sha256", "sha512"}},
      {"signature_verification", "not_performed"},
      {"publisher_authenticated", false},
      {"trust_chain_verified", false},
      {"transparency_verified", false},
      {"slsa_level_verified", false},
      {"fetches_remote_data", false}});
  for (const auto *Name :
       {"package_advisory_import", "package_advisory_records",
        "package_advisories_match", "package_advisories_match_records"})
    Operations.emplace_back(Name);
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "package_advisories"},
      {"profile", std::string(PackageAdvisoryProfile)},
      {"descriptor_profile", std::string(PackageEvidenceProfile)},
      {"available", true},
      {"max_input_bytes", MaxPackageEvidenceInputBytes},
      {"max_retained_metadata_bytes", MaxPackageEvidenceRetainedBytes},
      {"max_descriptors", MaxPackageEvidenceDescriptors},
      {"max_imports", MaxPackageAdvisoryImports},
      {"max_affected_entries", MaxPackageAdvisoryEntries},
      {"max_selectors", MaxPackageAdvisorySelectors},
      {"max_severities", MaxPackageAdvisorySeverities},
      {"max_records", MaxPackageAdvisoryMatches},
      {"max_cached_matches", MaxCachedPackageAdvisoryMatches},
      {"max_steps", MaxPackageEvidenceSteps},
      {"fetches_remote_data", false},
      {"publisher_authenticated", false},
      {"feed_completeness_verified", false},
      {"freshness_assessed", false},
      {"reachability_verified", false},
      {"exploitability_verified", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "package_integrity"},
      {"profile", std::string(PackageIntegrityProfile)},
      {"available", true},
      {"algorithms", llvm::json::Array{"sha1", "sha256", "sha384", "sha512"}},
      {"byte_domain", "selected_original_artifact"},
      {"max_cached_verifications", 16},
      {"authenticates_publisher", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "package_archive"},
      {"profile", std::string(PackageArchiveProfile)},
      {"zip_profile", std::string(ZipArchiveProfile)},
      {"formats", llvm::json::Array{"tar", "tgz", "zip"}},
      {"tar_profiles",
       llvm::json::Array{std::string(PackageArchiveProfile),
                         std::string(PackageGNUArchiveProfile)}},
      {"available", packageArchiveAvailable()},
      {"gzip_available", packageGzipAvailable()},
      {"zip_deflate_available", packageZipDeflateAvailable()},
      {"zip_max_expansion_ratio", 1000},
      {"max_expanded_bytes", std::to_string(MaxPackageArchiveBytes)},
      {"max_cached_expanded_bytes", std::to_string(MaxPackageArchiveBytes)},
      {"max_file_bytes", std::to_string(MaxPackageArchiveFileBytes)},
      {"max_members", MaxPackageArchiveMembers},
      {"max_cached", 4},
      {"recursive_archives", false},
      {"follows_links", false},
      {"executes_input", false}});
  for (const auto *Name : {"packages_analyze", "package_records",
                           "packages_compare", "package_diff_records"})
    Operations.emplace_back(Name);
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "node_package_metadata"},
      {"profile", std::string(PackageProfile)},
      {"input_kinds", llvm::json::Array{"npm-lock", "package-json"}},
      {"npm_lock_versions", llvm::json::Array{1, 2, 3}},
      {"max_metadata_bytes", std::to_string(MaxPackageMetadataBytes)},
      {"max_instances", MaxPackageInstances},
      {"max_dependencies", MaxPackageEdges},
      {"max_cached", 4},
      {"max_cached_diffs", 4},
      {"dependency_resolution", "supplied_placement_candidates_only"},
      {"verifies_version_satisfaction", false},
      {"integrity_verification", "declarations_only"},
      {"advisory_analysis", "unavailable"},
      {"executes_input", false},
      {"resolves_external_references", false}});
  Operations.emplace_back("html_analyze");
  Operations.emplace_back("html_records");
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "html_script_candidates"},
      {"profile", std::string(HTMLProfile)},
      {"max_bytes", std::to_string(MaxHTMLBytes)},
      {"max_records", MaxHTMLRecords},
      {"max_steps", MaxHTMLSteps},
      {"max_link_steps", MaxHTMLLinkSteps},
      {"max_cached", 4},
      {"record_kinds", llvm::json::Array{"scripts", "bases", "import_maps"}},
      {"tree_construction_verified", false},
      {"runtime_entries_verified", false},
      {"executes_input", false},
      {"resolves_external_references", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "html_import_map_candidates"},
      {"profile", std::string(ImportMapProfile)},
      {"url_parser", "ada-4.0.0-b12a893a45809da8103bb4f1e2f6f5ee13f9100b"},
      {"max_declarations", MaxHTMLImportMaps},
      {"max_total_body_bytes", MaxImportMapBytes},
      {"max_records", MaxImportMapRecords},
      {"max_url_bytes", MaxImportMapURLBytes},
      {"max_steps", MaxImportMapSteps},
      {"activation_verified", false},
      {"integrity_verified", false},
      {"executes_input", false},
      {"resolves_external_references", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "electron_manifest"},
      {"profile", std::string(ElectronManifestProfile)},
      {"max_bytes", std::to_string(MaxElectronManifestBytes)},
      {"max_cached", 16},
      {"framework_verified", false},
      {"runtime_entry_verified", false}});
  if (asarAvailable()) {
    Operations.emplace_back("asar_extract");
    Operations.emplace_back("asar_records");
  }
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "asar_extraction"},
      {"profile", std::string(AsarProfile)},
      {"available", asarAvailable()},
      {"path_policy", std::string(ArchivePathProfile)},
      {"max_header_bytes", std::to_string(MaxAsarHeaderBytes)},
      {"max_members", MaxAsarMembers},
      {"max_depth", MaxAsarDepth},
      {"max_path_bytes", std::to_string(MaxAsarPathBytes)},
      {"max_payload_bytes", std::to_string(MaxAsarPayloadBytes)},
      {"max_cached_extractions", 4},
      {"follows_links", false},
      {"host_companion_discovery", false},
      {"authenticates_producer", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "native_handoff"},
      {"profile", std::string(NativeHandoffProfile)},
      {"max_input_bytes", std::to_string(MaxNativeInputBytes)},
      {"loads_automatically", false},
      {"executes_input", false},
      {"companion_file_discovery", false},
      {"resource_domain", "native_loader_and_pipeline"}});
  llvm::json::Array BunProfiles;
  for (const auto &P : bunProfiles())
    BunProfiles.emplace_back(P);
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "bun_standalone_extraction"},
      {"profile", std::string(BunProfile)},
      {"profiles", std::move(BunProfiles)},
      {"max_modules", MaxBunModules},
      {"max_builtins", MaxBunBuiltins},
      {"max_private_name_bytes", std::to_string(MaxBunNameBytes)},
      {"max_cached_extractions", 4},
      {"record_kinds", llvm::json::Array{"modules", "regions"}},
      {"executes_input", false},
      {"authenticates_producer_version", false},
      {"source_map_decoding",
       bunSourceMapAvailable() ? "available_on_request" : "zstd_unavailable"},
      {"bytecode_decoding", "opaque"}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "bun_local_export"},
      {"profile", "bun-local-evidence-export-v1"},
      {"destination", "new_private_directory"},
      {"filename_policy", "generated_ordinals"},
      {"raw_local_disclosure", true},
      {"executes_input", false},
      {"max_output_bytes", "1073741824"},
      {"max_files", 40000},
      {"max_decoded_source_bytes", std::to_string(MaxBunDecodedSourceBytes)},
#ifdef NEVERD_ENABLE_WEB_JAVASCRIPT
      {"readable_source_available", true},
      {"readable_source_profile", std::string(JavaScriptRecoveryProfile)},
      {"max_recovery_parse_bytes", std::to_string(MaxRecoverySourceBytes)},
#else
      {"readable_source_available", false},
      {"readable_source_profile", nullptr},
      {"max_recovery_parse_bytes", nullptr},
#endif
      {"verification", "all_exported_files_reread_sha256_matches"}});
  if (bunSourceMapAvailable())
    Analyses.emplace_back(llvm::json::Object{
        {"kind", "bun_serialized_source_map"},
        {"profile", std::string(BunSourceMapProfile)},
        {"max_map_bytes", std::to_string(MaxSourceMapBytes)},
        {"max_sources", MaxBunMapSources},
        {"max_source_bytes", std::to_string(MaxBunMapSourceBytes)},
        {"max_decoded_bytes", std::to_string(MaxBunMapDecodedBytes)},
        {"max_segments", MaxSourceMapSegments},
        {"mapping_coverage", "retained_mapped_anchors_only"},
        {"resolves_external_references", false}});
#ifdef NEVERD_ENABLE_WEB_JAVASCRIPT
  Operations.emplace_back("source_analyze");
  Operations.emplace_back("source_nodes");
  Operations.emplace_back("source_readable_export");
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "source_local_readable_export"},
      {"profile", std::string(SourceReadableExportProfile)},
      {"recovery_profile", std::string(JavaScriptRecoveryProfile)},
      {"max_source_bytes", std::to_string(MaxRecoverySourceBytes)},
      {"max_output_bytes", std::to_string(MaxRecoverySourceBytes)},
      {"max_syntax_nodes", MaxRecoveryNodes},
      {"max_lexemes", MaxRecoveryLexemes},
      {"max_string_units", std::to_string(MaxRecoveryStringUnits)},
      {"storage_read_chunk_bytes", BlobTransferBytes},
      {"destination", "new_private_directory"},
      {"filename_policy", "fixed_names"},
      {"raw_local_disclosure", true},
      {"interactive_cache_modified", false},
      {"transformation", "whitespace_insertion_only"},
      {"verification", "same_parser_tree_and_exported_file_sha256"},
      {"semantic_coverage", "not_established"},
      {"executes_input", false}});
  Operations.emplace_back("source_map_lookup");
  Operations.emplace_back("source_bindings_analyze");
  Operations.emplace_back("source_binding_records");
  Operations.emplace_back("source_semantics_analyze");
  Operations.emplace_back("source_semantic_records");
  Operations.emplace_back("source_rewrite_analyze");
  Operations.emplace_back("node_behavior_analyze");
  Operations.emplace_back("node_behavior_records");
  Operations.emplace_back("package_behavior_analyze");
  Operations.emplace_back("package_behavior_records");
  Operations.emplace_back("package_behavior_diff_analyze");
  Operations.emplace_back("package_behavior_diff_records");
  Analyses.emplace_back(
      llvm::json::Object{{"kind", "node_selected_behavior_diff"},
                         {"profile", std::string(PackageBehaviorDiffProfile)},
                         {"max_sources_per_side", MaxPackageBehaviorSources},
                         {"max_records_per_side", MaxPackageBehaviorRecords},
                         {"max_pairs", MaxPackageBehaviorDiffPairs},
                         {"max_occurrences", MaxPackageBehaviorDiffOccurrences},
                         {"max_steps", MaxPackageBehaviorDiffSteps},
                         {"max_cached", MaxCachedPackageBehaviorDiffs},
                         {"shares_session_source_cache", true},
                         {"selection_complete", false},
                         {"call_correspondence_verified", false},
                         {"executes_input", false}});
  Operations.emplace_back("package_install_analyze");
  Operations.emplace_back("package_install_records");
  Analyses.emplace_back(
      llvm::json::Object{{"kind", "node_install_entry_traces"},
                         {"profile", std::string(PackageInstallProfile)},
                         {"max_scripts", MaxPackageInstallScripts},
                         {"max_requests", MaxPackageInstallRequests},
                         {"max_associations", MaxPackageInstallAssociations},
                         {"max_depth", MaxPackageInstallDepth},
                         {"max_steps", MaxPackageInstallSteps},
                         {"max_cached", MaxCachedPackageInstalls},
                         {"max_selected_sources", MaxPackageBehaviorSources},
                         {"max_selected_bytes", MaxPackageBehaviorBytes},
                         {"max_selected_nodes", MaxPackageBehaviorNodes},
                         {"runtime_resolution_verified", false},
                         {"reachability_verified", false},
                         {"executes_input", false}});
  Analyses.emplace_back(
      llvm::json::Object{{"kind", "node_source_behavior"},
                         {"profile", std::string(NodeBehaviorProfile)},
                         {"max_records", MaxNodeBehaviorRecords},
                         {"max_argument_anchors", MaxNodeBehaviorArguments},
                         {"max_steps", MaxNodeBehaviorSteps},
                         {"max_cached_sources", MaxCachedNodeBehaviors},
                         {"max_selected_sources", MaxPackageBehaviorSources},
                         {"max_selected_bytes", MaxPackageBehaviorBytes},
                         {"max_selected_nodes", MaxPackageBehaviorNodes},
                         {"max_ancestry_depth", MaxPackageBehaviorAncestry},
                         {"max_declared_entry_ids", MaxPackageBehaviorEntries},
                         {"max_package_records", MaxPackageBehaviorRecords},
                         {"max_package_join_steps", MaxPackageBehaviorSteps},
                         {"max_cached_packages", MaxCachedPackageBehaviors},
                         {"runtime_api_verified", false},
                         {"reachability_verified", false},
                         {"executes_input", false}});
  Operations.emplace_back("source_rewrite_records");
  Operations.emplace_back("source_rewrite_export");
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_primitive_rewrite"},
      {"profile", std::string(SourceRewriteProfile)},
      {"passes", llvm::json::Array{"string_recovery", "constant_folding"}},
      {"max_source_bytes", std::to_string(MaxSourceRewriteBytes)},
      {"max_candidate_bytes", std::to_string(MaxSourceRewriteBytes)},
      {"max_records", MaxSourceRewriteRecords},
      {"max_steps", MaxSourceRewriteSteps},
      {"max_cached", MaxCachedSourceRewrites},
      {"whole_program_equivalence", false},
      {"source_reflection_preserved", false},
      {"executes_input", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_scope_rename"},
      {"profile", std::string(SourceRenameProfile)},
      {"passes", llvm::json::Array{"scope_renaming"}},
      {"max_source_bytes", std::to_string(MaxSourceRewriteBytes)},
      {"max_candidate_bytes", std::to_string(MaxSourceRewriteBytes)},
      {"max_records", MaxSourceRewriteRecords},
      {"max_steps", MaxSourceRewriteSteps},
      {"max_cached", MaxCachedSourceRewrites},
      {"cache_pool", "source_rewrites"},
      {"original_name_recovery", false},
      {"whole_program_equivalence", false},
      {"source_reflection_preserved", false},
      {"executes_input", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_boolean_branches"},
      {"profile", std::string(SourceBranchProfile)},
      {"passes", llvm::json::Array{"boolean_branches"}},
      {"max_source_bytes", std::to_string(MaxSourceRewriteBytes)},
      {"max_candidate_bytes", std::to_string(MaxSourceRewriteBytes)},
      {"max_records", MaxSourceRewriteRecords},
      {"max_steps", MaxSourceRewriteSteps},
      {"max_cached", MaxCachedSourceRewrites},
      {"cache_pool", "source_rewrites"},
      {"statements_rewritten", false},
      {"whole_program_equivalence", false},
      {"source_reflection_preserved", false},
      {"executes_input", false}});
  Operations.emplace_back("source_modules_analyze");
  Operations.emplace_back("source_module_records");
  Operations.emplace_back("source_bundles_analyze");
  Operations.emplace_back("source_bundle_records");
  Operations.emplace_back("source_view_preview");
  Operations.emplace_back("source_view_commit");
  Operations.emplace_back("source_view_records");
  Operations.emplace_back("source_view_chunk");
  Operations.emplace_back("source_navigation_analyze");
  Operations.emplace_back("electron_source_analyze");
  Operations.emplace_back("electron_source_records");
  Operations.emplace_back("electron_ipc_analyze");
  Operations.emplace_back("electron_ipc_records");
  Operations.emplace_back("electron_entries_analyze");
  Operations.emplace_back("electron_entry_records");
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "electron_entry_candidates"},
      {"profile", std::string(ElectronEntryProfile)},
      {"path_profile", std::string(CapturedPathProfile)},
      {"max_sources", MaxElectronIPCSources},
      {"max_records", MaxElectronRecords},
      {"max_steps", MaxElectronEntrySteps},
      {"max_path_units", MaxCapturedPathUnits},
      {"max_cached", 4},
      {"record_kinds", llvm::json::Array{"sources", "entries"}},
      {"runtime_entries_verified", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "electron_scoped_channels"},
      {"profile", std::string(ElectronIPCProfile)},
      {"max_sources", MaxElectronIPCSources},
      {"max_records", MaxElectronIPCEndpoints},
      {"max_steps", MaxElectronIPCSteps},
      {"max_channel_units", MaxElectronIPCUnits},
      {"max_cached", 4},
      {"record_kinds", llvm::json::Array{"sources", "channels", "endpoints"}},
      {"runtime_routing_verified", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "electron_source_boundaries"},
      {"profile", std::string(ElectronSourceProfile)},
      {"origin_profile", std::string(JavaScriptOriginProfile)},
      {"max_records", MaxElectronRecords},
      {"max_steps", MaxElectronSteps},
      {"runtime_targets_verified", false}});
  Operations.emplace_back("source_navigation_records");
  Operations.emplace_back("source_anchor");
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_source_navigation"},
      {"profile", std::string(JavaScriptNavigationProfile)},
      {"anchor_profile", std::string(SourceAnchorProfile)},
      {"max_steps", MaxJavaScriptNavigationSteps},
      {"record_kinds",
       llvm::json::Array{"functions", "calls", "references", "features",
                         "desktop_bridges", "config_merges", "config_files"}},
      {"client_feature_profile", std::string(JavaScriptClientFeatureProfile)},
      {"desktop_bridge_profile", std::string(DesktopBridgeProfile)},
      {"max_desktop_bridge_records", MaxDesktopBridgeRecords},
      {"max_desktop_bridge_steps", MaxDesktopBridgeSteps},
      {"configuration_order_profile", std::string(ConfigurationOrderProfile)},
      {"configuration_files_profile", std::string(ConfigurationFilesProfile)},
      {"max_configuration_file_records", MaxConfigurationFileRecords},
      {"max_configuration_file_terms", MaxConfigurationFileTerms},
      {"max_configuration_file_steps", MaxConfigurationFileSteps},
      {"max_configuration_order_records", MaxConfigurationOrderRecords},
      {"max_configuration_order_terms", MaxConfigurationOrderTerms},
      {"max_configuration_order_steps", MaxConfigurationOrderSteps},
      {"max_client_feature_records", MaxJavaScriptClientFeatureRecords},
      {"max_client_feature_steps", MaxJavaScriptClientFeatureSteps},
      {"client_feature_attribution", "source_syntax_only"},
      {"runtime_call_graph", false},
      {"resolves_runtime_values", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_source_view"},
      {"profile", std::string(SourceViewProfile)},
      {"lexeme_profile", std::string(JavaScriptLexemeProfile)},
      {"max_view_bytes", std::to_string(MaxSourceViewBytes)},
      {"max_view_segments", MaxSourceViewSegments},
      {"max_chunk_bytes", MaxSourceViewChunk},
      {"max_published_views", MaxPublishedSourceViews},
      {"max_pending_views", 1},
      {"max_reviewed_ranges", 64},
      {"default_policy", "structural-with-reviewed-ranges-v1"},
      {"semantic_rewrite", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_syntax_inventory"},
      {"profile", std::string(JavaScriptParserProfile)},
      {"source_types", llvm::json::Array{"script", "module", "commonjs"}},
      {"max_source_bytes", std::to_string(MaxJavaScriptBytes)},
      {"max_source_nodes", MaxJavaScriptNodes},
      {"max_source_lexemes", MaxJavaScriptLexemes},
      {"max_cached_lexemes", ::neverd::limits::web::kMaxCachedSourceLexemes},
      {"max_decoded_string_units", MaxJavaScriptStringUnits},
      {"max_cached_sources", ::neverd::limits::web::kSessionMaxCachedSources},
      {"max_cached_nodes", ::neverd::limits::web::kSessionMaxCachedNodes},
      {"executes_input", false},
      {"semantic_validation_complete", false}});
  Operations.emplace_back("source_analyze_large");
  const auto LargeLimits = sourceLimits(SourceAnalysisMode::Large);
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_large_source_inventory"},
      {"profile", std::string(LargeLimits.Profile)},
      {"max_source_bytes", std::to_string(LargeLimits.Bytes)},
      {"max_source_nodes", LargeLimits.Nodes},
      {"max_source_lexemes", LargeLimits.Lexemes},
      {"max_decoded_string_units", LargeLimits.StringUnits},
      {"max_source_model_bytes", std::to_string(LargeLimits.ModelBytes)},
      {"max_parser_allocation_bytes", std::to_string(LargeLimits.ParserBytes)},
      {"max_parser_allocations", LargeLimits.ParserAllocations},
      {"max_parser_seconds", LargeLimits.ParserSeconds},
      {"analysis_work_multiplier", LargeLimits.WorkMultiplier},
      {"max_cached_sources", 1},
      {"automatic_fallback", false},
      {"executes_input", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_module_evidence"},
      {"profile", std::string(JavaScriptModuleProfile)},
      {"link_profile", std::string(JavaScriptModuleLinkProfile)},
      {"html_link_profile", std::string(HTMLModuleLinkProfile)},
      {"max_html_link_steps", MaxHTMLModuleLinkSteps},
      {"max_html_module_requests", MaxHTMLModuleRequests},
      {"max_steps", MaxJavaScriptModuleSteps},
      {"max_link_steps", MaxJavaScriptModuleSteps},
      {"max_private_name_units", MaxJavaScriptModuleNameUnits},
      {"max_relative_path_units", MaxJavaScriptModulePathUnits},
      {"max_retained_diagnostics", 32},
      {"record_kinds",
       llvm::json::Array{"requests", "imports", "exports", "attributes"}},
      {"resolves_runtime_modules", false},
      {"executes_input", false},
      {"semantic_validation_complete", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_bundle_partitions"},
      {"profile", std::string(JavaScriptBundleProfile)},
      {"max_steps", MaxJavaScriptBundleSteps},
      {"max_bundles", MaxJavaScriptBundles},
      {"max_modules", MaxJavaScriptBundleModules},
      {"max_dependencies", MaxJavaScriptBundleDependencies},
      {"max_private_key_units", MaxJavaScriptBundleNameUnits},
      {"record_kinds",
       llvm::json::Array{"bundles", "modules", "dependencies", "regions"}},
      {"executes_input", false},
      {"authenticates_producer", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_lexical_bindings"},
      {"profile", std::string(JavaScriptBindingProfile)},
      {"max_steps", MaxJavaScriptBindingSteps},
      {"record_kinds",
       llvm::json::Array{"scopes", "bindings", "declarations", "references"}},
      {"resolves_runtime_values", false},
      {"semantic_validation_complete", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_primitive_values_and_effects"},
      {"value_profile", std::string(JavaScriptValueProfile)},
      {"effect_profile", std::string(JavaScriptEffectProfile)},
      {"max_value_steps", MaxJavaScriptValueSteps},
      {"max_effect_steps", MaxJavaScriptEffectSteps},
      {"max_value_string_units", MaxJavaScriptValueStringUnits},
      {"max_value_allocated_string_units", MaxJavaScriptValueAllocatedUnits},
      {"max_bigint_magnitude_bits", MaxJavaScriptBigIntBits},
      {"executes_input", false},
      {"resolves_runtime_bindings", false},
      {"authorizes_source_rewrites", false},
      {"semantic_validation_complete", false}});
#endif
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"identity_profile", "web-identity-v1"},
      {"redaction_policy", "metadata-only-v1"},
#ifdef _WIN32
      {"input_reader", "windows-ntfs-handle-v1"},
      {"artifact_storage", "windows-private-unlinked-spool-v1"},
#else
      {"input_reader", "posix-descriptor-v1"},
      {"artifact_storage", "posix-unlinked-spool-v1"},
#endif
      {"operations", std::move(Operations)},
      {"analysis", std::move(Analyses)},
      {"limits",
       llvm::json::Object{
           {"max_input_bytes", std::to_string(Limits::HardInputBytes)},
           {"max_member_bytes", std::to_string(Limits::HardMemberBytes)},
           {"max_entries", Limits::HardEntries},
           {"max_depth", Limits::HardDepth},
           {"max_blob_read_bytes", std::to_string(MaxBlobReadBytes)},
           {"capture_buffer_bytes", std::to_string(BlobTransferBytes)},
           {"max_session_spool_bytes",
            std::to_string(2 * Limits::HardInputBytes +
                           MaxPackageArchiveBytes)},
           {"max_page_entries", 512}}}});
}

std::string Session::preview(std::string_view Path, std::string_view Options) {
  std::lock_guard Lock(State->Mutex);
  // A failed replacement invalidates the older approval token, but leaves
  // the published snapshot intact.
  State->Token.clear();
  State->CandidateID.clear();
  auto Budget = parseLimits(Options);
  auto Candidate = capture(Path, Budget);
  const auto Sequence = std::to_string(++State->PreviewSequence);
  const auto Config = limitsIdentity(Budget);
  const auto Token =
      identity("import-preview", {Candidate.ID, Config, Sequence});
  auto Reply = json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"preview_token", Token},
      {"snapshot_id", Candidate.ID},
      {"artifact_count", Candidate.Artifacts.size()},
      {"input_bytes", std::to_string(Candidate.InputBytes)},
      {"redaction_policy", "metadata-only-v1"},
      {"exposed_fields", llvm::json::Array{"identities", "hashes", "sizes",
                                           "kinds", "parent_id"}},
      {"source_text_exposed", false},
      {"input_names_exposed", false}});
  State->Budget = Budget;
  State->Path = Path;
  // Preview retains the digest only. Commit captures again and compares it;
  // retaining a second set of bytes here would spend disk without evidence.
  State->CandidateID = Candidate.ID;
  State->Token = Token;
  return Reply;
}

std::string Session::commit(std::string_view Token) {
  std::lock_guard Lock(State->Mutex);
  if (State->Token.empty() || State->Token != Token)
    throw Error("stale_preview");
  // Consume on any attempted commit, including an input change or read error.
  State->Token.clear();
  const auto CandidateID = std::move(State->CandidateID);
  State->CandidateID.clear();
  Snapshot Current = capture(State->Path, State->Budget);
  if (Current.ID != CandidateID) {
    throw Error("input_changed");
  }
  const auto Revision = State->Revision + 1;
  auto Reply = json(
      llvm::json::Object{{"schema_version", 1},
                         {"status", "ok"},
                         {"project_id", Current.ID},
                         {"revision", std::to_string(Revision)},
                         {"artifact_count", Current.Artifacts.size()},
                         {"input_bytes", std::to_string(Current.InputBytes)},
                         {"analysis_status", "not_analyzed"},
                         {"redaction_policy", "metadata-only-v1"}});
  State->Published = std::move(Current);
  State->Sources.clear();
  State->LargeSourceID.clear();
  State->Bindings.clear();
  State->Navigation.clear();
  State->Semantics.clear();
  State->Modules.clear();
  State->Bundles.clear();
  State->BunExtractions.clear();
  State->SEAExtractions.clear();
  State->NexeExtractions.clear();
  State->PkgExtractions.clear();
  State->DataPacks.clear();
  State->DecodedDataPackResources.clear();
  State->WailsExtractions.clear();
  State->TauriExtractions.clear();
  State->AsarExtractions.clear();
  State->ElectronManifests.clear();
  State->DesktopManifests.clear();
  State->ElectronSources.clear();
  State->ElectronIPCs.clear();
  State->ElectronEntryAnalyses.clear();
  State->PackageAnalyses.clear();
  State->PackageNamespaces.clear();
  State->PackageInstalls.clear();
  State->NodeBehaviors.clear();
  State->PackageBehaviors.clear();
  State->PackageBehaviorDiffs.clear();
  State->PackageArchives.clear();
  State->PackageIntegrity.clear();
  State->PackageAdvisoryMatchResults.clear();
  State->PackageAdvisories.clear();
  State->PackageAttestationDigests.clear();
  State->PackageAttestations.clear();
  State->PackageEvidenceDescriptors.clear();
  State->PackageEvidenceBytes = 0;
  State->PackageAssessments.clear();
  State->PackageReviews.clear();
  State->PackageReviewEvents = 0;
  State->PackageReviewBytes = 0;
  State->HARCaptures.clear();
  State->StreamCaptures.clear();
  State->PendingStream.reset();
  State->StreamPreviewToken.clear();
  State->PendingHAR.reset();
  State->HARPreviewToken.clear();
  State->InterfaceSources.clear();
  State->InterfaceRelations.clear();
  State->InterfaceCorrelations.clear();
  State->PassiveReports.clear();
  State->CachedArchiveBytes = 0;
  State->PackageDiffs.clear();
  State->HTMLDocuments.clear();
  State->CachedNodes = 0;
  State->CachedLexemes = 0;
  State->Maps.clear();
  State->CachedMapSegments = 0;
  State->SourceViews.clear();
  State->SourceRewrites.clear();
  State->PendingSourceView.reset();
  State->SourceViewToken.clear();
  State->Revision = Revision;
  return Reply;
}

std::string Session::metadata() const {
  std::lock_guard Lock(State->Mutex);
  if (!State->Revision)
    throw Error("no_project");
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"project_id", State->Published.ID},
      {"revision", std::to_string(State->Revision)},
      {"artifact_count", State->Published.Artifacts.size()},
      {"input_bytes", std::to_string(State->Published.InputBytes)},
      {"inventory_complete", true},
      {"analysis_status", State->analysisStatus()},
      {"source_inventory_count", State->Sources.size()},
      {"source_binding_analysis_count", State->Bindings.size()},
      {"source_navigation_count", State->Navigation.size()},
      {"source_semantic_analysis_count", State->Semantics.size()},
      {"source_module_analysis_count", State->Modules.size()},
      {"source_bundle_analysis_count", State->Bundles.size()},
      {"bun_extraction_count", State->BunExtractions.size()},
      {"sea_extraction_count", State->SEAExtractions.size()},
      {"nexe_extraction_count", State->NexeExtractions.size()},
      {"pkg_extraction_count", State->PkgExtractions.size()},
      {"wails_extraction_count", State->WailsExtractions.size()},
      {"tauri_extraction_count", State->TauriExtractions.size()},
      {"asar_extraction_count", State->AsarExtractions.size()},
      {"electron_manifest_count", State->ElectronManifests.size()},
      {"desktop_manifest_count", State->DesktopManifests.size()},
      {"datapack_count", State->DataPacks.size()},
      {"decoded_datapack_resource_count",
       State->DecodedDataPackResources.size()},
      {"electron_source_count", State->ElectronSources.size()},
      {"electron_ipc_count", State->ElectronIPCs.size()},
      {"electron_entries_count", State->ElectronEntryAnalyses.size()},
      {"source_map_count", State->Maps.size()},
      {"source_view_count", State->SourceViews.size()},
      {"source_rewrite_count", State->SourceRewrites.size()},
      {"node_behavior_count", State->NodeBehaviors.size()},
      {"package_behavior_count", State->PackageBehaviors.size()},
      {"package_behavior_diff_count", State->PackageBehaviorDiffs.size()},
      {"package_install_count", State->PackageInstalls.size()},
      {"package_assessment_count", State->PackageAssessments.size()},
      {"package_review_event_count", State->PackageReviewEvents},
      {"package_review_retained_bytes", State->PackageReviewBytes},
      {"package_advisory_count", State->PackageAdvisories.size()},
      {"package_attestation_count", State->PackageAttestations.size()},
      {"package_attestation_digest_match_count",
       State->PackageAttestationDigests.size()},
      {"package_advisory_matches_count",
       State->PackageAdvisoryMatchResults.size()},
      {"package_evidence_descriptor_count",
       State->PackageEvidenceDescriptors.size()},
      {"package_evidence_retained_bytes", State->PackageEvidenceBytes},
      {"har_capture_count", State->HARCaptures.size()},
      {"stream_capture_count", State->StreamCaptures.size()},
      {"interface_analysis_count", State->InterfaceSources.size()},
      {"interface_relation_analysis_count", State->InterfaceRelations.size()},
      {"interface_correlation_count", State->InterfaceCorrelations.size()},
      {"redaction_policy", "metadata-only-v1"}});
}

std::string Session::artifacts(std::string_view ExpectedRevision,
                               uint64_t Offset, uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  if (!Limit || Limit > ::neverd::limits::web::kMaxSessionPageRows ||
      Offset > State->Published.Artifacts.size())
    throw Error("invalid_page");
  const auto End =
      std::min<uint64_t>(State->Published.Artifacts.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &A = State->Published.Artifacts[I];
    Items.emplace_back(llvm::json::Object{
        {"artifact_id", A.ID},
        {"parent_id", A.ParentID},
        {"blob_sha256", A.Directory ? llvm::json::Value(nullptr)
                                    : llvm::json::Value(A.BlobHash)},
        {"size", std::to_string(A.Content.size())},
        {"kind", A.Kind},
        {"name_redacted", true}});
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"project_id", State->Published.ID},
      {"revision", std::to_string(State->Revision)},
      {"items", std::move(Items)},
      {"offset", Offset},
      {"page_complete", End == State->Published.Artifacts.size()},
      {"next_offset", End == State->Published.Artifacts.size()
                          ? llvm::json::Value(nullptr)
                          : llvm::json::Value(End)},
      {"inventory_complete", true},
      {"analysis_status", State->analysisStatus()},
      {"redaction_policy", "metadata-only-v1"}});
}

std::string Session::analyzeSource(std::string_view ExpectedRevision,
                                   std::string_view ArtifactID,
                                   std::string_view SourceType) {
  return analyzeSource(ExpectedRevision, ArtifactID, SourceType,
                       SourceAnalysisMode::Interactive);
}

std::string Session::analyzeSource(std::string_view ExpectedRevision,
                                   std::string_view ArtifactID,
                                   std::string_view SourceType,
                                   SourceAnalysisMode Mode) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  if (Mode != SourceAnalysisMode::Interactive &&
      Mode != SourceAnalysisMode::Large)
    throw Error("unsupported_source_analysis_mode");
  if (SourceType != "script" && SourceType != "module" &&
      SourceType != "commonjs")
    throw Error("unsupported_source_type");
  for (const auto &[ID, Source] : State->Sources)
    if (Source.ArtifactID == ArtifactID && Source.SourceType == SourceType &&
        Source.Mode == Mode) {
      const auto Bindings = State->Bindings.find(ID);
      return sourceSummary(
          Source, State->Revision,
          Bindings == State->Bindings.end() ? nullptr : &Bindings->second);
    }
  const auto Limits = sourceLimits(Mode);
  const auto Bytes = State->sourceBytes(ArtifactID, Limits.Bytes,
                                        "source_byte_budget_exceeded");
  // Reserve room for a complete maximum-size result before calling a parser.
  // Concurrent jobs and repeated queries cannot escape the session budget.
  const bool Large = Mode == SourceAnalysisMode::Large;
  if (Large && !State->LargeSourceID.empty())
    throw Error("large_source_cache_budget_exceeded");
  if (!Large &&
      (State->Sources.size() - !State->LargeSourceID.empty() >=
           ::neverd::limits::web::kSessionMaxCachedSources ||
       State->CachedNodes >
           ::neverd::limits::web::kSessionMaxCachedNodes - MaxJavaScriptNodes ||
       State->CachedLexemes > ::neverd::limits::web::kMaxCachedSourceLexemes -
                                  MaxJavaScriptLexemes))
    throw Error("source_cache_budget_exceeded");
  auto Source = Large ? inspectLargeJavaScript(ArtifactID, Bytes, SourceType)
                      : inspectJavaScript(ArtifactID, Bytes, SourceType);
  auto Reply = sourceSummary(Source, State->Revision);
  const auto Count = Source.Nodes.size();
  const auto Lexemes = Source.Lexemes.size();
  auto ID = Source.ID;
  State->Sources.emplace(ID, std::move(Source));
  if (Large) {
    State->LargeSourceID = std::move(ID);
  } else {
    State->CachedNodes += Count;
    State->CachedLexemes += Lexemes;
  }
  return Reply;
#endif
}

std::string Session::sourceNodes(std::string_view ExpectedRevision,
                                 std::string_view SourceID, uint64_t Offset,
                                 uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  const auto &Source = Found->second;
  if (!Limit || Limit > ::neverd::limits::web::kMaxSessionPageRows ||
      Offset > Source.Nodes.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Source.Nodes.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &Node = Source.Nodes[I];
    Items.emplace_back(
        llvm::json::Object{{"node_id", Node.ID},
                           {"parent_id", Node.ParentID},
                           {"kind", Node.Kind},
                           {"start_byte", std::to_string(Node.Start)},
                           {"end_byte", std::to_string(Node.End)}});
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"source_id", Source.ID},
      {"items", std::move(Items)},
      {"offset", Offset},
      {"page_complete", End == Source.Nodes.size()},
      {"next_offset", End == Source.Nodes.size() ? llvm::json::Value(nullptr)
                                                 : llvm::json::Value(End)},
      {"parse_status", Source.ParseStatus},
      {"semantic_analysis",
       State->Bindings.count(Source.ID) ? "partial" : "not_analyzed"},
      {"redaction_policy", "metadata-only-v1"}});
#endif
}

} // namespace neverd::web
