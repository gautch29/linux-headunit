#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Focused real runner CLI checks; no Linux job, VM or dependency installation."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / 'tools/ci/run-local.sh'


class RunnerCliTests(unittest.TestCase):
    def assert_early_rejection(self, arguments):
        with tempfile.TemporaryDirectory(prefix='headunit-local-cli-') as folder:
            root = Path(folder)
            output = root / 'not-created/evidence'
            marker = root / 'unexpected-command'
            commands = root / 'bin'
            commands.mkdir()
            for name in ('git', 'python3', 'docker', 'clang-format'):
                command = commands / name
                command.write_text('#!/bin/sh\nprintf invoked > ' + shlex.quote(str(marker)) + '\nexit 99\n')
                command.chmod(0o755)
            environment = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ['PATH'])
            result = subprocess.run([str(RUNNER), '--output', str(output), *arguments],
                                    env=environment, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertFalse(output.parent.exists(), 'invalid CLI created output/snapshot directories')
            self.assertFalse(marker.exists(), 'invalid CLI queried sources or started a job')
            self.assertNotIn('Running ', result.stdout)

    def test_all_zero_parallelism_fails_before_work(self):
        for value in ('0', '00', '000', '00000000000000000000000000000000'):
            with self.subTest(value=value):
                self.assert_early_rejection(['--job', 'linux-gcc', '--jobs', value])

    def test_invalid_decimal_parallelism_fails_before_work(self):
        for value in ('', '-1', '+1', '1.0', ' 1', '1 ', '1$(touch marker)'):
            with self.subTest(value=value):
                self.assert_early_rejection(['--job', 'format', '--jobs', value])

    def test_job_requires_one_supported_name(self):
        for value in ('', ' ', 'unknown', 'linux-unknown', 'format macos'):
            with self.subTest(value=value):
                self.assert_early_rejection(['--job', value])

    def test_format_selection_preserves_positive_decimal(self):
        with tempfile.TemporaryDirectory(prefix='headunit-local-cli-') as folder:
            output = Path(folder) / 'evidence'
            result = subprocess.run([str(RUNNER), '--job', 'format', '--jobs', '0004',
                                     '--output', str(output)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            summary = (output / 'summary.md').read_text()
            self.assertIn('parallelism: `0004`', summary)
            self.assertIn('| format | PASS |', summary)
            self.assertEqual(list(output.glob('*.log')), [output / 'format.log'])


if __name__ == '__main__':
    unittest.main()
