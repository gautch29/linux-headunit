# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual capture probe main with an injected synthetic device, never hardware evidence."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PROBE = sys.argv.pop(1)


class ProbeGeometry(unittest.TestCase):
    def run_probe(self, fit, acquisition_width=640):
        profile = {
            "schemaVersion": 1,
            "enabled": True,
            "manualActivation": True,
            "streamIdentifier": "camera",
            "source": {
                "kind": "v4l2",
                "device": "/dev/video4294967295",
                "pixelFormat": "yuyv",
                "delivery": "cpu-copy",
                "adaptation": {"width": acquisition_width, "height": 360, "fit": fit},
            },
            "capture": {"width": 1650, "height": 720, "frameRate": 30, "maximumAgeMs": 150},
            "destination": {
                "panel": "main",
                "canvas": {"width": 1920, "height": 720},
                "frame": {"x": 135, "y": 0, "width": 1650, "height": 720},
                "rotationDegrees": 0,
                "mirrorHorizontal": False,
            },
        }
        with tempfile.TemporaryDirectory(prefix="camera-fit-probe-") as temporary:
            path = Path(temporary) / "synthetic.json"
            path.write_text(json.dumps(profile))
            return subprocess.run([PROBE, str(path)], text=True, capture_output=True, timeout=15)

    def test_production_probe_geometry_and_retirement(self):
        for fit in ("contain", "cover"):
            with self.subTest(fit=fit):
                result = self.run_probe(fit)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("synthetic probe fixture: device stopped", result.stderr)
                report = json.loads(result.stdout)
                print(json.dumps({"fixture": "synthetic-pixels/injected-device",
                                  "physicalEvidence": False, "report": report}))
                # This unchanged production flag is NOT a hardware pass in this
                # injected fixture. The probe must still deny display evidence.
                self.assertTrue(report["physicalCapture"])
                self.assertFalse(report["displayQualified"])
                self.assertGreaterEqual(report["frames"], 30)
                self.assertEqual((report["driverWidth"], report["driverHeight"]), (640, 360))
                self.assertEqual((report["deliveryWidth"], report["deliveryHeight"]), (1650, 720))
                geometry = report["captureGeometry"]
                self.assertEqual(set(geometry), {
                    "policy", "adaptationEnabled", "negotiatedStrideBytes", "sourceAspect",
                    "deliveryAspect", "scale", "imageRect", "sourceRect", "deliveryRect",
                    "signedSceneFit", "opticalFit",
                })
                self.assertEqual(geometry["policy"], fit)
                self.assertTrue(geometry["adaptationEnabled"])
                self.assertEqual(geometry["negotiatedStrideBytes"], 1296)
                self.assertAlmostEqual(geometry["sourceAspect"], 16 / 9)
                self.assertAlmostEqual(geometry["deliveryAspect"], 55 / 24)
                self.assertIsNone(geometry["signedSceneFit"])
                self.assertIsNone(geometry["opticalFit"])
                if fit == "contain":
                    self.assertEqual(geometry["scale"], 2)
                    self.assertEqual(geometry["sourceRect"],
                                     {"x": 0, "y": 0, "width": 640, "height": 360})
                    self.assertEqual(geometry["deliveryRect"],
                                     {"x": 185, "y": 0, "width": 1280, "height": 720})
                else:
                    self.assertEqual(geometry["scale"], 2.578125)
                    self.assertEqual(geometry["imageRect"],
                                     {"x": 0, "y": -104.0625, "width": 1650, "height": 928.125})
                    self.assertAlmostEqual(geometry["sourceRect"]["y"], 40.36363636363636)
                    self.assertEqual(geometry["deliveryRect"],
                                     {"x": 0, "y": 0, "width": 1650, "height": 720})
                for name in ("lastDequeue", "lastPublication"):
                    timing = report[name]
                    self.assertEqual(timing["reference"], "start-of-exposure")
                    self.assertGreater(timing["captureMonotonicNs"], 0)
                    self.assertGreaterEqual(timing["observedMonotonicNs"], timing["captureMonotonicNs"])
                    self.assertAlmostEqual(timing["ageMs"],
                                           (timing["observedMonotonicNs"] - timing["captureMonotonicNs"]) / 1e6,
                                           delta=.001)  # Existing display precision; integer dates are exact.
                self.assertLessEqual(report["maximumAgeMs"], 150)
                self.assertNotIn("device", result.stdout)

    def test_changed_negotiation_has_no_success_geometry(self):
        result = self.run_probe("cover", acquisition_width=638)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("capture negotiation differs", result.stderr)
        self.assertIn("synthetic probe fixture: device stopped", result.stderr)


if __name__ == "__main__":
    unittest.main()
