import datetime
import os
from pathlib import Path
import unittest
from unittest import mock

from scripts import generate_linux_types as linux


class GenerationDateTests(unittest.TestCase):
    def test_unset_preserves_local_date(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(linux.generation_date(), datetime.date.today().isoformat())

    def test_epoch_is_utc_and_repeatable(self):
        for timezone in ('UTC', 'Pacific/Honolulu', 'Asia/Shanghai'):
            with self.subTest(timezone=timezone), mock.patch.dict(
                    os.environ, {'SOURCE_DATE_EPOCH': '0', 'TZ': timezone}):
                self.assertEqual(linux.generation_date(), '1970-01-01')
                self.assertEqual(linux.generation_date(), linux.generation_date())

    def test_day_boundary(self):
        for epoch, expected in [('86399', '1970-01-01'), ('86400', '1970-01-02')]:
            with mock.patch.dict(os.environ, {'SOURCE_DATE_EPOCH': epoch}):
                self.assertEqual(linux.generation_date(), expected)

    def test_invalid_epoch_fails(self):
        for epoch in ('', '-1', '+1', '1.5', ' 1', '1\n', 'abc', '９', '9' * 100):
            with self.subTest(epoch=epoch), mock.patch.dict(
                    os.environ, {'SOURCE_DATE_EPOCH': epoch}):
                with self.assertRaisesRegex(ValueError, 'SOURCE_DATE_EPOCH'):
                    linux.generation_date()

    def test_all_generators_use_shared_date(self):
        root = Path(linux.__file__).parent
        for name in ('linux', 'windows', 'darwin', 'uefi', 'freebsd'):
            source = (root / f'generate_{name}_types.py').read_text()
            self.assertIn('{generation_date()}' if name == 'linux' else
                          '{linux.generation_date()}', source)
            if name != 'linux':
                self.assertNotIn('date.today()', source)


if __name__ == '__main__':
    unittest.main()
