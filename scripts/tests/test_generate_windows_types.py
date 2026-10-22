"""Regressions for reproducible Windows provider declaration inputs."""

import hashlib
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "generate_windows_types.py"
SPEC = importlib.util.spec_from_file_location("generate_windows_types", SCRIPT)
windows = importlib.util.module_from_spec(SPEC)
# dataclasses resolves the defining module when annotating Target.
sys.modules[SPEC.name] = windows
SPEC.loader.exec_module(windows)


class WindowsProviderInputs(unittest.TestCase):
    def test_legacy_wan_missing_comma_is_the_only_repair(self):
        broken = ("void NdisMWanIndicateReceiveComplete(\n"
                  "NDIS_HANDLE MiniportAdapterHandle\n"
                  "NDIS_HANDLE NdisLinkContext);")
        fixed = broken.replace("MiniportAdapterHandle\n",
                               "MiniportAdapterHandle,\n")
        self.assertEqual(windows.repair_kernel_declarations(broken), fixed)
        self.assertEqual(windows.repair_kernel_declarations(fixed), fixed)
        unrelated = broken.replace("NdisMWanIndicateReceiveComplete", "Other")
        self.assertEqual(windows.repair_kernel_declarations(unrelated), unrelated)

    def test_filter_ddk_and_information_class_are_verified_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = ("typedef enum _SYSTEM_INFORMATION_CLASS {\n"
                      "SystemBasicInformation = 0,\n"
                      "SystemProcessInformation = 5\n"
                      "} SYSTEM_INFORMATION_CLASS;\n")
            (root / "ntexapi.h").write_text(source)
            ddk = root / "fltkernel.h"
            ddk.write_bytes(b"/* pinned public-domain DDK fixture */\n")
            (root / "reactos-fltkernel.sha256").write_text(
                hashlib.sha256(ddk.read_bytes()).hexdigest() + " fltkernel.h\n")
            arguments = SimpleNamespace(phnt=root, reactos_ddk=root)
            with patch.object(windows, "PHNT_SHIMS", root):
                includes = windows.kernel_additions(arguments, root)
                self.assertEqual((root / "system_information_class.h").read_text(),
                                 source)
                self.assertIn(f'#include "{ddk.resolve()}"', includes)
                ddk.write_bytes(b"changed input")
                with self.assertRaisesRegex(SystemExit, "pinned"):
                    windows.kernel_additions(arguments, root)
                (root / "ntexapi.h").write_text(source + source)
                with self.assertRaisesRegex(SystemExit, "exactly one"):
                    windows.kernel_additions(arguments, root)


if __name__ == "__main__":
    unittest.main()
