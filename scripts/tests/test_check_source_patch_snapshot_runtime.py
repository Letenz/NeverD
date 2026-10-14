from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zlib

from scripts import check_source_patch_snapshot_runtime as checker


class SnapshotEvidenceTests(unittest.TestCase):
    def test_delta_has_exact_original_and_final_byte_identity(self):
        original, final = b"original", b"orPATCHl+new"
        delta = {"original_sha256": hashlib.sha256(original).hexdigest(),
                 "sha256": hashlib.sha256(final).hexdigest(), "size": len(final),
                 "writes": [{"offset": 2, "data": base64.b64encode(zlib.compress(b"PATCH")).decode()},
                            {"offset": 8, "data": base64.b64encode(zlib.compress(b"+new")).decode()}]}
        self.assertEqual(checker.apply_delta(original, delta), final)
        for mutation in (lambda d: d.update(original_sha256="0" * 64),
                         lambda d: d.update(sha256="0" * 64),
                         lambda d: d["writes"][1].update(offset=4),
                         lambda d: d["writes"][1].update(offset=100)):
            copy = json.loads(json.dumps(delta))
            mutation(copy)
            with self.assertRaises(ValueError):
                checker.apply_delta(original, copy)

    def test_zlib_suffix_truncation_and_bomb_cannot_escape_the_bound(self):
        for value in (zlib.compress(b"payload") + b"hidden", zlib.compress(b"payload")[:-1],
                      zlib.compress(b"large" * 100)):
            with self.assertRaises(ValueError):
                checker.decode_bounded(base64.b64encode(value).decode(), 8)

    def test_stale_engine_snapshot_is_rejected_before_materialization(self):
        repository = Path(__file__).resolve().parents[2]
        fixtures = repository / "unittests/Inputs/source-patch/native-processes"
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "images"
            with patch.object(checker, "engine_digest", return_value="changed"):
                with self.assertRaisesRegex(ValueError, "engine source changed"):
                    checker.prepare(repository, fixtures, output, "windows", "arm64")
            self.assertFalse(output.exists())
