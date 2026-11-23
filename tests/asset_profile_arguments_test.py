# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Check that phone-free benchmarks use the profile's exact trust policy."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'benchmark_physical_ui', Path(__file__).resolve().parents[1] / 'tools/benchmark_physical_ui.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class ProfileArgumentsTest(unittest.TestCase):
    def test_policy_and_optional_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            asset = root / 'asset'
            asset.mkdir()
            vehicle = root / 'vehicle.plist'
            vehicle.write_bytes(b'fixture')
            receipt = root / 'receipt.conf'
            receipt.write_text('fixture')
            path = root / 'profile.conf'
            base = (f'assetDirectory={asset}\nassetIdentifier=fixture\nassetVersion=7\n'
                    f'vehicleProfile={vehicle}\n')
            expected = [str(asset), 'fixture', '7', '-', '--worker-gpu', str(vehicle)]
            path.write_text(base)
            self.assertEqual(bench.profile_arguments(path), expected)
            path.write_text(base + 'allowExpiredAssetCertificate=false\n')
            self.assertEqual(bench.profile_arguments(path), expected)
            path.write_text(base + 'allowExpiredAssetCertificate=true\n')
            self.assertEqual(bench.profile_arguments(path), expected + ['--allow-expired-leaf'])
            path.write_text(base + f'acceptanceReference={receipt}\nallowExpiredAssetCertificate=true\n')
            pinned = expected.copy()
            pinned[3] = str(receipt)
            self.assertEqual(bench.profile_arguments(path), pinned + ['--allow-expired-leaf'])
            for bad in ('yes', '1', 'False', 'false\nallowExpiredAssetCertificate=true'):
                path.write_text(base + f'allowExpiredAssetCertificate={bad}\n')
                with self.assertRaises(ValueError):
                    bench.profile_arguments(path)
            path.write_text(base + 'acceptanceReference=relative.conf\n')
            with self.assertRaises(ValueError):
                bench.profile_arguments(path)


if __name__ == '__main__':
    unittest.main()
