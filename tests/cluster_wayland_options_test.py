#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Retry admission must reject before acquiring any display or private asset."""
import subprocess
import sys

binary = sys.argv[1]
for args in [
    ["--local-ultra-output-retry"],
    ["--background", "--local-ultra-output-retry"],
    ["--local-ultra-profile", "/private/not-read", "--local-ultra-output-retry"],
]:
    result = subprocess.run([binary, *args], capture_output=True, text=True, timeout=5)
    assert result.returncode == 1, result
    assert "requires signed profile and background role" in result.stderr, result.stderr
    assert "Local Ultra gauges" not in result.stderr, result.stderr
    assert "Wayland" not in result.stderr, result.stderr
print("Retry prerequisites reject before resource or display ownership")

for path in ("relative.sock", "/tmp/../prephone.sock", "/" + "a" * 104):
    result = subprocess.run([binary, '--prephone-publication-socket', path], capture_output=True, text=True, timeout=5)
    assert result.returncode == 1 and 'invalid private pre-phone publication socket' in result.stderr, result
    assert 'Wayland' not in result.stderr, result.stderr
print('Native publication paths reject before display ownership')

for source in ('--simulate', '--simulate-dated', '--vehicle-socket'):
    for other in ('--simulate', '--simulate-dated', '--vehicle-socket'):
        args = [source] + (['/not-opened/source.sock'] if source == '--vehicle-socket' else [])
        args += [other] + (['/not-opened/other.sock'] if other == '--vehicle-socket' else [])
        result = subprocess.run([binary, *args], capture_output=True, text=True, timeout=5)
        assert result.returncode == 1 and 'usage:' in result.stderr, result
        assert 'Wayland' not in result.stderr, result.stderr
print('All duplicate/mixed native source modes reject before display ownership')

for args in (["--prephone-indicators"], ["--prephone-indicators", "--prephone-indicators"],
             ["--prephone-publication-socket", "/tmp/prephone.sock", "--prephone-indicators", "--prephone-indicators"]):
    result = subprocess.run([binary, *args], capture_output=True, text=True, timeout=5)
    assert result.returncode == 1, result
    assert "Wayland" not in result.stderr, result.stderr
    assert "require native publication" in result.stderr or "usage:" in result.stderr, result.stderr
print('Optional indicators reject missing publication and duplicate flags before display ownership')
