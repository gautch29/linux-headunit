# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Exercise the real playback checker through its explicit silent route."""
import json
import subprocess
import sys


def main():
    executable = sys.argv[1]
    invalid = [
        [], ["--sink"], ["--sink", "autoaudiosink"],
        ["--sink", "alsasink"], ["--sink", "pulsesink"],
        ["--sink", "fakesink", "--device", "null"],
        ["--sink", "fakesink", "--device", ""],
        ["--sink", "fakesink", "--seconds", "0"],
        ["--sink", "fakesink", "--seconds", "11"],
        ["--sink", "fakesink", "--seconds", "1.5"],
        ["--sink", "fakesink", "--volume", "100"],
        ["--sink", "fakesink", "--volume", "-1"],
        ["--sink", "fakesink", "--sink", "alsasink"],
        ["--sink", "fakesink", "--unknown", "1"],
    ]
    for args in invalid:
        result = subprocess.run([executable, *args], capture_output=True, text=True, timeout=3)
        assert result.returncode != 0, (args, result.stdout)
        assert not result.stdout, (args, result.stdout)
        assert "audio output check:" in result.stderr, (args, result.stderr)
    result = subprocess.run(
        [executable, "--sink", "fakesink", "--seconds", "1"],
        capture_output=True, text=True, timeout=8,
    )
    assert result.returncode == 0, result.stderr
    receipt = json.loads(result.stdout)
    assert receipt["submitted_blocks"] == 200, receipt
    assert receipt["sink_pad_buffers"] == 200, receipt
    assert receipt["sink_pad_peak_available"] == 200, receipt
    assert receipt["sink_pad_zero_buffers"] == 25, receipt
    assert receipt["sink_pad_nonzero_buffers"] == 175, receipt
    assert 1600 <= receipt["sink_pad_maximum_peak"] <= 1650, receipt
    assert receipt["discontinuities"] == 2, receipt
    assert receipt["schedule_underruns"] == 1, receipt
    assert receipt["volume_percent"] == 5, receipt
    assert receipt["drained"] and not receipt["audible_verified"], receipt
    print("Bounded silent playback, mute, gain, pause/resume and explicit-route rejection pass.")


if __name__ == "__main__":
    main()
