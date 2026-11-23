#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Original numeric synthetic traces exercise the production offline parser."""
import copy
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import pcm_trace_audit as pcm

# Literal producer framing, not imported from parser's accepted-key catalogue.
HEADER = {"schema": 2, "clock": "CLOCK_MONOTONIC", "unavailable": 18446744073709551615,
          "collectorPid": 210, "parentPid": 200, "socketSendBufferBytes": 65536,
          "socketReceiveBufferBytes": 65536,
          "columns": ["dateNs", "kind", "source", "route", "epoch", "a", "b", "c", "d"]}
U = 18446744073709551615


def row(date, kind, source=2, route=1, epoch=1, a=U, b=U, c=U, d=U):
    return [date, kind, source, route, epoch, a, b, c, d]


def encoded(value):
    return json.dumps(value, separators=(",", ":")).encode() + b"\n"


def trace(events, losses=None, footer=True, header=None):
    raw = encoded(HEADER if header is None else header) + b"".join(encoded(e) for e in events)
    if footer:
        loss = {"contention": 0, "overflow": 0, "budget": 0, "lifecycleOverflow": 0,
                "transportDroppedEvents": 0, "transportDroppedBatches": 0}
        loss.update(losses or {})
        raw += encoded({"recorded": len(events) + loss["transportDroppedEvents"], **loss,
            "writtenEvents": len(events), "encodingNs": 23, "writeNs": 31,
            "writeCalls": 1 + bool(events), "writtenBytes": len(raw), "finished": True})
    return raw


class PcmAuditTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "trace.jsonl"

    def run_trace(self, data, phases=None):
        self.path.write_bytes(data)
        return pcm.audit(self.path, phases)

    def reject(self, data):
        self.path.write_bytes(data)
        with self.assertRaises(pcm.AuditError):
            pcm.audit(self.path)

    def test_original_line_hash_dates_and_unavailable(self):
        raw = trace([row(101, 23, a=U, b=10, c=3840, d=0), row(99, 1, a=7, b=200, c=1, d=0)])
        result = self.run_trace(raw)
        self.assertEqual(result["input"]["sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(result["coverage"], "finished_no_reported_loss")
        self.assertEqual(result["fileDateReversals"], 1)
        kinds = result["groups"][0]["kinds"]
        e = kinds["decoded_buffer"]["first"]
        self.assertEqual((e["line"], e["byteOffset"], e["dateNs"]), (2, len(encoded(HEADER)), 101))
        self.assertIsNone(e["original"][5])
        self.assertIsNone(e["values"]["a"])
        self.assertIsNone(result["collectorReaped"])
        self.assertEqual(result["continuousProducerCoverage"], "unknown")

    def test_every_kind_and_exact_clock_semantics(self):
        result = self.run_trace(trace([row(100 + k, k) for k in range(1, 36)]))
        kinds = result["groups"][0]["kinds"]
        self.assertEqual(len(kinds), 35)
        for name, expected in (("datagram", "untrustedSampleTime"),
                               ("authenticated", "unused"), ("decoder_submit", "submittedCount"),
                               ("jitter_enter", "gstPts"), ("output_accept", "freshOrigin"),
                               ("output_exhausted", "exhaustionCount")):
            self.assertEqual(kinds[name]["first"]["fieldSemantics"]["d"], expected)
        self.assertEqual(kinds["focus_requested"]["first"]["fieldSemantics"]["a"], "requestCount")

    def test_lifecycle_first_equal_dates_and_old_queued_epoch(self):
        # Collector exports lifecycle before earlier queued hot rows. FLUSH
        # stops decoder epoch1; already admitted epoch1 PCM pops legitimately.
        events = [row(120, 4), row(121, 3, epoch=2), row(122, 9, epoch=2),
                  row(110, 28, a=1, b=1), row(123, 30, a=0, b=6),
                  row(124, 25, epoch=2, a=0, b=0), row(124, 25, epoch=2, a=1, b=0),
                  row(130, 33, source=0, epoch=0, a=80, b=70, c=0, d=1)]
        result = self.run_trace(trace(events))
        groups = {(g["source"], g["route"], g["epoch"]): g for g in result["groups"]}
        self.assertEqual(groups[2, 1, 1]["afterDecoderStop"], {"channel_pop": 1})
        self.assertEqual(groups[2, 1, 2]["kinds"]["pcm_pull"]["equalDateRows"], 1)
        context = result["exhaustionContexts"][0]["strictlyEarlierSameRouteBoundaries"]
        pulls = [c for c in context if c["event"]["kind"] == "pcm_pull"]
        self.assertEqual(pulls[0]["equalDateCandidates"], 2)
        self.assertTrue(pulls[0]["representativeOnly"])
        self.assertGreater(result["fileDateReversals"], 0)
        self.assertGreater(result["fileEpochReversals"], 0)

    def test_crossroute_epoch_no_clock_mapping_and_empty_observations(self):
        result = self.run_trace(trace([
            row(10, 32, source=0, epoch=0, a=5_000_000_000, b=4_900_000_000, c=1, d=1),
            row(11, 14, route=7, a=1200), row(12, 25, epoch=2, a=0, b=0),
            row(13, 25, epoch=2, a=1, b=0),
            row(13, 33, source=0, epoch=0, a=9999, b=9990, c=0, d=1)]))
        context = result["exhaustionContexts"][0]
        self.assertEqual({c["event"]["route"] for c in context["strictlyEarlierSameRouteBoundaries"]}, {1})
        # Equal-date pull cannot be "earlier"; running time isn't host date.
        pull = next(c for c in context["strictlyEarlierSameRouteBoundaries"] if c["event"]["kind"] == "pcm_pull")
        self.assertEqual((pull["event"]["dateNs"], pull["observedGapNs"]), (12, 1))
        accepted = next(c for c in context["strictlyEarlierSameRouteBoundaries"] if c["event"]["kind"] == "output_accept")
        self.assertEqual(accepted["observedGapNs"], 3)
        self.assertEqual(accepted["event"]["values"]["a"], 5_000_000_000)

    def test_gaps_separate_source_epoch_and_actual_line_endpoints(self):
        result = self.run_trace(trace([row(30, 14), row(10, 14), row(30, 14),
                                      row(500, 14, epoch=2), row(1000, 14, source=3)]))
        first = next(g for g in result["groups"] if g["source"] == 2 and g["epoch"] == 1)
        metric = first["kinds"]["datagram"]
        self.assertEqual(metric["maximumObservedGapNs"], 20)
        self.assertEqual([e["dateNs"] for e in metric["maximumGapEndpoints"]], [10, 30])
        self.assertEqual(metric["equalDateRows"], 1)

    def test_retired_stream_route_channel_rows_never_revive_state(self):
        result = self.run_trace(trace([row(10, 2), row(11, 31),
            row(12, 6, source=0, epoch=0), row(13, 14), row(14, 28),
            row(15, 32, source=0, epoch=0), row(16, 1, source=3, route=7)]))
        old = next(g for g in result["groups"] if g["source"] == 2)
        self.assertEqual(old["afterChannelDetach"]["supply"], 1)
        self.assertEqual(old["afterStreamStop"]["datagram"], 1)
        self.assertEqual(old["afterRouteStop"]["datagram"], 1)
        new = next(g for g in result["groups"] if g["route"] == 7)
        self.assertEqual(new["afterStreamStop"], {})

    def test_each_loss_and_missing_footer_unknown_not_zero(self):
        for key in ("contention", "overflow", "budget", "lifecycleOverflow",
                    "transportDroppedEvents", "transportDroppedBatches"):
            with self.subTest(key=key):
                result = self.run_trace(trace([], losses={key: 1}))
                self.assertEqual(result["coverage"], "incomplete_reported_loss")
                self.assertEqual(result["loss"][key], 1)
        result = self.run_trace(trace([], footer=False))
        self.assertEqual(result["coverage"], "unknown_partial")
        self.assertIsNone(result["loss"])
        self.assertIsNone(result["footer"])

    def test_partial_tail_preserved_hash_no_data_echo(self):
        raw = trace([row(10, 13)], footer=False) + b'[11,14,2,1,1,1200'
        result = self.run_trace(raw)
        self.assertEqual(result["events"], 1)
        self.assertEqual(result["partialTail"]["line"], 3)
        self.assertEqual(result["partialTail"]["sha256"], hashlib.sha256(b'[11,14,2,1,1,1200').hexdigest())
        self.assertEqual(result["coverage"], "unknown_partial")
        # Valid JSON without the producer's newline is still an uncommitted tail.
        self.assertIsNone(self.run_trace(trace([])[:-1])["footer"])
        self.reject(trace([], footer=False) + b'[broken]\n')

    def test_strict_parser_security_and_uint64(self):
        for invalid in (b'[-1,14,2,1,1,0,0,0,0]\n', b'[true,14,2,1,1,0,0,0,0]\n',
                        b'[1.0,14,2,1,1,0,0,0,0]\n', b'[1,14,2,1,1,null,0,0,0]\n',
                        b'[1,99,2,1,1,0,0,0,0]\n', b'[1,14,2,1,1,18446744073709551616,0,0,0]\n',
                        b'[1,14,2,1,1,NaN,0,0,0]\n', b'[[[[0]]]]\n',
                        b'{"x":0,"x":1}\n', b'"' + b'x' * 129 + b'"\n'):
            with self.subTest(invalid=invalid[:20]):
                self.reject(encoded(HEADER) + invalid)
        self.reject(encoded(HEADER) + b'x' * 4097)
        self.reject(encoded(HEADER) + b'\n')
        header = copy.deepcopy(HEADER)
        header["clock"] = "CLOCK_REALTIME"
        self.reject(trace([], header=header))
        header = copy.deepcopy(HEADER)
        header["columns"][0] = "originaldateNs"
        self.reject(trace([], header=header))

    def test_footer_conservation_bytes_records_and_no_append(self):
        good = trace([row(10, 14)])
        footer = json.loads(good.splitlines()[-1])
        before = b"\n".join(good.splitlines()[:-1]) + b"\n"
        for key, value in (("writtenEvents", 2), ("recorded", 0), ("writtenBytes", 0), ("finished", False)):
            modified = {**footer, key: value}
            self.reject(before + encoded(modified))
        self.reject(good + encoded(row(11, 14)))

    def test_finished_footer_unaccounted_records_keep_coverage_incomplete(self):
        raw = trace([row(10, 14)])
        footer = json.loads(raw.splitlines()[-1])
        prefix = b"\n".join(raw.splitlines()[:-1]) + b"\n"
        footer["recorded"] = 8194
        result = self.run_trace(prefix + encoded(footer))
        self.assertEqual(result["unaccountedRecordedEvents"], 8193)
        self.assertEqual(result["coverage"], "incomplete_unaccounted_records")
        footer.update({"transportDroppedEvents": 64, "transportDroppedBatches": 1})
        result = self.run_trace(prefix + encoded(footer))
        self.assertEqual(result["unaccountedRecordedEvents"], 8129)
        self.assertEqual(result["coverage"], "incomplete_reported_loss")

    def test_file_admission_symlink_fifo_directory_and_size(self):
        self.path.write_bytes(trace([]))
        link = self.path.with_name("link")
        link.symlink_to(self.path)
        with self.assertRaises(OSError):
            pcm.audit(link)
        fifo = self.path.with_name("fifo")
        os.mkfifo(fifo)
        for path in (fifo, Path(self.directory.name)):
            with self.assertRaises(pcm.AuditError):
                pcm.audit(path)
        with self.path.open("wb") as file:
            file.truncate(53_000_001)
        with self.assertRaises(pcm.AuditError):
            pcm.audit(self.path)

    def test_actual_file_mutation_and_path_replacement(self):
        self.path.write_bytes(trace([]))
        original = pcm.parse_json
        def mutate(raw):
            parsed = original(raw)
            with self.path.open("ab") as file:
                file.write(b' ')
            return parsed
        with patch.object(pcm, "parse_json", mutate):
            with self.assertRaises(pcm.AuditError):
                pcm.audit(self.path)
        self.path.write_bytes(trace([]))
        def replace(raw):
            parsed = original(raw)
            successor = self.path.with_name("successor")
            successor.write_bytes(trace([]))
            os.replace(successor, self.path)
            return parsed
        with patch.object(pcm, "parse_json", replace):
            with self.assertRaises(pcm.AuditError):
                pcm.audit(self.path)

    def test_event_and_group_bounds(self):
        with patch.object(pcm, "MAX_EVENTS", 2):
            self.reject(trace([row(10, 14), row(11, 14), row(12, 14)]))
        self.reject(trace([row(i + 1, 14, source=i + 1) for i in range(129)]))

    def test_bound_context_retains_omission_count(self):
        events = [row(i + 1, 14, source=i + 1) for i in range(70)]
        events += [row(100 + i, 33, source=0, epoch=0) for i in range(130)]
        result = self.run_trace(trace(events))
        self.assertEqual(len(result["exhaustionContexts"]), 128)
        self.assertEqual(result["omittedExhaustionContexts"], 2)
        self.assertEqual(len(result["exhaustionContexts"][0]["strictlyEarlierSameRouteBoundaries"]), 64)
        self.assertEqual(result["exhaustionContexts"][0]["omittedContextBoundaries"], 6)

    def test_original_hash_bound_halfopen_phases_and_unknown_coverage(self):
        raw = trace([row(10, 14), row(20, 14), row(30, 14)])
        phases_path = self.path.with_name("phases.json")
        phases = {"traceSha256": hashlib.sha256(raw).hexdigest(), "clock": "CLOCK_MONOTONIC",
                  "parentPid": 200, "sameHostClockConfirmed": True,
                  "phases": [{"label": "off", "startNs": 10, "endNs": 20},
                             {"label": "on", "startNs": 20, "endNs": 30}]}
        phases_path.write_bytes(encoded(phases))
        result = self.run_trace(raw, phases_path)
        self.assertEqual([p["observedKindCounts"] for p in result["phases"]], [{"datagram": 1}, {"datagram": 1}])
        self.assertEqual(result["phases"][1]["phaseCoverage"], "unknown")
        for key, value in (("traceSha256", "0" * 64), ("parentPid", 201),
                           ("clock", "NTP"), ("sameHostClockConfirmed", False)):
            phases_path.write_bytes(encoded({**phases, key: value}))
            with self.assertRaises(pcm.AuditError):
                pcm.audit(self.path, phases_path)
        phases["phases"][1]["startNs"] = 19
        phases_path.write_bytes(encoded(phases))
        with self.assertRaises(pcm.AuditError):
            pcm.audit(self.path, phases_path)

    def test_exclusive_output_and_failed_write_retirement(self):
        output = self.path.with_name("report.json")
        report = self.run_trace(trace([]))
        pcm.write_report(output, report)
        self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
        before = output.read_bytes()
        with self.assertRaises(FileExistsError):
            pcm.write_report(output, report)
        self.assertEqual(output.read_bytes(), before)
        output.unlink()
        with patch.object(pcm, "MAX_REPORT_BYTES", 10):
            with self.assertRaises(pcm.AuditError):
                pcm.write_report(output, report)
        self.assertFalse(output.exists())
        output.symlink_to(self.path)
        with self.assertRaises(OSError):
            pcm.write_report(output, report)
        self.assertEqual(self.path.read_bytes(), trace([]))

    def test_maximum_producer_record_budget(self):
        # Exercise the real 262272 bound, not just a lowered test limit. This
        # also checks that the last original line remains attributable.
        self.path.write_bytes(encoded(HEADER))
        with self.path.open("ab") as file:
            for i in range(262272):
                file.write(encoded(row(i + 1, 14, a=U, b=U, c=U, d=U)))
        result = pcm.audit(self.path)
        self.assertEqual(result["events"], 262272)
        metric = result["groups"][0]["kinds"]["datagram"]
        self.assertEqual(metric["last"]["line"], 262273)
        self.assertEqual(metric["maximumObservedGapNs"], 1)
        with self.path.open("ab") as file:
            file.write(encoded(row(262273, 14)))
        with self.assertRaises(pcm.AuditError):
            pcm.audit(self.path)

    def test_output_failure_does_not_delete_replaced_artifact(self):
        output = self.path.with_name("output.json")
        replacement = self.path.with_name("replacement.json")
        replacement.write_bytes(b"other-owner-artifact")
        report = self.run_trace(trace([]))
        def fail(_):
            os.replace(replacement, output)
            raise OSError("fixture I/O failure")
        with patch.object(pcm.os, "fsync", fail):
            with self.assertRaises(OSError):
                pcm.write_report(output, report)
        self.assertEqual(output.read_bytes(), b"other-owner-artifact")

    def test_cli_sanitized_error_and_partial_success(self):
        self.path.write_bytes(trace([], footer=False))
        output = self.path.with_name("report.json")
        command = [sys.executable, "-B", str(ROOT / "tools/pcm_trace_audit.py"),
                   str(self.path), "--output", str(output)]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0)
        self.assertIn("coverage=unknown_partial", result.stdout)
        self.path.write_bytes(encoded(HEADER) + b'"PRIVATE_SENTINEL_PAYLOAD"\n')
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("PRIVATE_SENTINEL_PAYLOAD", result.stderr)
        self.assertNotIn(str(self.path), result.stderr)


if __name__ == "__main__":
    unittest.main()
