#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Exercise the actual H.264 preview with a generated stream, without a phone."""
import pathlib
import os
import subprocess
import sys
import tempfile

from generated_h264_fixture import FFMPEG_ARGUMENTS, packets

replay, ffmpeg = sys.argv[1:3]
encoded = subprocess.run([ffmpeg, *FFMPEG_ARGUMENTS], check=True,
                         stdout=subprocess.PIPE, timeout=15).stdout
with tempfile.TemporaryDirectory(prefix='headunit-preview-') as directory:
    path = pathlib.Path(directory) / 'synthetic.packets'
    path.write_bytes(packets(encoded))
    codecs = ['avdec_h264'] + (['v4l2h264dec'] if '--also-pi4' in sys.argv[3:] else [])
    sink = 'wayland-cluster' if '--wayland-cluster' in sys.argv[3:] else 'wayland-touch' if '--wayland-touch' in sys.argv[3:] else 'fake'
    environment = dict(os.environ)
    native_config = pathlib.Path(directory) / 'native-cluster.conf'
    if sink == 'wayland-cluster':
        native_config.write_text('version=1\nspeed_unit=mph\nspeed_maximum_kmh=160\n'
                                 'rpm_maximum=5000\nrpm_redline=4000\nshow_rpm=false\n')
        native_config.chmod(0o600)
        environment['HEADUNIT_NATIVE_CLUSTER_CONFIG'] = str(native_config)
    for codec in codecs:
        result = subprocess.run([replay, str(path), sink, '0', codec], capture_output=True, text=True, timeout=15, env=environment)
        assert result.returncode == 0, (codec, result.stdout, result.stderr)
        assert 'submitted_frames=18 decoded_frames=18' in result.stdout, result.stdout
        print(codec + ': 18/18 generated H.264 frames decoded via ' + sink)
    if sink == 'wayland-cluster':
        native_config.write_text('version=2\n')
        result = subprocess.run([replay, str(path), sink, '0', 'avdec_h264'], capture_output=True,
                                text=True, timeout=15, env=environment)
        assert result.returncode == 1 and 'unknown native cluster version' in result.stderr, (result.stdout, result.stderr)
        print('Invalid configured native cluster rejected before preview')
    result = subprocess.run([replay, str(path), 'fake', '0', 'avdec_h264 ! fakesink'], capture_output=True, text=True, timeout=5)
    assert result.returncode == 1 and 'unsupported preview decoder' in result.stderr
    print('Unsupported pipeline fragments rejected')
    for probe in ('--output-gate-probe', '--standard-controller-probe'):
        if probe in sys.argv[3:]:
            at = sys.argv.index(probe)
            result = subprocess.run([sys.argv[at+1], str(path)], capture_output=True, text=True, timeout=20)
            assert result.returncode == 0, (result.stdout, result.stderr)
            print(result.stdout.strip())
