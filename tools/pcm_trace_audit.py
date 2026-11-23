#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Bounded offline audit of original numeric audio_observation schema 2.

No capture, runtime control, clock conversion, playout or causal classification.
Reports and input are private artifacts; the CLI creates a new 0600 report.
"""
from __future__ import annotations

import argparse
from array import array
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

U64 = (1 << 64) - 1
MAX_BYTES = 53_000_000
MAX_EVENTS = 262_272
MAX_LINE = 4096
MAX_GROUPS = 128
MAX_CONTEXTS = 128
MAX_CONTEXT_BOUNDARIES = 64
MAX_REPORT_BYTES = 32_000_000
MAX_PHASES = 16
COLUMNS = ["dateNs", "kind", "source", "route", "epoch", "a", "b", "c", "d"]
PRODUCER = "d544236ef00c75988975528bc70b828741d69454"
# Exact Point::observe call sites: stream, decoder, shared playback, output.
# All dates are host CLOCK_MONOTONIC observations; field clocks stay separate.
FIELDS = {
    1: ("stream_start", "format latencyMs media alternate"),
    2: ("stream_stop", "authenticatedCount decodedBlocks appliedFlushes fault"),
    3: ("decoder_start", "format latencyMs appsrcByteCap appsinkBufferCap"),
    4: ("decoder_stop", "submitted inputBackpressure discardedPcmFrames fault"),
    5: ("route_start", "startupReserveNs admissionHorizonNs appsrcByteCap unused"),
    6: ("route_stop", "acceptedBlocks exhaustionCount discontinuities fault"),
    7: ("flush_requested", "resumeSequence requestCount unused unused"),
    8: ("flush_begin", "resumeSequence pendingPcm lastSequence unused"),
    9: ("flush_applied", "resumeSequence unused unused unused"),
    10: ("flush_skipped", "resumeSequence lastSequence unused unused"),
    11: ("resource", "enabled configuredCount unused unused"),
    12: ("focus", "configuredCount gainMillionths durationNs unused"),
    13: ("receive_turn", "pendingPcm unused unused unused"),
    14: ("datagram", "receivedBytes recvmsgFlags untrustedSequence untrustedSampleTime"),
    15: ("authenticated", "sequence originalSampleTime rtpBytes unused"),
    16: ("rejected", "receivedBytes unused unused unused"),
    17: ("truncated", "receivedBytes unused unused unused"),
    18: ("pre_resume", "sequence requiredResumeSequence unused unused"),
    19: ("decoder_submit", "sequence originalSampleTime rtpBytes submittedCount"),
    20: ("decoder_reject", "reason rtpBytes queuedBytes unused"),
    21: ("jitter_enter", "sequence decoderRtpTime ssrc gstPts"),
    22: ("jitter_release", "sequence decoderRtpTime ssrc gstPts"),
    23: ("decoded_buffer", "gstPts gstDuration bytes flags"),
    24: ("decoded_pull", "gstPts gstDuration bytes flags"),
    25: ("pcm_pull", "emittedBlocks localPcmFrames unused unused"),
    26: ("jitter_counts", "pushed consideredLost late duplicate"),
    27: ("decoder_counts", "submitted discardedPcmFrames invalidPcmBuffers appsinkDropCount"),
    # Schema does NOT encode whether supply is shared or direct. Never choose
    # one merely because a channel_pop diagnostic is present/absent elsewhere.
    28: ("supply", "sharedQueuedBlocks_or_directAccepted enabled_or_directPending unused unused"),
    29: ("supply_blocked", "sharedQueuedBlocks_or_directAccepted unused_or_directPending unused unused"),
    30: ("channel_pop", "remainingQueuedBlocks outputAcceptedBlocks unused unused"),
    31: ("channel_detach", "withdrawnQueuedBlocks outputAcceptedBlocks unused unused"),
    32: ("output_accept", "scheduledGstPts outputRunningTime acceptedCount freshOrigin"),
    33: ("output_exhausted", "outputRunningTime exhaustedScheduleEnd appsrcBytes exhaustionCount"),
    34: ("resource_requested", "enabled requestCount unused unused"),
    35: ("focus_requested", "requestCount gainMillionths durationNs unused"),
}
FOOTER_NUMBERS = {
    "recorded", "contention", "overflow", "budget", "lifecycleOverflow",
    "transportDroppedEvents", "transportDroppedBatches", "writtenEvents",
    "encodingNs", "writeNs", "writeCalls", "writtenBytes",
}
LOSSES = ("contention", "overflow", "budget", "lifecycleOverflow",
          "transportDroppedEvents", "transportDroppedBatches")
HEADER_KEYS = {"schema", "clock", "unavailable", "collectorPid", "parentPid",
               "socketSendBufferBytes", "socketReceiveBufferBytes", "columns"}
CONTEXT_KINDS = {13, 14, 15, 19, 20, 22, 24, 25, 28, 29, 30, 31, 32,
                 7, 8, 9, 10, 11, 12, 34, 35}


class AuditError(ValueError):
    """Safe fixed error, without private content or path interpolation."""


def require(condition, reason):
    if not condition:
        raise AuditError(reason)


def uint(value):
    return type(value) is int and 0 <= value <= U64


def _integer(token):
    require(len(token) <= 20 and token.isascii() and token.isdecimal(), "integer bound")
    result = int(token)
    require(uint(result), "uint64 bound")
    return result


def _invalid_number(_):
    raise AuditError("noninteger number")


def _object(pairs):
    require(len(pairs) <= 16, "object field bound")
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate field")
        result[key] = value
    return result


def parse_json(raw):
    """Reject depth/token excess before the stdlib parser allocates a tree."""
    require(len(raw) <= MAX_LINE, "line bound")
    try:
        text = raw.decode("ascii")
        depth = 0
        quoted = False
        escaped = False
        string_length = 0
        for char in text:
            if quoted:
                string_length += 1
                require(string_length <= 128, "string bound")
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
                string_length = 0
            elif char in "[{":
                depth += 1
                require(depth <= 3, "JSON depth bound")
            elif char in "]}":
                depth -= 1
        return json.loads(text, parse_int=_integer, parse_float=_invalid_number,
                          parse_constant=_invalid_number, object_pairs_hook=_object)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise AuditError("invalid numeric JSON") from error


def signature(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns,
            info.st_ctime_ns, info.st_nlink)


class StableFile:
    """Bounded, nonblocking admission; no-follow leaf, regular fd, two hashes."""
    def __init__(self, path, maximum):
        self.path = Path(path)
        self.maximum = maximum
        self.fd = -1

    def __enter__(self):
        try:
            self.fd = os.open(self.path, os.O_RDONLY | os.O_CLOEXEC |
                              os.O_NOFOLLOW | os.O_NONBLOCK)
            self.before = os.fstat(self.fd)
            require(stat.S_ISREG(self.before.st_mode), "input must be regular")
            require(self.before.st_size <= self.maximum, "file byte bound")
            require(signature(os.lstat(self.path)) == signature(self.before), "input replaced")
            self.stream = os.fdopen(self.fd, "rb")
            self.fd = -1
            return self
        except BaseException:
            if self.fd >= 0:
                os.close(self.fd)
            raise

    def verify(self, digest):
        require(signature(os.fstat(self.stream.fileno())) == signature(self.before), "input mutated")
        self.stream.seek(0)
        hashed = hashlib.sha256()
        total = 0
        while chunk := self.stream.read(65536):
            total += len(chunk)
            require(total <= self.maximum, "file byte bound")
            hashed.update(chunk)
        require(hashed.hexdigest() == digest, "input hash changed")
        require(signature(os.fstat(self.stream.fileno())) == signature(self.before), "input mutated")
        require(signature(os.lstat(self.path)) == signature(self.before), "input replaced")
        return {"sha256": digest, "bytes": total, "device": self.before.st_dev,
                "inode": self.before.st_ino, "mtimeNs": self.before.st_mtime_ns,
                "ctimeNs": self.before.st_ctime_ns, "stableTwoPass": True}

    def __exit__(self, *_):
        self.stream.close()


def validate_header(row):
    require(type(row) is dict and set(row) == HEADER_KEYS, "schema2 header required")
    require(row["schema"] == 2 and type(row["schema"]) is int, "schema2 required")
    require(row["clock"] == "CLOCK_MONOTONIC" and row["columns"] == COLUMNS,
            "clock or columns unsupported")
    require(type(row["unavailable"]) is int and row["unavailable"] == U64, "sentinel unsupported")
    for key in ("collectorPid", "parentPid", "socketSendBufferBytes", "socketReceiveBufferBytes"):
        require(uint(row[key]) and row[key] > 0, "header integer invalid")
    require(row["parentPid"] > 1 and row["collectorPid"] > 1, "header PID invalid")
    for key in ("socketSendBufferBytes", "socketReceiveBufferBytes"):
        require(4688 <= row[key] <= 65536, "socket capacity unsupported")


def read_trace(path):
    # Eleven packed uint64s per record: nine producer fields + original line,
    # byte offset. No Python tree or per-record dict is retained.
    rows = array("Q")
    groups = set()
    header = footer = tail = None
    hashed = hashlib.sha256()
    offset = 0
    footer_offset = None
    with StableFile(path, MAX_BYTES) as file:
        line = 0
        while raw := file.stream.readline(MAX_LINE + 1):
            line += 1
            require(len(raw) <= MAX_LINE, "line bound")
            require(offset + len(raw) <= MAX_BYTES, "file byte bound")
            hashed.update(raw)
            require(footer is None, "rows after footer")
            if not raw.endswith(b"\n"):
                require(header is not None, "partial header")
                # Even a syntactically complete last row without its producer
                # newline is an uncommitted tail, never a finished footer.
                tail = {"line": line, "byteOffset": offset, "bytes": len(raw),
                        "sha256": hashlib.sha256(raw).hexdigest()}
                offset += len(raw)
                break
            row = parse_json(raw)
            if header is None:
                validate_header(row)
                header = row
            elif type(row) is list:
                require(len(row) == 9 and all(uint(value) for value in row), "event must be nine uint64s")
                require(0 < row[0] < U64 and row[1] in FIELDS, "event date/kind unsupported")
                require(all(value < U64 for value in row[2:5]), "identity unavailable")
                require(len(rows) // 11 < MAX_EVENTS, "event count bound")
                groups.add(tuple(row[2:5]))
                require(len(groups) <= MAX_GROUPS, "source/route/epoch group bound")
                rows.extend(row + [line, offset])
            else:
                require(type(row) is dict and set(row) == FOOTER_NUMBERS | {"finished"}, "footer unsupported")
                require(all(uint(row[key]) for key in FOOTER_NUMBERS), "footer integers invalid")
                require(row["finished"] is True, "finished footer required")
                require(row["writtenEvents"] == len(rows) // 11, "footer event count mismatch")
                require(row["recorded"] >= row["writtenEvents"] + row["transportDroppedEvents"],
                        "footer overaccounts recorded events")
                require(row["recorded"] <= MAX_EVENTS, "footer record bound")
                require(row["writtenBytes"] == offset, "footer byte count mismatch")
                footer = row
                footer_offset = offset
            offset += len(raw)
        require(header is not None, "header missing")
        identity = file.verify(hashed.hexdigest())
    return rows, header, footer, tail, identity, footer_offset


def event(rows, index):
    at = index * 11
    values = rows[at:at + 9]
    fields = FIELDS[values[1]][1].split()
    return {"line": rows[at + 9], "byteOffset": rows[at + 10],
            "dateNs": values[0], "kind": FIELDS[values[1]][0],
            "source": values[2], "route": values[3], "epoch": values[4],
            "values": {column: None if value == U64 else value
                       for column, value in zip("abcd", values[5:])},
            "fieldSemantics": dict(zip("abcd", fields)),
            "original": [None if value == U64 else value for value in values]}


def read_phases(path, identity, header):
    if path is None:
        return [], None
    # Same bounded JSON parser; phase document is a <=4096-byte single object.
    with StableFile(path, MAX_LINE) as file:
        raw = file.stream.read(MAX_LINE + 1)
        document = parse_json(raw)
        proof = file.verify(hashlib.sha256(raw).hexdigest())
    require(type(document) is dict and set(document) == {
        "traceSha256", "clock", "parentPid", "sameHostClockConfirmed", "phases"}, "phase contract invalid")
    require(document["traceSha256"] == identity["sha256"] and
            document["clock"] == header["clock"] and
            type(document["parentPid"]) is int and document["parentPid"] == header["parentPid"] and
            document["sameHostClockConfirmed"] is True, "phase source/clock binding required")
    phases = document["phases"]
    require(type(phases) is list and len(phases) <= MAX_PHASES, "phase count bound")
    previous = 0
    for phase in phases:
        require(type(phase) is dict and set(phase) == {"label", "startNs", "endNs"}, "phase fields invalid")
        require(phase["label"] in ("off", "on", "pause", "resume", "teardown", "reopen", "other"), "phase label unsupported")
        require(uint(phase["startNs"]) and uint(phase["endNs"]) and
                0 < phase["startNs"] < phase["endNs"] < U64 and
                phase["startNs"] >= previous, "phase dates invalid or overlapping")
        previous = phase["endNs"]
    return phases, proof


def summarize(rows):
    count = len(rows) // 11
    # Stable tie order is only a reproducible enumeration; never causality.
    order = sorted(range(count), key=lambda i: rows[i * 11])
    by_group = {}
    stops = {}
    file_reversals = 0
    epoch_reversals = 0
    previous_epochs = {}
    previous_date = 0
    for i in range(count):
        at = i * 11
        date, kind, source, route, epoch = rows[at:at + 5]
        file_reversals += date < previous_date
        previous_date = date
        owner = (source, route)
        if epoch and source:
            epoch_reversals += epoch < previous_epochs.get(owner, epoch)
            previous_epochs[owner] = epoch
        if kind in (2, 4, 6, 31):
            scope = (kind, source, route, epoch if kind == 4 else 0)
            stops[scope] = min(stops.get(scope, date), date)
    contexts = []
    omitted = 0
    pending = {}
    prior = {}
    # Prior boundary contexts are updated only after all equal-date records
    # have been inspected, so an equal date is never called "before".
    date_before = None
    for i in order:
        at = i * 11
        date, kind, source, route, epoch = rows[at:at + 5]
        key = (source, route, epoch)
        if date != date_before:
            prior.update(pending)
            pending = {}
            date_before = date
        group = by_group.setdefault(key, {"source": source, "route": route,
            "epoch": epoch, "kinds": {}, "afterDecoderStop": Counter(),
            "afterStreamStop": Counter(), "afterRouteStop": Counter(),
            "afterChannelDetach": Counter(), "atTerminalDates": Counter()})
        name = FIELDS[kind][0]
        item = group["kinds"].setdefault(name, {"count": 0, "first": event(rows, i),
            "last": None, "equalDateRows": 0, "maximumObservedGapNs": None,
            "maximumGapEndpoints": None, "lastDate": None,
            "zeroEmittedPulls": 0, "zeroRemainingQueuePops": 0,
            "zeroAppsrcBytesExhaustions": 0,
            "fieldRanges": {column: {"available": 0, "unavailable": 0, "minimum": None, "maximum": None}
                            for column in "abcd"}})
        item["count"] += 1
        for column, value in zip("abcd", rows[at + 5:at + 9]):
            extent = item["fieldRanges"][column]
            if value == U64:
                extent["unavailable"] += 1
            else:
                extent["available"] += 1
                extent["minimum"] = value if extent["minimum"] is None else min(value, extent["minimum"])
                extent["maximum"] = value if extent["maximum"] is None else max(value, extent["maximum"])
        last = item["lastDate"]
        if last is not None:
            gap = date - last
            if gap == 0:
                item["equalDateRows"] += 1
            elif item["maximumObservedGapNs"] is None or gap > item["maximumObservedGapNs"]:
                item["maximumObservedGapNs"] = gap
                item["maximumGapEndpoints"] = [item["last"], event(rows, i)]
        item["last"] = event(rows, i)
        item["lastDate"] = date
        item["zeroEmittedPulls"] += kind == 25 and rows[at + 5] == 0
        item["zeroRemainingQueuePops"] += kind == 30 and rows[at + 5] == 0
        item["zeroAppsrcBytesExhaustions"] += kind == 33 and rows[at + 7] == 0
        for stop_kind, scope, field in (
            (4, (source, route, epoch), "afterDecoderStop"),
            (2, (source, route, 0), "afterStreamStop"),
            (6, (0, route, 0), "afterRouteStop"),
            (31, (source, route, 0), "afterChannelDetach"),
        ):
            retired = stops.get((stop_kind, *scope))
            if retired is not None and date > retired:
                group[field][name] += 1
            elif retired is not None and date == retired:
                group["atTerminalDates"][name] += 1
        boundary = (*key, kind)
        pending[boundary] = (date, i, pending.get(boundary, (None, None, 0))[2] + 1)
        if kind == 33:
            if len(contexts) == MAX_CONTEXTS:
                omitted += 1
                continue
            before = []
            omitted_boundaries = 0
            for prior_key, (prior_date, index, equal_count) in sorted(prior.items()):
                if prior_key[1] == route and prior_key[3] in CONTEXT_KINDS:
                    if len(before) == MAX_CONTEXT_BOUNDARIES:
                        omitted_boundaries += 1
                        continue
                    before.append({"event": event(rows, index),
                                   "observedGapNs": date - prior_date,
                                   "equalDateCandidates": equal_count,
                                   "representativeOnly": equal_count > 1})
            contexts.append({"exhaustion": event(rows, i),
                             "strictlyEarlierSameRouteBoundaries": before,
                             "omittedContextBoundaries": omitted_boundaries})
    for group in by_group.values():
        for item in group["kinds"].values():
            del item["lastDate"]
    return list(by_group.values()), contexts, omitted, file_reversals, epoch_reversals


def audit(path, phases_path=None):
    rows, header, footer, tail, identity, footer_offset = read_trace(path)
    phases, phase_identity = read_phases(phases_path, identity, header)
    groups, contexts, omitted, reversals, epoch_reversals = summarize(rows)
    phase_results = []
    for phase in phases:
        counts = Counter()
        sources = set()
        for i in range(len(rows) // 11):
            at = i * 11
            if phase["startNs"] <= rows[at] < phase["endNs"]:
                counts[FIELDS[rows[at + 1]][0]] += 1
                sources.add(tuple(rows[at + 2:at + 5]))
        phase_results.append({**phase, "interval": "[startNs,endNs)",
            "observedKindCounts": counts, "observedGroups": sorted(sources),
            "phaseCoverage": "unknown", "firstLastRowsDoNotProveContinuousCoverage": True})
    loss = None if footer is None else {key: footer[key] for key in LOSSES}
    unaccounted = None if footer is None else (
        footer["recorded"] - footer["writtenEvents"] - footer["transportDroppedEvents"])
    coverage = "unknown_partial" if footer is None or tail is not None else (
        "incomplete_reported_loss" if any(loss.values()) else (
            "incomplete_unaccounted_records" if unaccounted else "finished_no_reported_loss"))
    return {"auditSchema": 1, "producerContractSource": PRODUCER,
        "producerSourceIdentityVerified": False, "input": identity, "header": header,
        "events": len(rows) // 11, "footer": footer, "footerByteOffset": footer_offset,
        "partialTail": tail, "loss": loss, "coverage": coverage,
        "unaccountedRecordedEvents": unaccounted,
        "collectorReaped": None, "continuousProducerCoverage": "unknown",
        "fileDateReversals": reversals, "fileEpochReversals": epoch_reversals, "groups": groups,
        "exhaustionContexts": contexts, "omittedExhaustionContexts": omitted,
        "phaseInput": phase_identity, "phases": phase_results,
        "limits": {"bytes": MAX_BYTES, "events": MAX_EVENTS, "lineBytes": MAX_LINE,
                   "groups": MAX_GROUPS, "exhaustionContexts": MAX_CONTEXTS,
                   "phases": MAX_PHASES, "contextBoundaries": MAX_CONTEXT_BOUNDARIES,
                   "reportBytes": MAX_REPORT_BYTES},
        "interpretation": [
            "Dates are original host CLOCK_MONOTONIC observations, not kernel arrivals.",
            "Gaps are between observed same-boundary/source/route/epoch rows; absence never proves no network or supply.",
            "Equal-date first/last/context representatives do not establish event order.",
            "Lifecycle rows can precede older hot rows; date sorting is observation order only.",
            "Queued channel_pop can retain an old decoder epoch after FLUSH/decoder_stop; no revival is inferred.",
            "Request/resource/focus rows are requests or worker configuration, not physical applied state.",
            "Supply a/b have shared/direct alternatives; schema2 does not encode the mode.",
            "RTP/decoder Gst PTS/output running time stay separate; no NTP/RTP/PTS mapping or packet-to-block join.",
            "output_accept is appsrc admission, exhaustion is local schedule exhaustion, neither is played/audible.",
            "Footer write cost/bytes/calls describe preceding writes; reaping requires a separate actual owner receipt.",
            "recorded minus writtenEvents minus transportDroppedEvents is unaccounted coverage, not an attributed loss cause.",
            "No phase coverage, causal fault or continuity pass follows from this audit.",
            "Missing lifecycle rows cannot certify active state; at-terminal-date counts establish no before/after order.",
        ]}


def write_report(path, report):
    # Exclusive private output; never overwrite original evidence or follow a
    # symlink. A failed write retires only this newly created artifact.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    created = os.fstat(fd)
    try:
        with os.fdopen(fd, "w", encoding="ascii") as file:
            size = 0
            for chunk in json.JSONEncoder(allow_nan=False, separators=(",", ":")).iterencode(report):
                size += len(chunk)
                require(size < MAX_REPORT_BYTES, "report byte bound")
                file.write(chunk)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
    except BaseException:
        try:
            current = os.lstat(path)
            if (current.st_dev, current.st_ino) == (created.st_dev, created.st_ino):
                os.unlink(path)
        except FileNotFoundError:
            pass
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="original private schema2 numeric JSONL")
    parser.add_argument("--phases", help="hash-bound original host-monotonic owner phases")
    parser.add_argument("--output", required=True, help="new private audit JSON (exclusive 0600)")
    args = parser.parse_args(argv)
    try:
        report = audit(args.trace, args.phases)
        write_report(args.output, report)
    except (AuditError, OSError) as error:
        reason = str(error) if isinstance(error, AuditError) else "private file operation failed"
        print("pcm-trace-audit: rejected: " + reason, file=sys.stderr)
        return 2
    print(f"pcm-trace-audit: events={report['events']} coverage={report['coverage']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
