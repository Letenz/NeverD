//===- Descriptor.cpp - Offline metadata provenance admission ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate captured source/time declarations without consulting a clock.
///
//===----------------------------------------------------------------------===//

#include "Admission.h"

#include "neverd/web/Limits.h"
#include "neverd/web/PackageEvidence.h"

#include <array>

namespace neverd::web {

PackageEvidenceTime packageEvidenceTime(std::string_view Text) {
  PackageEvidenceTime T;
  if (Text.empty())
    return T;
  if (Text.size() >
      ::neverd::limits::web::kDescriptorPackageEvidenceTimeTextSizeLimit) {
    T.Status = "invalid_timestamp";
    return T;
  }
  T.Value = Text;
  T.Status = "unsupported_or_invalid_timestamp";
  if (Text.size() < 20 || Text.size() > 30 || Text[4] != '-' ||
      Text[7] != '-' || Text[10] != 'T' || Text[13] != ':' || Text[16] != ':' ||
      Text.back() != 'Z')
    return T;
  const std::array<size_t, 6> Starts{0, 5, 8, 11, 14, 17};
  std::array<unsigned, 6> Parts{};
  for (unsigned I = 0; I != Parts.size(); ++I) {
    const auto Token = Text.substr(Starts[I], I ? 2 : 4);
    if (Token.find_first_not_of("0123456789") != Token.npos)
      return T;
    for (const auto C : Token)
      Parts[I] = Parts[I] * 10 + unsigned(C - '0');
  }
  const auto [Year, Month, Day, Hour, Minute, Second] = Parts;
  constexpr std::array<unsigned, 12> Days{31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
  const bool Leap = !(Year % 4) && ((Year % 100) || !(Year % 400));
  if (!Year || !Month || Month > 12 || !Day ||
      Day > Days[Month - 1] + (Month == 2 && Leap) || Hour > 23 ||
      Minute > 59 || Second > 59)
    return T;
  std::string Fraction;
  if (Text.size() > 20) {
    if (Text[19] != '.' || Text.size() < 22)
      return T;
    Fraction = Text.substr(20, Text.size() - 21);
    if (Fraction.find_first_not_of("0123456789") != Fraction.npos)
      return T;
  }
  Fraction.resize(9, '0');
  T.OrderKey = std::string(Text.substr(0, 19)) + Fraction;
  T.Status = "valid_utc_timestamp";
  return T;
}

PackageEvidenceDescriptor readPackageEvidenceDescriptor(const Artifact &Input) {
  using namespace package_evidence;
  Budget Work;
  const auto V = document(Input, Work);
  const auto &O = *V.getAsObject();
  if (O.getInteger("schema_version") != 1 || O.size() > 4)
    throw Error("invalid_package_evidence_descriptor");
  for (const auto &[Key, Value] : O)
    if (Key != "schema_version" && Key != "source" && Key != "retrieved_at" &&
        Key != "as_of")
      throw Error("invalid_package_evidence_descriptor");
  PackageEvidenceDescriptor D;
  D.ArtifactID = Input.ID;
  D.BlobHash = Input.BlobHash;
  D.Source = text(O, "source").value_or("");
  D.RetrievedAt =
      packageEvidenceTime(text(O, "retrieved_at", 128).value_or(""));
  D.AsOf = packageEvidenceTime(text(O, "as_of", 128).value_or(""));
  D.Status = "declared_source_and_times";
  const auto Invalid = [](const auto &T) {
    return T.Status != "valid_utc_timestamp" && T.Status != "not_supplied";
  };
  if (Invalid(D.RetrievedAt) || Invalid(D.AsOf))
    D.Status = "unsupported_or_invalid_declared_time";
  else if (!D.RetrievedAt.OrderKey.empty() && !D.AsOf.OrderKey.empty() &&
           D.AsOf.OrderKey > D.RetrievedAt.OrderKey)
    D.Status = "conflicting_declared_times";
  else if (D.Source.empty() || D.RetrievedAt.OrderKey.empty() ||
           D.AsOf.OrderKey.empty())
    D.Status = "incomplete_source_or_times";
  D.ID = identity("package-evidence-descriptor",
                  {PackageEvidenceProfile, D.ArtifactID, D.BlobHash});
  D.RetainedBytes = 1024 + D.Source.size() + D.RetrievedAt.Value.size() +
                    D.AsOf.Value.size() + D.RetrievedAt.OrderKey.size() +
                    D.AsOf.OrderKey.size();
  D.Steps = Work.Steps;
  return D;
}
} // namespace neverd::web
