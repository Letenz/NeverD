//===- ComparisonService.cpp - Independent binary comparison jobs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owns two read-only inputs and a C comparison handle. The transport supplies
/// a cancellation probe; a joined monitor forwards it without touching
/// sessions.
//===----------------------------------------------------------------------===//

#include "ComparisonService.h"

#include "EngineSymbols.h"
#include "GraphSnapshot.h"

#include "neverd/diff/Limits.h"
#include "neverd/sdk/NeverDCAPIDiff.h"
#include "neverd/sdk/NeverDCAPISession.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <mutex>
#include <thread>

namespace neverd::worker {
namespace {
namespace limits = ::neverd::limits::diff;
namespace fs = std::filesystem;

struct API {
  decltype(&neverd_session_load_cancellable) LoadCancellable =
      engineSymbol<decltype(LoadCancellable)>(
          "neverd_session_load_cancellable");
#define ENTRY(Name)                                                            \
  decltype(&neverd_comparison_##Name) Name =                                   \
      engineSymbol<decltype(Name)>("neverd_comparison_" #Name)
  ENTRY(create);
  ENTRY(destroy);
  ENTRY(run_owned);
  ENTRY(cancel);
  ENTRY(last_error);
  ENTRY(status_json);
  ENTRY(summary_json);
  ENTRY(export_json);
  ENTRY(restore_json);
  ENTRY(functions_json);
  ENTRY(function_json);
  ENTRY(function_page_json);
  ENTRY(graph_json);
  ENTRY(regions_json);
  ENTRY(region_page_json);
  ENTRY(flow_regions_json);
  ENTRY(dataflow_json);
  ENTRY(slice_json);
  ENTRY(impact_json);
  ENTRY(observations_json);
  ENTRY(owned_scalar_validation_json);
  ENTRY(reviews_json);
  ENTRY(review_record_json);
  ENTRY(override_json);
  ENTRY(undo);
  ENTRY(redo);
  ENTRY(validate_inputs);
#undef ENTRY
  explicit operator bool() const {
    return create && destroy && run_owned && cancel && last_error &&
           status_json && summary_json && export_json && restore_json &&
           functions_json && function_json && function_page_json &&
           graph_json && override_json && undo && redo && validate_inputs &&
           regions_json && region_page_json && flow_regions_json &&
           dataflow_json && slice_json && impact_json && observations_json &&
           reviews_json && review_record_json && owned_scalar_validation_json;
  }
};
const API &api() {
  static const API Functions;
  return Functions;
}
std::string take(const char *Text) {
  const std::unique_ptr<const char, decltype(&neverd_free_string)> Owned(
      Text, neverd_free_string);
  return Text ? Text : "";
}
fs::path path(const std::string &Text) {
  return fs::u8path(Text.begin(), Text.end());
}
std::string pathField(const Json &Value) {
  const auto Text =
      stringField(Value, "path", {},
                  ::neverd::limits::worker::
                      kEngineExecuteImplIdentifyPathPathStringFieldMax);
  if (Text.empty())
    throw Error("invalid_request",
                "A comparison input or output path is required");
  return Text;
}
void fields(const Json &Value,
            std::initializer_list<std::string_view> Allowed) {
  if (!Value.is_object())
    throw Error("invalid_request", "Comparison payload must be an object");
  for (const auto &[Key, V] : Value.items()) {
    (void)V;
    if (std::find(Allowed.begin(), Allowed.end(), Key) == Allowed.end())
      throw Error("invalid_request", "Unknown comparison field: " + Key);
  }
}
using Session = std::unique_ptr<void, decltype(&neverd_session_destroy)>;
Session load(const Json &Input, const char *Side,
             const ComparisonService::Progress &Notify,
             const ComparisonService::Cancellation &Cancelled) {
  fields(Input, {"path", "load_options"});
  if (Cancelled && Cancelled())
    throw Error("cancelled", "Comparison stopped before loading an input");
  Session S(neverd_session_create(), neverd_session_destroy);
  if (!S)
    throw Error("comparison_load_failed", "Cannot create comparison session");
  const auto Options = Input.value("load_options", Json::object());
  if (!Options.is_object())
    throw Error("invalid_request", "load_options must be an object");
  if (!Options.empty() &&
      neverd_session_set_load_options(S.get(), Options.dump().c_str()) < 0)
    throw Error("comparison_load_failed", take(neverd_last_error(S.get())));
  const auto Name = pathField(Input);
  const std::string Phase = std::string(Side) + ":load";
  if (Notify)
    Notify(Phase.c_str(), 0, 1, Name.c_str());
  struct LoadContext {
    const ComparisonService::Progress &Notify;
    const ComparisonService::Cancellation &Cancelled;
    const std::string &Phase;
    std::exception_ptr Failure;
    std::mutex FailureMutex;
    void rememberFailure() {
      std::lock_guard Lock(FailureMutex);
      if (!Failure)
        Failure = std::current_exception();
    }
    bool failed() {
      std::lock_guard Lock(FailureMutex);
      return bool(Failure);
    }
    static void progress(void *User, const char *Step, unsigned long long Done,
                         unsigned long long Total,
                         const char *Detail) noexcept {
      auto &Context = *static_cast<LoadContext *>(User);
      try {
        if (Context.Notify)
          Context.Notify((Context.Phase + ":" + Step).c_str(), Done, Total,
                         Detail);
      } catch (...) {
        Context.rememberFailure();
      }
    }
    static int stop(void *User) noexcept {
      auto &Context = *static_cast<LoadContext *>(User);
      try {
        return Context.failed() || (Context.Cancelled && Context.Cancelled());
      } catch (...) {
        Context.rememberFailure();
        return 1;
      }
    }
  } Context{Notify, Cancelled, Phase, {}, {}};
  neverd_session_set_load_progress(S.get(), LoadContext::progress, &Context);
  const int Loaded = api().LoadCancellable
                         ? api().LoadCancellable(S.get(), Name.c_str(),
                                                 LoadContext::stop, &Context)
                         : neverd_session_load(S.get(), Name.c_str());
  neverd_session_set_load_progress(S.get(), nullptr, nullptr);
  if (Context.Failure)
    std::rethrow_exception(Context.Failure);
  if (Loaded == -2 || (Cancelled && Cancelled()))
    throw Error("cancelled", std::string(Side) + ": input loading stopped");
  if (Loaded != 1)
    throw Error("comparison_load_failed",
                std::string(Side) + ": " + take(neverd_last_error(S.get())));
  if (Notify)
    Notify(Phase.c_str(), 1, 1, Name.c_str());
  return S;
}
struct Stamp {
  std::string Path;
  fs::file_time_type Time;
  std::uintmax_t Size = 0;
  explicit Stamp(const Json &Input) : Path(pathField(Input)) {
    Time = fs::last_write_time(path(Path));
    Size = fs::file_size(path(Path));
  }
  bool changed() const {
    std::error_code E;
    if (fs::file_size(path(Path), E) != Size || E)
      return true;
    return fs::last_write_time(path(Path), E) != Time || E;
  }
};
std::string readSaved(const Json &Payload) {
  const auto Path = path(pathField(Payload));
  std::error_code E;
  const auto Size = fs::file_size(Path, E);
  if (E || Size > limits::kMaxSavedComparisonBytes)
    throw Error("comparison_read_failed",
                "Cannot read bounded comparison file");
  std::string Text(Size, '\0');
  std::ifstream Input(Path, std::ios::binary);
  if (!Input.read(Text.data(), static_cast<std::streamsize>(Size)))
    throw Error("comparison_read_failed", "Cannot read comparison file");
  return Text;
}
} // namespace

struct ComparisonService::Impl {
  Writer Write;
  neverd_comparison_t Handle = nullptr;
  std::unique_ptr<Stamp> PrimaryStamp, SecondaryStamp;
  Json Primary, Secondary;
  std::uint64_t Revision = 0;
  std::unique_ptr<GraphSnapshot> Graphs[2];
  std::string GraphMetricsKeys[2];
  bool GraphComplete[2] = {false, false};
  void clearGraphs() {
    Graphs[0].reset();
    Graphs[1].reset();
  }
  explicit Impl(Writer W) : Write(std::move(W)) {}
  ~Impl() {
    if (Handle)
      api().destroy(Handle);
  }
  bool stale() const {
    return PrimaryStamp &&
           (PrimaryStamp->changed() || SecondaryStamp->changed());
  }
  std::string text(const char *Value) const {
    if (!Value)
      throw Error("comparison_failed", take(api().last_error(Handle)));
    return take(Value);
  }
  Json result(const char *Value) const {
    auto Out = parseJson(text(Value), MaxBackendBytes);
    Out["comparison_revision"] = std::to_string(Revision);
    return Out;
  }
  Json summary() const {
    auto Out = result(api().summary_json(Handle));
    Out["stale"] = stale();
    Out["job"] = parseJson(text(api().status_json(Handle)));
    return Out;
  }
  void check(int OK, const char *Code = "comparison_failed") const {
    if (!OK)
      throw Error(Code, take(api().last_error(Handle)));
  }
};

ComparisonService::ComparisonService(Writer Write)
    : State(std::make_unique<Impl>(std::move(Write))) {}
ComparisonService::~ComparisonService() = default;
bool ComparisonService::supported() { return bool(api()); }

bool ComparisonService::cancellable(std::string_view Operation) {
  for (const auto Candidate : CancellableOperations)
    if (Candidate == Operation)
      return true;
  return false;
}

Json ComparisonService::execute(const std::string &Operation, const Json &P,
                                const Progress &Notify,
                                const Cancellation &Cancelled) {
  if (!supported())
    throw Error("unsupported", "This engine has no native comparison C API");
  auto &S = *State;
  Json Payload = P;
  if (Payload.contains("expected_comparison_revision")) {
    if (Payload.at("expected_comparison_revision") !=
        std::to_string(S.Revision))
      throw Error("stale_comparison",
                  "Comparison mapping changed; refresh before retrying");
    Payload.erase("expected_comparison_revision");
  }
  if (Operation == "comparison_start") {
    fields(Payload, {"primary", "secondary", "options", "restore_path"});
    if (!Payload.contains("primary") || !Payload.contains("secondary"))
      throw Error("invalid_request", "Two comparison inputs are required");
    const auto &A = Payload.at("primary"), &B = Payload.at("secondary");
    auto AS = std::make_unique<Stamp>(A), BS = std::make_unique<Stamp>(B);
    auto Primary = load(A, "primary", Notify, Cancelled);
    auto Secondary = load(B, "secondary", Notify, Cancelled);
    auto Settings = Payload.value("options", Json::object());
    std::string Saved;
    if (Payload.contains("restore_path")) {
      Saved = readSaved({{"path", Payload.at("restore_path")}});
      const auto Document = parseJson(Saved, MaxBackendBytes);
      if (!Document.contains("settings") ||
          !Document.at("settings").is_object() || !Settings.is_object())
        throw Error("invalid_request",
                    "Saved comparison and request need settings objects");
      for (const auto &[Key, Value] : Document.at("settings").items())
        Settings[Key] = Value;
    }
    const auto Options = Settings.dump();
    std::unique_ptr<void, decltype(api().destroy)> Next(api().create(),
                                                        api().destroy);
    if (!Next)
      throw Error("comparison_failed", "Cannot create comparison handle");
    const auto Forward = [](void *Context, const char *Phase,
                            unsigned long long Done, unsigned long long Total) {
      const auto &Sink = *static_cast<const Progress *>(Context);
      if (Sink)
        Sink(Phase, Done, Total, "");
    };
    if (Cancelled && Cancelled())
      api().cancel(Next.get());
    std::jthread Monitor([&](std::stop_token Stop) {
      while (!Stop.stop_requested()) {
        if (Cancelled && Cancelled()) {
          api().cancel(Next.get());
          return;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(limits::kCancellationPollMilliseconds));
      }
    });
    auto AHandle = Primary.get(), BHandle = Secondary.get();
    const int OK =
        api().run_owned(Next.get(), &AHandle, &BHandle, Options.c_str(),
                        Forward, const_cast<Progress *>(&Notify));
    if (!AHandle)
      Primary.release();
    if (!BHandle)
      Secondary.release();
    Monitor.request_stop();
    Monitor.join();
    if (!OK)
      throw Error("comparison_failed", take(api().last_error(Next.get())));
    if (AS->changed() || BS->changed())
      throw Error("stale_input",
                  "A comparison input changed while it was read");
    if (Payload.contains("restore_path")) {
      if (!api().restore_json(Next.get(), Saved.c_str()))
        throw Error("stale_comparison", take(api().last_error(Next.get())));
    }
    if (S.Handle)
      api().destroy(S.Handle);
    S.Handle = Next.release();
    S.Primary = A;
    S.Secondary = B;
    S.PrimaryStamp = std::move(AS);
    S.SecondaryStamp = std::move(BS);
    ++S.Revision;
    S.clearGraphs();
    return S.summary();
  }
  if (!S.Handle)
    throw Error("comparison_not_loaded", "Start a comparison first");
  if (Operation == "comparison_summary") {
    fields(Payload, {});
    return S.summary();
  }
  if (Operation == "comparison_close") {
    fields(Payload, {});
    api().destroy(S.Handle);
    S.Handle = nullptr;
    ++S.Revision;
    S.PrimaryStamp.reset();
    S.SecondaryStamp.reset();
    return {{"closed", true}};
  }
  if (Operation == "comparison_save") {
    fields(Payload, {"path"});
    S.Write(path(pathField(Payload)),
            S.text(api().export_json(S.Handle)) + "\n");
    return {{"saved", true}, {"path", pathField(Payload)}};
  }
  if (Operation == "comparison_export") {
    fields(Payload, {});
    // Export keeps the exact shared schema; large exports use comparison_save.
    return parseJson(S.text(api().export_json(S.Handle)), MaxBackendBytes);
  }
  if (S.stale())
    throw Error("stale_input",
                "A comparison input changed; reload the comparison");
  std::jthread Monitor;
  if (cancellable(Operation))
    Monitor = std::jthread([&](std::stop_token Stop) {
      while (!Stop.stop_requested()) {
        if (Cancelled && Cancelled())
          api().cancel(S.Handle);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(limits::kCancellationPollMilliseconds));
      }
    });
  if (Operation == "comparison_functions")
    return S.result(api().functions_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_function_page")
    return S.result(api().function_page_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_regions")
    return S.result(api().regions_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_flow_regions")
    return S.result(api().flow_regions_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_region_page")
    return S.result(api().region_page_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_reviews")
    return S.result(api().reviews_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_observations")
    return S.result(api().observations_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_scalar_validation")
    return S.result(
        api().owned_scalar_validation_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_dataflow")
    return S.result(api().dataflow_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_slice" || Operation == "comparison_impact") {
    return S.result((Operation == "comparison_slice" ? api().slice_json
                                                     : api().impact_json)(
        S.Handle, Payload.dump().c_str()));
  }
  if (Operation == "comparison_cfg_summary" ||
      Operation == "comparison_cfg_viewport" ||
      Operation == "comparison_cfg_focus") {
    const auto Side = stringField(Payload, "side", "primary");
    if (Side != "primary" && Side != "secondary")
      throw Error("invalid_request",
                  "Comparison side must be primary or secondary");
    const auto Address =
        hexAddress(parseAddress(stringField(Payload, "address")));
    const int Index = Side == "secondary";
    const auto Metrics = GraphMetrics::fromRequest(Payload);
    const auto MetricsKey = Metrics.key();
    if (!S.Graphs[Index] || S.Graphs[Index]->address() != Address ||
        Payload.value("refresh", false) ||
        (Operation == "comparison_cfg_summary" &&
         S.GraphMetricsKeys[Index] != MetricsKey)) {
      Json Query{
          {"side", Side},
          {"address", Address},
          {"node_limit", ::neverd::limits::worker::kGraphSnapshotMaxNodes},
          {"edge_limit", ::neverd::limits::worker::kGraphSnapshotMaxEdges}};
      for (const auto *Key : {"timeout_ms", "refresh"})
        if (Payload.contains(Key))
          Query[Key] = Payload.at(Key);
      auto Graph =
          parseJson(S.text(api().graph_json(S.Handle, Query.dump().c_str())),
                    MaxBackendBytes);
      if (!Graph.value("computation_complete", true))
        throw Error(Graph.value("state", std::string("comparison_stopped")),
                    "Function comparison stopped; refresh to retry");
      S.GraphComplete[Index] = Graph.value("analysis_complete", false);
      S.GraphMetricsKeys[Index] = MetricsKey;
      S.Graphs[Index] = std::make_unique<GraphSnapshot>(
          std::move(Graph), Address,
          std::to_string(S.Revision) + ":" + Side + ":" + Address + ":" +
              MetricsKey,
          Metrics);
    }
    if (Operation == "comparison_cfg_focus") {
      auto Box = S.Graphs[Index]->locate(
          hexAddress(parseAddress(stringField(Payload, "block"))));
      if (Box.is_null())
        throw Error("invalid_request", "Block is absent from comparison graph");
      return Box;
    }
    auto Result = Operation == "comparison_cfg_summary"
                      ? S.Graphs[Index]->summary()
                      : S.Graphs[Index]->viewport(Payload);
    Result["comparison_revision"] = std::to_string(S.Revision);
    Result["analysis_complete"] = S.GraphComplete[Index];
    return Result;
  }
  if (Operation == "comparison_function") {
    fields(Payload, {"side", "address"});
    const auto Side = stringField(Payload, "side", "primary");
    if (Side != "primary" && Side != "secondary")
      throw Error("invalid_request",
                  "Comparison side must be primary or secondary");
    return S.result(
        api().function_json(S.Handle, Side == "secondary",
                            parseAddress(stringField(Payload, "address"))));
  }
  if (Operation == "comparison_restore") {
    fields(Payload, {"path"});
    const auto Saved = readSaved(Payload);
    S.check(api().restore_json(S.Handle, Saved.c_str()), "stale_comparison");
  } else if (Operation == "comparison_review_record") {
    S.check(api().review_record_json(S.Handle, Payload.dump().c_str()));
  } else if (Operation == "comparison_override") {
    S.check(api().override_json(S.Handle, Payload.dump().c_str()));
  } else if (Operation == "comparison_undo" || Operation == "comparison_redo") {
    fields(Payload, {});
    S.check(
        (Operation == "comparison_undo" ? api().undo : api().redo)(S.Handle));
  } else if (Operation == "comparison_validate") {
    fields(Payload, {});
    auto Primary = load(S.Primary, "primary", Notify, Cancelled);
    auto Secondary = load(S.Secondary, "secondary", Notify, Cancelled);
    S.check(api().validate_inputs(S.Handle, Primary.get(), Secondary.get()),
            "stale_input");
    return S.summary();
  } else {
    throw Error("unsupported", "Unknown comparison operation");
  }
  ++S.Revision;
  S.clearGraphs();
  return S.summary();
}
} // namespace neverd::worker
