import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from scripts import probe_native_host as probe


# Independent transcripts include creation and retirement, not just a boolean
# capability. ARM64 inserts its own initialization into these common paths.
KVM = """NativeArchitecture\tok\t0\t1
OpenKvm\tok\t0\t3
ApiVersion\tok\t0\t12
GuestDebugCapability\tok\t0\t1
CreateVM\tok\t0\t4
CreateCPU\tok\t0\t5
SetGuestDebug\tok\t0\t0
RunMappingSize\tok\t0\t12288
CloseCPU\tok\t0\t0
CloseVM\tok\t0\t0
CloseKvm\tok\t0\t0
"""
WHP = """NativeArchitecture\tok\t0\t1
LoadLibrary\tok\t0\t0
LibraryExports\tok\t0\t8
HypervisorPresent\tok\t0\t1
ExtendedExits\tok\t0\t7
CreatePartition\tok\t0\t0
ProcessorCount\tok\t0\t0
ConfigureExits\tok\t0\t0
SetupPartition\tok\t0\t0
CreateVirtualProcessor\tok\t0\t0
DeleteVirtualProcessor\tok\t0\t0
DeletePartition\tok\t0\t0
FreeLibrary\tok\t0\t0
"""
NO_KVM = "NativeArchitecture\tok\t0\t1\nOpenKvm\tunavailable\t2\t0\n"


class HostEvidenceTests(unittest.TestCase):
    def test_success_requires_creation_and_cleanup_for_each_host(self):
        for backend, text in (("kvm", KVM), ("whp", WHP)):
            with self.subTest(backend=backend):
                status, records = probe.parse_evidence(text, 0, backend, "X64")
                self.assertEqual(status, "setup_ready")
                self.assertEqual(len(records), len(text.splitlines()))

    def test_arm64_requires_its_native_initialization(self):
        arm_kvm = KVM.replace("\t0\t1\n", "\t0\t2\n", 1).replace(
            "SetGuestDebug", "PreferredTarget\tok\t0\t0\nInitializeCPU\tok\t0\t0\nSetGuestDebug")
        arm_whp = WHP.replace("\t0\t1\n", "\t0\t2\n", 1).replace(
            "ExtendedExits", "Arm64Support\tok\t0\t2048\nExtendedExits").replace(
            "SetupPartition", "GicLpiBits\tok\t0\t16\nConfigureGic\tok\t0\t0\nSetupPartition").replace(
            "DeleteVirtualProcessor", "SetRedistributor\tok\t0\t0\nDeleteVirtualProcessor")
        for backend, text in (("kvm", arm_kvm), ("whp", arm_whp)):
            with self.subTest(backend=backend):
                self.assertEqual(probe.parse_evidence(text, 0, backend, "ARM64")[0], "setup_ready")
        with self.assertRaises(ValueError):
            probe.parse_evidence(arm_kvm.replace("InitializeCPU\tok\t0\t0\n", ""), 0, "kvm", "ARM64")
        with self.assertRaises(ValueError):
            probe.parse_evidence(arm_whp.replace("ConfigureGic\tok\t0\t0\n", ""), 0, "whp", "ARM64")

    def test_capability_absence_is_explicit_unavailable(self):
        self.assertEqual(probe.parse_evidence(NO_KVM, 77, "kvm", "X64")[0], "unavailable")
        text = WHP.split("HypervisorPresent")[0] + "HypervisorPresent\tunavailable\t0\t0\nFreeLibrary\tok\t0\t0\n"
        self.assertEqual(probe.parse_evidence(text, 77, "whp", "X64")[0], "unavailable")

    def test_setup_failure_preserves_stage_code_and_cleanup(self):
        text = KVM.split("CreateCPU")[0] + "CreateCPU\tfailed\t12\t0\nCloseVM\tok\t0\t0\nCloseKvm\tok\t0\t0\n"
        status, records = probe.parse_evidence(text, 1, "kvm", "X64")
        self.assertEqual(status, "failed")
        self.assertIn({"stage": "CreateCPU", "status": "failed", "code": 12, "value": 0}, records)
        with self.assertRaises(ValueError):
            probe.parse_evidence(text.replace("failed", "unavailable"), 77, "kvm", "X64")

    def test_cleanup_failure_overrides_unavailability(self):
        text = KVM.split("GuestDebugCapability")[0] + "GuestDebugCapability\tunavailable\t0\t0\nCloseKvm\tfailed\t5\t0\n"
        self.assertEqual(probe.parse_evidence(text, 1, "kvm", "X64")[0], "failed")
        with self.assertRaises(ValueError):
            probe.parse_evidence(text, 77, "kvm", "X64")

    def test_all_successful_record_omissions_and_duplicates_fail(self):
        for backend, text in (("kvm", KVM), ("whp", WHP)):
            lines = text.splitlines()
            for index in range(len(lines)):
                for changed in (lines[:index] + lines[index + 1:],
                                lines[:index] + [lines[index]] + lines[index:]):
                    with self.subTest(backend=backend, index=index, count=len(changed)), self.assertRaises(ValueError):
                        probe.parse_evidence("\n".join(changed), 0, backend, "X64")

    def test_exit_codes_and_wrong_architecture_cannot_claim_readiness(self):
        for text, code, architecture in ((KVM, 77, "X64"), (KVM, -9, "X64"),
                                          (KVM, 0, "ARM64"), (NO_KVM, 0, "X64")):
            with self.subTest(code=code, architecture=architecture), self.assertRaises(ValueError):
                probe.parse_evidence(text, code, "kvm", architecture)

    def test_prepare_cannot_resume_after_failure_or_cleanup(self):
        for text in (NO_KVM + "ApiVersion\tok\t0\t12\n",
                     KVM.replace("CreateCPU\tok\t0\t5\n", "CreateCPU\tfailed\t5\t0\n"),
                     KVM.replace("SetGuestDebug\tok\t0\t0\n", "").replace(
                         "CloseVM", "SetGuestDebug\tok\t0\t0\nCloseVM")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                probe.parse_evidence(text, 1, "kvm", "X64")

    def test_transcript_rejects_unknown_records_and_out_of_range_values(self):
        for text in ("", KVM + "unknown\tok\t0\t0\n", KVM.replace("ok", "passed", 1),
                     KVM.replace("12288", "-1"), KVM.replace("12288", str(1 << 64))):
            with self.subTest(text=text), self.assertRaises(ValueError):
                probe.parse_evidence(text, 0, "kvm", "X64")

    def test_host_identity_requires_matching_isa_and_os(self):
        for system, machine, backend, architecture, matches in (
            ("Linux", "aarch64", "kvm", "ARM64", True),
            ("Windows", "ARM64", "whp", "ARM64", True),
            ("Windows", "AMD64", "whp", "X64", True),
            ("Linux", "x86_64", "kvm", "ARM64", False),
            ("Linux", "aarch64", "whp", "ARM64", False),
        ):
            with self.subTest(system=system, machine=machine), \
                    mock.patch.object(probe.platform, "system", return_value=system), \
                    mock.patch.object(probe.platform, "machine", return_value=machine):
                self.assertEqual(probe.host_matches(backend, architecture), matches)


class HostProbeRunnerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.output = Path(temporary.name)
        self.probe_result = subprocess.CompletedProcess([], 0, KVM, "")
        self.build_result = subprocess.CompletedProcess([], 0, "built\n", "")
        self.dirty = False

    def command(self, args, **kwargs):
        if "-o" in args:
            (self.output / "probe").write_bytes(b"fixture")
            if isinstance(self.build_result, Exception):
                raise self.build_result
            return self.build_result
        if isinstance(self.probe_result, Exception):
            raise self.probe_result
        return self.probe_result

    def git(self, args, **kwargs):
        if "rev-parse" in args:
            return "a" * 40 + "\n"
        return " M tracked.cpp\n" if self.dirty else ""

    def run_probe(self, require=False):
        with mock.patch.object(probe, "host_matches", return_value=True), \
                mock.patch.object(probe.shutil, "which", return_value="c++"), \
                mock.patch.object(probe.subprocess, "check_output", side_effect=self.git), \
                mock.patch.object(probe.subprocess, "run", side_effect=self.command), \
                mock.patch("builtins.print"):
            result = probe.run(self.output, "kvm", "X64", None, require)
        return result, json.loads((self.output / "summary.json").read_text())

    def test_success_records_source_binary_and_false_execution_claim(self):
        result, report = self.run_probe()
        self.assertEqual(result, 0)
        self.assertEqual(report["status"], "setup_ready")
        self.assertFalse(report["guest_execution_verified"])
        self.assertEqual(set(report["source_sha256"]), set(probe.SOURCES))
        self.assertEqual(len(report["program_sha256"]), 64)
        self.assertEqual(report["commit"], report["final_commit"])

    def test_unavailable_is_never_setup_ready_and_can_be_required(self):
        self.probe_result = subprocess.CompletedProcess([], 77, NO_KVM, "")
        for require in (False, True):
            result, report = self.run_probe(require)
            self.assertEqual(result, int(require))
            self.assertEqual(report["status"], "unavailable")

    def test_build_failure_is_not_a_hardware_skip(self):
        self.build_result = subprocess.CompletedProcess([], 1, "", "compiler error\n")
        result, report = self.run_probe()
        self.assertEqual(result, 1)
        self.assertEqual(report["stage"], "build")
        self.assertEqual(report["status"], "failed")
        self.assertEqual((self.output / "build.stderr").read_text(), "compiler error\n")

    def test_timeout_preserves_partial_output_and_fails(self):
        self.probe_result = subprocess.TimeoutExpired(["probe"], 30, output=b"partial\n", stderr=b"diagnostic\n")
        result, report = self.run_probe()
        self.assertEqual(result, 1)
        self.assertEqual(report["status"], "failed")
        self.assertEqual((self.output / "probe.stdout").read_bytes(), b"partial\n")
        self.assertEqual((self.output / "probe.stderr").read_bytes(), b"diagnostic\n")

    def test_unexpected_stderr_and_truncated_transcript_fail(self):
        for result in (subprocess.CompletedProcess([], 0, KVM, "warning"),
                       subprocess.CompletedProcess([], 0, KVM.split("CreateCPU")[0], "")):
            self.probe_result = result
            code, report = self.run_probe()
            self.assertEqual(code, 1)
            self.assertEqual(report["status"], "failed")

    def test_dirty_source_cannot_be_used_as_native_evidence(self):
        self.dirty = True
        code, report = self.run_probe()
        self.assertEqual(code, 1)
        self.assertEqual(report["commands"], [])


if __name__ == "__main__":
    unittest.main()
