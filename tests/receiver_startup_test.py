# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Managed radio ordering with explicit command/clock substitutes; no radios."""
import subprocess
import sys
from pathlib import Path
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import run_lab_receiver as launcher


class Startup(unittest.TestCase):
    def setUp(self):
        self.now = 100.
        self.commands = []

    def sleep(self, seconds):
        self.now += seconds

    def run_command(self, command, **options):
        self.commands.append((command, options))
        return subprocess.CompletedProcess(command, 0, stdout='s "fixture"\n')

    def test_manager_waits_for_existing_network_owner_before_bluetooth(self):
        events = []
        def run(command, **options):
            self.commands.append((command, options))
            events.append(command[0])
            result = 1 if len(self.commands) == 1 else 0
            return subprocess.CompletedProcess(command, result)
        with patch.object(launcher.subprocess, 'run', side_effect=run), \
                patch.object(launcher.time, 'monotonic', side_effect=lambda: self.now), \
                patch.object(launcher.time, 'sleep', side_effect=self.sleep), \
                patch.object(launcher, 'enable_bluetooth_connections', side_effect=lambda: events.append('bluetooth')):
            launcher.prepare_radios(True, False, lambda: False)
        self.assertEqual(events, ['busctl', 'busctl', 'nmcli', 'bluetooth'])
        self.assertEqual(self.commands[-1][0], ['nmcli', 'radio', 'wifi', 'on'])
        self.assertEqual(self.commands[-1][1]['timeout'], 5)
        self.assertTrue(self.commands[-1][1]['check'])
        self.assertLessEqual(self.commands[0][1]['timeout'], 1)

    def test_missing_or_stalled_network_owner_is_bounded_and_never_admits_bluetooth(self):
        for failure in ('absent', 'timeout', 'late-success'):
            with self.subTest(failure=failure):
                self.now = 100.
                def run(command, **options):
                    self.assertEqual(command[0], 'busctl')
                    if failure == 'late-success':
                        self.now += 6
                        return subprocess.CompletedProcess(command, 0)
                    self.now += options['timeout']
                    if failure == 'timeout':
                        raise subprocess.TimeoutExpired(command, options['timeout'])
                    return subprocess.CompletedProcess(command, 1)
                with patch.object(launcher.subprocess, 'run', side_effect=run), \
                        patch.object(launcher.time, 'monotonic', side_effect=lambda: self.now), \
                        patch.object(launcher.time, 'sleep', side_effect=self.sleep), \
                        patch.object(launcher, 'enable_bluetooth_connections') as bluetooth:
                    with self.assertRaisesRegex(RuntimeError, 'did not become ready'):
                        launcher.prepare_radios(True, False, lambda: False)
                bluetooth.assert_not_called()
                self.assertAlmostEqual(self.now, 106.)

    def test_wifi_command_failure_cannot_start_bluetooth_or_report_ready(self):
        for error in (subprocess.TimeoutExpired('nmcli', 5), subprocess.CalledProcessError(1, 'nmcli')):
            stages = []
            def run(command, **options):
                if command[0] == 'nmcli':
                    raise error
                return subprocess.CompletedProcess(command, 0)
            with self.subTest(error=error), patch.object(launcher.subprocess, 'run', side_effect=run), \
                    patch.object(launcher, 'startup_diagnostic', side_effect=lambda enabled, stage: stages.append(stage)), \
                    patch.object(launcher, 'enable_bluetooth_connections') as bluetooth:
                with self.assertRaises(type(error)):
                    launcher.prepare_radios(True, True, lambda: False)
            self.assertEqual(stages, ['wifi-wait-begin'])
            bluetooth.assert_not_called()

    def test_cancellation_before_wait_after_reply_or_after_wifi_prevents_bluetooth(self):
        for replies in ([True], [False, True], [False, False, True]):
            self.commands = []
            with self.subTest(replies=replies), patch.object(launcher.subprocess, 'run', side_effect=self.run_command), \
                    patch.object(launcher, 'enable_bluetooth_connections') as bluetooth:
                with self.assertRaisesRegex(RuntimeError, 'cancelled'):
                    launcher.prepare_radios(True, False, unittest.mock.Mock(side_effect=replies))
            bluetooth.assert_not_called()
            self.assertLessEqual(len(self.commands), 2)

    def test_legacy_launcher_keeps_its_existing_network_admission(self):
        with patch.object(launcher, 'enable_wifi_radio') as wifi, \
                patch.object(launcher, 'enable_bluetooth_connections') as bluetooth:
            launcher.prepare_radios(False, False, lambda: False)
        wifi.assert_not_called()
        bluetooth.assert_called_once_with()


if __name__ == '__main__':
    unittest.main()
